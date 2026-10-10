#include "util/bits.h"
#include <stdexcept>

namespace aa3 {
namespace util {

BitReader::BitReader(const uint8_t* data, uint32_t bit_len)
    : data_(data), bit_len_(bit_len), pos_(0) {
}

BitReader BitReader::FromPacket(const uint8_t* data, uint32_t byte_len) {
    // UE3 BitWriter pads to byte boundary with a 1-bit terminator followed by
    // zero bits. To recover the real bit length, scan backwards for the last
    // set bit — that's the terminator. Real data is everything strictly before
    // it. An all-zero buffer yields zero bits.
    uint32_t real_bits = 0;
    for (int32_t i = static_cast<int32_t>(byte_len) - 1; i >= 0; --i) {
        if (data[i] != 0) {
            uint8_t b = data[i];
            int hi = 7;
            while (hi >= 0 && ((b >> hi) & 1) == 0) --hi;
            real_bits = static_cast<uint32_t>(i) * 8 + static_cast<uint32_t>(hi);
            break;
        }
    }
    return BitReader(data, real_bits);
}

bool BitReader::ReadBit() {
    if (pos_ >= bit_len_) throw std::out_of_range("BitReader out of bits");
    bool bit = (data_[pos_ >> 3] >> (pos_ & 7)) & 1;
    pos_++;
    return bit;
}

uint32_t BitReader::ReadBits(uint32_t count) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (ReadBit()) {
            value |= (1 << i);
        }
    }
    return value;
}

uint32_t BitReader::ReadIntMax(uint32_t max) {
    // Mirrors UE3 FArchive::SerializeInt — read bits while (value + mask) < max.
    uint32_t value = 0;
    for (uint32_t mask = 1; (value + mask) < max && mask != 0; mask <<= 1) {
        if (ReadBit()) {
            value |= mask;
        }
    }
    return value;
}

void BitReader::Skip(uint32_t bits) {
    pos_ += bits;
    if (pos_ > bit_len_) pos_ = bit_len_;
}

uint8_t BitReader::ReadByte() {
    return static_cast<uint8_t>(ReadBits(8));
}

uint32_t BitReader::ReadInt() {
    return ReadBits(32);
}

std::string BitReader::ReadString() {
    std::string str;
    while (true) {
        char c = static_cast<char>(ReadByte());
        if (c == 0) break;
        str += c;
    }
    return str;
}

BitWriter::BitWriter() : bit_count_(0) {
}

void BitWriter::WriteBit(bool bit) {
    uint32_t byte_pos = bit_count_ >> 3;
    uint32_t bit_pos = bit_count_ & 7;
    
    if (byte_pos >= data_.size()) {
        data_.push_back(0);
    }
    
    if (bit) {
        data_[byte_pos] |= (1 << bit_pos);
    } else {
        data_[byte_pos] &= ~(1 << bit_pos);
    }
    
    bit_count_++;
}

void BitWriter::WriteBits(uint32_t value, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        WriteBit((value >> i) & 1);
    }
}

void BitWriter::WriteIntMax(uint32_t value, uint32_t value_max) {
    // Mirrors UE3 FArchive::SerializeInt — emit bits while (new_value + mask) < value_max.
    uint32_t new_value = 0;
    for (uint32_t mask = 1; (new_value + mask) < value_max && mask != 0; mask <<= 1) {
        bool bit = (value & mask) != 0;
        WriteBit(bit);
        if (bit) new_value |= mask;
    }
}

void BitWriter::WriteByte(uint8_t value) {
    WriteBits(value, 8);
}

void BitWriter::WriteInt(uint32_t value) {
    WriteBits(value, 32);
}

void BitWriter::WriteString(const std::string& str) {
    for (char c : str) {
        WriteByte(static_cast<uint8_t>(c));
    }
    WriteByte(0);
}

void BitWriter::AppendBits(const BitWriter& other) {
    for (uint32_t i = 0; i < other.bit_count_; ++i) {
        bool bit = (other.data_[i >> 3] >> (i & 7)) & 1;
        WriteBit(bit);
    }
}

void BitWriter::WriteTerminator() {
    WriteBit(true);
    while ((bit_count_ & 7) != 0) WriteBit(false);
}

} // namespace util
} // namespace aa3
