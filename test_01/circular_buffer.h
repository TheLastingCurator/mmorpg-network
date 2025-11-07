#pragma once

#include <cstdint>
#include <cstring>
#include <cassert>

/// @brief Circular buffer with 64-bit indices for network messages
/// Size must be power of 2 for efficient modulo operation using bit mask
class CircularBuffer {
public:
    /// @brief Constructor
    /// @param size_power_of_2 Buffer size, must be power of 2 (e.g., 1024, 2048, 4096)
    explicit CircularBuffer(size_t size_power_of_2) 
        : size_(size_power_of_2)
        , mask_(size_power_of_2 - 1)
        , read_pos_(0)
        , write_pos_(0) {
        // Check that size is power of 2
        assert((size_power_of_2 & (size_power_of_2 - 1)) == 0 && "Size must be power of 2");
        assert(size_power_of_2 > 0 && "Size must be greater than 0");
        
        buffer_ = new uint8_t[size_];
    }
    
    ~CircularBuffer() {
        delete[] buffer_;
    }
    
    // Disable copying
    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;
    
    /// @brief Write data to buffer
    /// @param data Pointer to data
    /// @param length Number of bytes to write
    /// @return Number of bytes actually written (may be less if buffer is full)
    size_t Write(const void* data, size_t length) {
        if (!data || length == 0) return 0;
        
        const uint8_t* src = static_cast<const uint8_t*>(data);
        size_t available_space = GetAvailableWriteSpace();
        size_t to_write = (length > available_space) ? available_space : length;
        
        if (to_write == 0) return 0;
        
        // Write data, considering possible buffer wrapping
        size_t write_index = write_pos_ & mask_;
        size_t first_chunk = size_ - write_index;
        
        if (to_write <= first_chunk) {
            // Data fits before buffer end
            std::memcpy(buffer_ + write_index, src, to_write);
        } else {
            // Data doesn't fit, need to split into two parts
            std::memcpy(buffer_ + write_index, src, first_chunk);
            std::memcpy(buffer_, src + first_chunk, to_write - first_chunk);
        }
        
        write_pos_ += to_write;
        return to_write;
    }
    
    /// @brief Read data from buffer
    /// @param data Pointer to buffer for reading
    /// @param length Maximum number of bytes to read
    /// @return Number of bytes actually read
    size_t Read(void* data, size_t length) {
        if (!data || length == 0) return 0;
        
        uint8_t* dst = static_cast<uint8_t*>(data);
        size_t available_data = GetAvailableReadData();
        size_t to_read = (length > available_data) ? available_data : length;
        
        if (to_read == 0) return 0;
        
        // Read data, considering possible buffer wrapping
        size_t read_index = read_pos_ & mask_;
        size_t first_chunk = size_ - read_index;
        
        if (to_read <= first_chunk) {
            // Data fits before buffer end
            std::memcpy(dst, buffer_ + read_index, to_read);
        } else {
            // Data doesn't fit, need to split into two parts
            std::memcpy(dst, buffer_ + read_index, first_chunk);
            std::memcpy(dst + first_chunk, buffer_, to_read - first_chunk);
        }
        
        read_pos_ += to_read;
        return to_read;
    }
    
    /// @brief Peek at data without removing it from buffer
    /// @param data Pointer to buffer for reading
    /// @param length Maximum number of bytes to read
    /// @return Number of bytes actually read
    size_t Peek(void* data, size_t length) const {
        if (!data || length == 0) return 0;
        
        uint8_t* dst = static_cast<uint8_t*>(data);
        size_t available_data = GetAvailableReadData();
        size_t to_read = (length > available_data) ? available_data : length;
        
        if (to_read == 0) return 0;
        
        // Read data without changing read_pos_
        size_t read_index = read_pos_ & mask_;
        size_t first_chunk = size_ - read_index;
        
        if (to_read <= first_chunk) {
            std::memcpy(dst, buffer_ + read_index, to_read);
        } else {
            std::memcpy(dst, buffer_ + read_index, first_chunk);
            std::memcpy(dst + first_chunk, buffer_, to_read - first_chunk);
        }
        
        return to_read;
    }
    
    /// @brief Skip data (equivalent to reading without copying)
    /// @param length Number of bytes to skip
    /// @return Number of bytes actually skipped
    size_t Skip(size_t length) {
        size_t available_data = GetAvailableReadData();
        size_t to_skip = (length > available_data) ? available_data : length;
        read_pos_ += to_skip;
        return to_skip;
    }
    
