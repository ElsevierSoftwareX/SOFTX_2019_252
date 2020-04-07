/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * FileDrainerSingleThread.cpp
 *
 *  Created on: April 1, 2020
 *      Author: Norbert Podhorszki <pnorbert@ornl.gov>
 */

#include "FileDrainerSingleThread.h"

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "../../common/ADIOSTypes.h"

/// \endcond

namespace adios2
{
namespace burstbuffer
{

FileDrainerSingleThread::FileDrainerSingleThread() : FileDrainer() {}

FileDrainerSingleThread::~FileDrainerSingleThread() { Join(); }

void FileDrainerSingleThread::SetBufferSize(size_t bufferSizeBytes)
{
    bufferSize = bufferSizeBytes;
}

void FileDrainerSingleThread::Start()
{
    th = std::thread(&FileDrainerSingleThread::DrainThread, this);
}

void FileDrainerSingleThread::Finish()
{
    finishMutex.lock();
    finish = true;
    finishMutex.unlock();
}

typedef std::chrono::duration<double> Seconds;
typedef std::chrono::time_point<
    std::chrono::steady_clock,
    std::chrono::duration<double, std::chrono::steady_clock::period>>
    TimePoint;

void FileDrainerSingleThread::Join()
{
    if (th.joinable())
    {
        const auto tTotalStart = std::chrono::steady_clock::now();
        Seconds timeTotal = Seconds(0.0);

        Finish();
        th.join();

        const auto tTotalEnd = std::chrono::steady_clock::now();
        timeTotal = tTotalEnd - tTotalStart;
        if (m_Verbose)
        {
            std::cout << "Drain " << m_Rank
                      << ": Waited for thread to join = " << timeTotal.count()
                      << " seconds" << std::endl;
        }
    }
}

/*
 * This function is running in a separate thread from all other member function
 * calls.
 */
void FileDrainerSingleThread::DrainThread()
{
    const auto tTotalStart = std::chrono::steady_clock::now();
    Seconds timeTotal = Seconds(0.0);
    Seconds timeSleep = Seconds(0.0);
    Seconds timeRead = Seconds(0.0);
    Seconds timeWrite = Seconds(0.0);
    Seconds timeClose = Seconds(0.0);
    TimePoint ts, te;
    size_t maxQueueSize = 0;
    std::vector<char> buffer; // fixed, preallocated buffer to read/write data
    buffer.resize(bufferSize);

    size_t nReadBytesTasked = 0;
    size_t nReadBytesSucc = 0;
    size_t nWriteBytesTasked = 0;
    size_t nWriteBytesSucc = 0;

    /* Copy a block of data from one file to another at the same offset */
    auto lf_Copy = [&](FileDrainOperation &fdo, int fdr, int fdw,
                       size_t count) {
        nReadBytesTasked += count;
        ts = std::chrono::steady_clock::now();
        size_t n = Read(fdr, count, buffer.data(), fdo.fromFileName);
        te = std::chrono::steady_clock::now();
        timeRead += te - ts;
        nReadBytesSucc += n;

        nWriteBytesTasked += count;
        ts = std::chrono::steady_clock::now();
        n = Write(fdw, count, buffer.data(), fdo.toFileName);
        te = std::chrono::steady_clock::now();
        timeWrite += te - ts;
        nWriteBytesSucc += n;
    };

    std::chrono::duration<double> d(0.100);

    while (true)
    {
        operationsMutex.lock();
        if (operations.empty())
        {
            operationsMutex.unlock();
            finishMutex.lock();
            bool done = finish;
            finishMutex.unlock();
            if (done)
            {
                break;
            }
            ts = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(d);
            te = std::chrono::steady_clock::now();
            timeSleep += te - ts;
            continue;
        }

        FileDrainOperation &fdo = operations.front();
        size_t queueSize = operations.size();
        if (queueSize > maxQueueSize)
        {
            maxQueueSize = queueSize;
        }
        operationsMutex.unlock();

        switch (fdo.op)
        {

        case DrainOperation::CopyAt:
        case DrainOperation::Copy:
        {
            ts = std::chrono::steady_clock::now();
            int fdr = GetFileDescriptor(fdo.fromFileName, Mode::Read);
            te = std::chrono::steady_clock::now();
            timeRead += te - ts;

            ts = std::chrono::steady_clock::now();
            Mode wMode =
                (fdo.op == DrainOperation::CopyAt ? Mode::Write : Mode::Append);
            int fdw = GetFileDescriptor(fdo.toFileName, wMode);
            te = std::chrono::steady_clock::now();
            timeWrite += te - ts;

            if (m_Verbose >= 2)
            {
                std::cout << "Drain " << m_Rank << ": Copy from "
                          << fdo.fromFileName << " (fd=" << fdr << ") -> "
                          << fdo.toFileName << " (fd=" << fdw << ") "
                          << fdo.countBytes << " bytes ";
                if (fdo.op == DrainOperation::CopyAt)
                {
                    std::cout << ", offsets: from " << fdo.fromOffset << " to "
                              << fdo.toOffset;
                }
                std::cout << std::endl;
            }

            if (fdr != errorState && fdw != errorState)
            {
                try
                {
                    if (fdo.op == DrainOperation::CopyAt)
                    {
                        ts = std::chrono::steady_clock::now();
                        Seek(fdr, fdo.fromOffset, fdo.fromFileName);
                        te = std::chrono::steady_clock::now();
                        timeRead += te - ts;

                        ts = std::chrono::steady_clock::now();
                        Seek(fdw, fdo.toOffset, fdo.toFileName);
                        te = std::chrono::steady_clock::now();
                        timeWrite += te - ts;
                    }
                    const size_t batches = fdo.countBytes / bufferSize;
                    const size_t remainder = fdo.countBytes % bufferSize;
                    for (size_t b = 0; b < batches; ++b)
                    {
                        lf_Copy(fdo, fdr, fdw, bufferSize);
                    }
                    if (remainder)
                    {
                        lf_Copy(fdo, fdr, fdw, remainder);
                    }
                }
                catch (std::ios_base::failure &e)
                {
                    std::cerr << "ADIOS THREAD ERROR: " << e.what()
                              << std::endl;
                }
            }
            break;
        }
        case DrainOperation::SeekEnd:
        {
            if (m_Verbose >= 2)
            {
                std::cout << "Drain " << m_Rank << ": Seek to End of file "
                          << fdo.toFileName << std::endl;
            }
            ts = std::chrono::steady_clock::now();
            int fdw = GetFileDescriptor(fdo.toFileName, Mode::Write);
            Seek(fdw, 0, fdo.toFileName, SEEK_END);
            te = std::chrono::steady_clock::now();
            timeWrite += te - ts;
            break;
        }
        case DrainOperation::WriteAt:
        {
            if (m_Verbose >= 2)
            {
                std::cout << "Drain " << m_Rank << ": Write to file "
                          << fdo.toFileName << " " << fdo.countBytes
                          << " bytes of data from memory to offset "
                          << fdo.toOffset << std::endl;
            }
            nWriteBytesTasked += fdo.countBytes;
            ts = std::chrono::steady_clock::now();
            int fdw = GetFileDescriptor(fdo.toFileName, Mode::Write);
            Seek(fdw, fdo.toOffset, fdo.toFileName);
            size_t n = Write(fdw, fdo.countBytes, fdo.dataToWrite.data(),
                             fdo.toFileName);
            te = std::chrono::steady_clock::now();
            timeWrite += te - ts;
            nWriteBytesSucc += n;
            break;
        }
        case DrainOperation::Write:
        {
            if (m_Verbose >= 2)
            {
                std::cout << "Drain " << m_Rank << ": Write to file "
                          << fdo.toFileName << " " << fdo.countBytes
                          << " bytes of data from memory (no seek)"
                          << std::endl;
            }
            nWriteBytesTasked += fdo.countBytes;
            ts = std::chrono::steady_clock::now();
            int fdw = GetFileDescriptor(fdo.toFileName, Mode::Write);
            size_t n = Write(fdw, fdo.countBytes, fdo.dataToWrite.data(),
                             fdo.toFileName);
            te = std::chrono::steady_clock::now();
            timeWrite += te - ts;
            nWriteBytesSucc += n;
            break;
        }
        case DrainOperation::Create:
        {
            if (m_Verbose >= 2)
            {
                std::cout << "Drain " << m_Rank << ": Create new file "
                          << fdo.toFileName << std::endl;
            }
            ts = std::chrono::steady_clock::now();
            GetFileDescriptor(fdo.toFileName, Mode::Write);
            te = std::chrono::steady_clock::now();
            timeWrite += te - ts;
            break;
        }
        case DrainOperation::Open:
        {
            if (m_Verbose >= 2)
            {
                std::cout << "Drain " << m_Rank << "Open file "
                          << fdo.toFileName << " for append " << std::endl;
            }
            ts = std::chrono::steady_clock::now();
            GetFileDescriptor(fdo.toFileName, Mode::Append);
            te = std::chrono::steady_clock::now();
            timeWrite += te - ts;
            break;
        }

        default:
            break;
        }
        operationsMutex.lock();
        operations.pop();
        operationsMutex.unlock();
    }

    ts = std::chrono::steady_clock::now();
    CloseAll();
    te = std::chrono::steady_clock::now();
    timeClose += te - ts;

    const auto tTotalEnd = std::chrono::steady_clock::now();
    timeTotal = tTotalEnd - tTotalStart;
    if (m_Verbose)
    {
        std::cout << "Drain " << m_Rank
                  << ": Runtime  total = " << timeTotal.count()
                  << " read = " << timeRead.count()
                  << " write = " << timeWrite.count()
                  << " close = " << timeClose.count()
                  << " sleep = " << timeSleep.count() << " seconds"
                  << ". Max queue size = " << maxQueueSize << ".";
        if (nReadBytesTasked == nReadBytesSucc)
        {
            std::cout << " Read " << nReadBytesSucc << " bytes";
        }
        else
        {
            std::cout << " WARNING Read wanted = " << nReadBytesTasked
                      << " but successfully read = " << nReadBytesSucc
                      << " bytes.";
        }
        if (nWriteBytesTasked == nWriteBytesSucc)
        {
            std::cout << " Wrote " << nWriteBytesSucc << " bytes";
        }
        else
        {
            std::cout << " WARNING Write wanted = " << nWriteBytesTasked
                      << " but successfully wrote = " << nWriteBytesSucc
                      << " bytes.";
        }
        std::cout << std::endl;
    }
}

} // end namespace burstbuffer
} // end namespace adios2
