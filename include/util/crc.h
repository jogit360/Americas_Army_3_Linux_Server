#ifndef AA3_CRC_H
#define AA3_CRC_H

#include <cstdint>
#include <string>

namespace aa3 {
namespace util {

class CRC32 {
public:
    static uint32_t Calculate(const std::string& str);
    static uint32_t Calculate(const uint8_t* data, size_t len);

private:
    static const uint32_t crc_table[256];
};

} // namespace util
} // namespace aa3

#endif // AA3_CRC_H