    /// @brief Get number of bytes available for reading
    size_t GetAvailableReadData() const {
        return write_pos_ - read_pos_;
    }
    
    /// @brief Get amount of free space for writing
    size_t GetAvailableWriteSpace() const {
        // Leave one byte free to distinguish between full and empty buffer
        return size_ - GetAvailableReadData() - 1;
    }
    
    /// @brief Check if buffer is empty
    bool IsEmpty() const {
        return read_pos_ == write_pos_;
    }
    
    /// @brief Check if buffer is full
    bool IsFull() const {
        return GetAvailableWriteSpace() == 0;
    }
    
    /// @brief Clear buffer
    void Clear() {
        read_pos_ = write_pos_ = 0;
    }
    
    /// @brief Get buffer size
    size_t GetSize() const {
        return size_;
    }
    
    /// @brief Get read position (64-bit)
    uint64_t GetReadPosition() const {
        return read_pos_;
    }
    
    /// @brief Get write position (64-bit)
    uint64_t GetWritePosition() const {
        return write_pos_;
    }
    
    /// @brief Get address and size of continuous data region in buffer
    /// @param out_ptr Pointer to store the address of continuous data
    /// @param out_size Pointer to store the size of continuous data
    /// @return true if there is data available, false if buffer is empty
    /// @note If data wraps around, only returns the first continuous chunk
    bool GetContinuousReadRegion(const uint8_t** out_ptr, size_t* out_size) const {
        if (!out_ptr || !out_size) return false;
        
        size_t available_data = GetAvailableReadData();
        if (available_data == 0) {
            *out_ptr = nullptr;
            *out_size = 0;
            return false;
        }
        
        size_t read_index = read_pos_ & mask_;
        size_t continuous_size = size_ - read_index;
        
        // If available data is less than space to end of buffer, 
        // return only available data
        if (available_data <= continuous_size) {
            *out_ptr = buffer_ + read_index;
            *out_size = available_data;
        } else {
            // Data wraps around, return only first chunk
            *out_ptr = buffer_ + read_index;
            *out_size = continuous_size;
        }
        
        return true;
    }
    
    /// @brief Get address and size of continuous free space in buffer
    /// @param out_ptr Pointer to store the address of continuous free space
    /// @param out_size Pointer to store the size of continuous free space
    /// @return true if there is free space available, false if buffer is full
    /// @note If free space wraps around, only returns the first continuous chunk
    bool GetContinuousWriteRegion(uint8_t** out_ptr, size_t* out_size) {
        if (!out_ptr || !out_size) return false;
        
        size_t available_space = GetAvailableWriteSpace();
        if (available_space == 0) {
            *out_ptr = nullptr;
            *out_size = 0;
            return false;
        }
        
        size_t write_index = write_pos_ & mask_;
        size_t continuous_size = size_ - write_index;
        
        // If available space is less than space to end of buffer,
        // return only available space
        if (available_space <= continuous_size) {
            *out_ptr = buffer_ + write_index;
            *out_size = available_space;
        } else {
            // Free space wraps around, return only first chunk
            *out_ptr = buffer_ + write_index;
            *out_size = continuous_size;
        }
        
        return true;
    }
    
    /// @brief Advance read position after direct read from continuous region
    /// @param bytes_read Number of bytes that were read directly
    /// @return true if successful, false if trying to advance beyond available data
    bool AdvanceReadPosition(size_t bytes_read) {
        if (bytes_read > GetAvailableReadData()) {
            return false;
        }
        read_pos_ += bytes_read;
        return true;
    }
    
    /// @brief Advance write position after direct write to continuous region
    /// @param bytes_written Number of bytes that were written directly
    /// @return true if successful, false if trying to advance beyond available space
    bool AdvanceWritePosition(size_t bytes_written) {
        if (bytes_written > GetAvailableWriteSpace()) {
            return false;
        }
        write_pos_ += bytes_written;
        return true;
    }

private:
    uint8_t* buffer_;           ///< Data buffer
    size_t size_;               ///< Buffer size (power of 2)
    size_t mask_;               ///< Bit mask for fast modulo (size_ - 1)
    uint64_t read_pos_;         ///< 64-bit read position
    uint64_t write_pos_;        ///< 64-bit write position
};
