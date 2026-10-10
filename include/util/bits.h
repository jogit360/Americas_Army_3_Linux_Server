#ifndef AA3_BITS_H
#define AA3_BITS_H

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace aa3 {
namespace util {

class BitReader {
public:
    BitReader(const uint8_t* data, uint32_t bit_len);

    // Build a reader over an on-wire UE3 packet: trims trailing 0-pad and
    // the terminator bit so `bit_len` is the real payload bit count.
    static BitReader FromPacket(const uint8_t* data, uint32_t byte_len);

    bool ReadBit();
    uint32_t ReadBits(uint32_t count);
    uint32_t ReadIntMax(uint32_t max);
    uint8_t ReadByte();
    uint32_t ReadInt();
    std::string ReadString();
    void Skip(uint32_t bits);

    bool AtEnd() const { return pos_ >= bit_len_; }
    uint32_t BitsRemaining() const { return (pos_ >= bit_len_) ? 0 : (bit_len_ - pos_); }
    uint32_t GetPos() const { return pos_; }
    void SetPos(uint32_t p) { pos_ = p; }

private:
    const uint8_t* data_;
    uint32_t bit_len_;
    uint32_t pos_;
};

class BitWriter {
public:
    BitWriter();

    void WriteBit(bool bit);
    void WriteBits(uint32_t value, uint32_t count);
    void WriteIntMax(uint32_t value, uint32_t value_max);
    void WriteByte(uint8_t value);
    void WriteInt(uint32_t value);
    void WriteString(const std::string& str);

    // Copy another writer's bits onto the end of this one, bit-for-bit.
    // Used to splice a pre-built payload (e.g. a property-replication
    // sub-buffer, sized before the bunch header that must precede it) into
    // the real packet stream.
    void AppendBits(const BitWriter& other);

    // Append the UE3 terminator (single set bit) then zero-pad to a byte
    // boundary. After this call, GetData() is the on-wire packet.
    void WriteTerminator();

    const std::vector<uint8_t>& GetData() const { return data_; }
    uint32_t GetBitCount() const { return bit_count_; }

private:
    std::vector<uint8_t> data_;
    uint32_t bit_count_;
};

} // namespace util
} // namespace aa3

#endif // AA3_BITS_H
