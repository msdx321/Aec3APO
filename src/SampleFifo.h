//
// SampleFifo.h -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  Lock-free circular buffer (FIFO) for audio samples
//  Optimized for Single-Producer-Single-Consumer (SPSC) pattern
//

#pragma once

#include <vector>
#include <algorithm>
#include <atomic>

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
// - Init() and Reset() must be called when no concurrent access is happening
//
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier
struct SampleFifo
{
    std::vector<float> buffer;

    // Cache-line aligned atomics to prevent false sharing
    alignas(64) std::atomic<size_t> read{0};
    alignas(64) std::atomic<size_t> write{0};
    alignas(64) std::atomic<size_t> count{0};

    size_t capacity = 0;

    // Default constructor
    SampleFifo() = default;

    // Move constructor
    SampleFifo(SampleFifo &&other) noexcept
        : buffer(std::move(other.buffer)), read(other.read.load(std::memory_order_relaxed)), write(other.write.load(std::memory_order_relaxed)), count(other.count.load(std::memory_order_relaxed)), capacity(other.capacity)
    {
        other.read.store(0, std::memory_order_relaxed);
        other.write.store(0, std::memory_order_relaxed);
        other.count.store(0, std::memory_order_relaxed);
        other.capacity = 0;
    }

    // Move assignment operator
    SampleFifo &operator=(SampleFifo &&other) noexcept
    {
        if (this != &other)
        {
            buffer = std::move(other.buffer);
            read.store(other.read.load(std::memory_order_relaxed), std::memory_order_relaxed);
            write.store(other.write.load(std::memory_order_relaxed), std::memory_order_relaxed);
            count.store(other.count.load(std::memory_order_relaxed), std::memory_order_relaxed);
            capacity = other.capacity;

            other.read.store(0, std::memory_order_relaxed);
            other.write.store(0, std::memory_order_relaxed);
            other.count.store(0, std::memory_order_relaxed);
            other.capacity = 0;
        }
        return *this;
    }

    // Delete copy operations
    SampleFifo(const SampleFifo &) = delete;
    SampleFifo &operator=(const SampleFifo &) = delete;

    void Init(size_t cap)
    {
        buffer.assign(cap, 0.0f);
        capacity = cap;
        read.store(0, std::memory_order_relaxed);
        write.store(0, std::memory_order_relaxed);
        count.store(0, std::memory_order_relaxed);
    }

    void Reset()
    {
        read.store(0, std::memory_order_relaxed);
        write.store(0, std::memory_order_relaxed);
        count.store(0, std::memory_order_relaxed);
    }

    size_t Capacity() const { return capacity; }

    size_t Count() const
    {
        return count.load(std::memory_order_acquire);
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

        size_t currentWrite = write.load(std::memory_order_relaxed);
        size_t currentCount = count.load(std::memory_order_acquire);
        if (currentCount > capacity)
        {
            currentCount = capacity;
        }

        size_t freeSpace = capacity - currentCount;
        if (samples > freeSpace)
        {
            if (freeSpace == 0)
            {
                return;
            }
            data += (samples - freeSpace);
            samples = freeSpace;
        }

        // Batch copy with wrap-around handling
        size_t firstChunk = (std::min)(samples, capacity - currentWrite);
        std::copy_n(data, firstChunk, &buffer[currentWrite]);

        if (samples > firstChunk)
        {
            size_t secondChunk = samples - firstChunk;
            std::copy_n(data + firstChunk, secondChunk, &buffer[0]);
        }

        size_t newWrite = (currentWrite + samples) % capacity;
        write.store(newWrite, std::memory_order_release);
        count.fetch_add(samples, std::memory_order_acq_rel);
    }

    // Consumer only
    size_t Pop(float *out, size_t samples)
    {
        if (capacity == 0 || samples == 0)
        {
            return 0;
        }

        size_t currentRead = read.load(std::memory_order_relaxed);
        size_t available = count.load(std::memory_order_acquire);

        size_t toRead = (std::min)(samples, available);
        if (toRead == 0)
        {
            return 0;
        }

        // Batch copy with wrap-around handling
        size_t firstChunk = (std::min)(toRead, capacity - currentRead);
        std::copy_n(&buffer[currentRead], firstChunk, out);

        if (toRead > firstChunk)
        {
            size_t secondChunk = toRead - firstChunk;
            std::copy_n(&buffer[0], secondChunk, out + firstChunk);
        }

        size_t newRead = (currentRead + toRead) % capacity;
        read.store(newRead, std::memory_order_release);
        count.fetch_sub(toRead, std::memory_order_acq_rel);

        return toRead;
    }
};
#pragma warning(pop)
