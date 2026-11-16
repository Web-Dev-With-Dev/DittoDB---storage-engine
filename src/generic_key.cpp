#include "generic_key.h"

// ==================== Unsigned Integers ====================

void KeySerializer::SerializeUint32(uint32_t val, char *dest) {
    dest[0] = static_cast<char>((val >> 24) & 0xFF);
    dest[1] = static_cast<char>((val >> 16) & 0xFF);
    dest[2] = static_cast<char>((val >> 8) & 0xFF);
    dest[3] = static_cast<char>(val & 0xFF);
}

uint32_t KeySerializer::DeserializeUint32(const char *src) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(src[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(src[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(src[2])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(src[3])));
}

void KeySerializer::SerializeUint64(uint64_t val, char *dest) {
    for (int i = 7; i >= 0; --i) {
        dest[7 - i] = static_cast<char>((val >> (i * 8)) & 0xFF);
    }
}

uint64_t KeySerializer::DeserializeUint64(const char *src) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | static_cast<uint64_t>(static_cast<uint8_t>(src[i]));
    }
    return val;
}

// ==================== Signed Integers (Order-Preserving Sign Flip) ====================

void KeySerializer::SerializeInt32(int32_t val, char *dest) {
    uint32_t u = static_cast<uint32_t>(val) ^ 0x80000000u;
    SerializeUint32(u, dest);
}

int32_t KeySerializer::DeserializeInt32(const char *src) {
    uint32_t u = DeserializeUint32(src) ^ 0x80000000u;
    return static_cast<int32_t>(u);
}

void KeySerializer::SerializeInt64(int64_t val, char *dest) {
    uint64_t u = static_cast<uint64_t>(val) ^ 0x8000000000000000ull;
    SerializeUint64(u, dest);
}

int64_t KeySerializer::DeserializeInt64(const char *src) {
    uint64_t u = DeserializeUint64(src) ^ 0x8000000000000000ull;
    return static_cast<int64_t>(u);
}

// ==================== Floating Point (IEEE 754 Order-Preserving) ====================

void KeySerializer::SerializeFloat(float val, char *dest) {
    uint32_t bits;
    std::memcpy(&bits, &val, sizeof(float));
    if ((bits & 0x80000000u) != 0) {
        bits = ~bits; // Negative number: invert all bits
    } else {
        bits |= 0x80000000u; // Positive number: flip sign bit
    }
    SerializeUint32(bits, dest);
}

float KeySerializer::DeserializeFloat(const char *src) {
    uint32_t bits = DeserializeUint32(src);
    if ((bits & 0x80000000u) != 0) {
        bits &= ~0x80000000u;
    } else {
        bits = ~bits;
    }
    float val;
    std::memcpy(&val, &bits, sizeof(float));
    return val;
}

void KeySerializer::SerializeDouble(double val, char *dest) {
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(double));
    if ((bits & 0x8000000000000000ull) != 0) {
        bits = ~bits;
    } else {
        bits |= 0x8000000000000000ull;
    }
    SerializeUint64(bits, dest);
}

double KeySerializer::DeserializeDouble(const char *src) {
    uint64_t bits = DeserializeUint64(src);
    if ((bits & 0x8000000000000000ull) != 0) {
        bits &= ~0x8000000000000000ull;
    } else {
        bits = ~bits;
    }
    double val;
    std::memcpy(&val, &bits, sizeof(double));
    return val;
}

// ==================== Strings ====================

void KeySerializer::SerializeString(std::string_view sv, char *dest, size_t dest_size) {
    size_t copy_len = std::min(sv.size(), dest_size);
    std::memcpy(dest, sv.data(), copy_len);
    if (copy_len < dest_size) {
        std::memset(dest + copy_len, 0, dest_size - copy_len);
    }
}

std::string_view KeySerializer::DeserializeString(const char *src, size_t src_size) {
    size_t len = 0;
    while (len < src_size && src[len] != '\0') {
        ++len;
    }
    return std::string_view(src, len);
}

