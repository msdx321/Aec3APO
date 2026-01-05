//
// SampleFifo.h -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  Circular buffer (FIFO) for audio samples with thread-safe operations
//

#pragma once

#include <vector>
#include <algorithm>

//
// SampleFifo - A circular buffer for floating-point audio samples
//
// This FIFO provides:
// - Fixed capacity circular buffering
// - Automatic overflow handling (drops oldest samples when full)
// - Efficient modulo-based indexing
//
struct SampleFifo
{
    std::vector<float> buffer;
    size_t read = 0;
    size_t write = 0;
    size_t count = 0;

    // Default constructor
    SampleFifo() = default;

    // Move constructor
    SampleFifo(SampleFifo &&other) noexcept
        : buffer(std::move(other.buffer)), read(other.read), write(other.write), count(other.count)
    {
        other.read = 0;
        other.write = 0;
        other.count = 0;
    }

    // Move assignment operator
    SampleFifo &operator=(SampleFifo &&other) noexcept
    {
        if (this != &other)
        {
            buffer = std::move(other.buffer);
            read = other.read;
            write = other.write;
            count = other.count;

            other.read = 0;
            other.write = 0;
            other.count = 0;
        }
        return *this;
    }

    // Delete copy operations (use move semantics instead)
    SampleFifo(const SampleFifo &) = delete;
    SampleFifo &operator=(const SampleFifo &) = delete;

    //
    // Initialize the FIFO with specified capacity
    //
    void Init(size_t capacity)
    {
        buffer.assign(capacity, 0.0f);
        read = 0;
        write = 0;
        count = 0;
    }

    //
    // Reset the FIFO without deallocating memory
    //
    void Reset()
    {
        read = 0;
        write = 0;
        count = 0;
    }

    //
    // Get the maximum capacity of the FIFO
    //
    size_t Capacity() const { return buffer.size(); }

    //
    // Get the current number of samples in the FIFO
    //
    size_t Count() const { return count; }

    //
    // Push samples into the FIFO
    // If FIFO is full, oldest samples are dropped
    //
    void Push(const float *data, size_t samples)
    {
        if (buffer.empty() || samples == 0)
        {
            return;
        }

        const size_t capacity = buffer.size();

        // If input has more samples than capacity, only keep the most recent
        if (samples > capacity)
        {
            data += (samples - capacity);
            samples = capacity;
        }

        // Drop oldest samples if not enough room
        if (samples > capacity - count)
        {
            size_t drop = samples - (capacity - count);
            read = (read + drop) % capacity;
            count -= drop;
        }

        // Write samples to buffer
        for (size_t i = 0; i < samples; ++i)
        {
            buffer[write] = data[i];
            write = (write + 1) % capacity;
        }
        count += samples;
    }

    //
    // Pop samples from the FIFO
    // Returns the actual number of samples read (may be less than requested)
    //
    size_t Pop(float *out, size_t samples)
    {
        if (buffer.empty() || samples == 0)
        {
            return 0;
        }

        size_t to_read = (samples < count) ? samples : count;
        const size_t capacity = buffer.size();

        // Read samples from buffer
        for (size_t i = 0; i < to_read; ++i)
        {
            out[i] = buffer[read];
            read = (read + 1) % capacity;
        }
        count -= to_read;
        return to_read;
    }
};
