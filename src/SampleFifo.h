//
// SampleFifo.h -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  Lock-free circular buffer (FIFO) for audio samples
//  Optimized for Single-Producer-Single-Consumer (SPSC) pattern
//

#pragma once

#include <algorithm>
#include <atomic>
#include <vector>

//
// SampleFifo - Lock-free circular buffer for floating-point audio samples
//
// Features:
// - Fixed capacity circular buffering
// - Lock-free thread-safe operations (SPSC pattern)
// - Overflow-safe writes (drops incoming samples when full)
// - Batch copy operations for improved performance
// - Proper memory ordering for x64 and ARM64
//
// Thread Safety:
// - Producer thread: Calls Push() only
// - Consumer thread: Calls Pop() only
// - Init() must be called when no concurrent access is happening
//
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier
class SampleFifo
{
public:
    SampleFifo() = default;

    SampleFifo(const SampleFifo &) = delete;
    SampleFifo &operator=(const SampleFifo &) = delete;

    void Init(size_t cap)
    {
        buffer.assign(cap, 0.0f);
        capacity = cap;
        read.store(0, std::memory_order_relaxed);
        write.store(0, std::memory_order_relaxed);
    }

    // Producer only
    void Push(const float *data, size_t samples)
    {
        if (capacity == 0 || samples == 0)
        {
            return;
        }

        if (samples > capacity)
        {
            data += (samples - capacity);
            samples = capacity;
        }

        const size_t currentWrite = write.load(std::memory_order_relaxed);
        const size_t currentRead = read.load(std::memory_order_acquire);
        const size_t used = UsedSamples(currentRead, currentWrite, capacity);
        const size_t freeSpace = capacity - used;
        const size_t writable = (std::min)(samples, freeSpace);
        if (writable == 0)
        {
            return;
        }
        data += samples - writable;

        // Batch copy with wrap-around handling
        const size_t writeOffset = currentWrite % capacity;
        CopyIntoCircularBuffer(buffer.data(), capacity, writeOffset, data, writable);

        write.store(currentWrite + writable, std::memory_order_release);
    }

    // Consumer only
    size_t Pop(float *out, size_t samples)
    {
        if (capacity == 0 || samples == 0)
        {
            return 0;
        }

        const size_t currentRead = read.load(std::memory_order_relaxed);
        const size_t currentWrite = write.load(std::memory_order_acquire);
        const size_t available = UsedSamples(currentRead, currentWrite, capacity);

        const size_t toRead = (std::min)(samples, available);
        if (toRead == 0)
        {
            return 0;
        }

        // Batch copy with wrap-around handling
        const size_t readOffset = currentRead % capacity;
        CopyFromCircularBuffer(out, buffer.data(), capacity, readOffset, toRead);

        read.store(currentRead + toRead, std::memory_order_release);

        return toRead;
    }

private:
    static size_t UsedSamples(size_t currentRead, size_t currentWrite, size_t maxCapacity)
    {
        if (currentWrite < currentRead)
        {
            return 0;
        }

        return (std::min)(currentWrite - currentRead, maxCapacity);
    }

    static void CopyIntoCircularBuffer(float *destination,
                                       size_t destinationCapacity,
                                       size_t destinationOffset,
                                       const float *source,
                                       size_t samples)
    {
        const size_t firstChunk = (std::min)(samples, destinationCapacity - destinationOffset);
        std::copy_n(source, firstChunk, destination + destinationOffset);

        if (samples > firstChunk)
        {
            const size_t secondChunk = samples - firstChunk;
            std::copy_n(source + firstChunk, secondChunk, destination);
        }
    }

    static void CopyFromCircularBuffer(float *destination,
                                       const float *source,
                                       size_t sourceCapacity,
                                       size_t sourceOffset,
                                       size_t samples)
    {
        const size_t firstChunk = (std::min)(samples, sourceCapacity - sourceOffset);
        std::copy_n(source + sourceOffset, firstChunk, destination);

        if (samples > firstChunk)
        {
            const size_t secondChunk = samples - firstChunk;
            std::copy_n(source, secondChunk, destination + firstChunk);
        }
    }

    std::vector<float> buffer;

    // Cache-line aligned atomics to prevent false sharing
    alignas(64) std::atomic<size_t> read{0};
    alignas(64) std::atomic<size_t> write{0};
    size_t capacity = 0;
};
#pragma warning(pop)
