#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <algorithm>
#include <type_traits>
#include <ostream>
#include <iomanip>
#include "config.h"
#include "rid.h"

// Forward declaration
template <size_t KeySize>
class GenericKey;

/**
 * @class KeySerializer
 * @brief Order-preserving serialization and deserialization helpers for database index keys.
 *
 * Implements canonical order-preserving encodings so that raw lexicographical byte comparisons
 * (std::memcmp) on serialized keys match the exact semantic ordering of the original data types:
 * - Unsigned integers: Big-endian bytes.
 * - Signed integers (int32_t, int64_t): High sign-bit inverted + big-endian bytes.
 * - Floating point (float, double): IEEE 754 sign-flip transform + big-endian bytes.
 * - Strings: Null-padded fixed-length ASCII / UTF-8 bytes.
 */
class KeySerializer {
public:
    // ==================== Unsigned Integers ====================

    static void SerializeUint32(uint32_t val, char *dest) {
        dest[0] = static_cast<char>((val >> 24) & 0xFF);
        dest[1] = static_cast<char>((val >> 16) & 0xFF);
        dest[2] = static_cast<char>((val >> 8) & 0xFF);
        dest[3] = static_cast<char>(val & 0xFF);
    }

    static uint32_t DeserializeUint32(const char *src) {
        return (static_cast<uint32_t>(static_cast<uint8_t>(src[0])) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(src[1])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(src[2])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(src[3])));
    }

    static void SerializeUint64(uint64_t val, char *dest) {
        for (int i = 7; i >= 0; --i) {
            dest[7 - i] = static_cast<char>((val >> (i * 8)) & 0xFF);
        }
    }

    static uint64_t DeserializeUint64(const char *src) {
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i) {
            val = (val << 8) | static_cast<uint64_t>(static_cast<uint8_t>(src[i]));
        }
        return val;
    }

    // ==================== Signed Integers (Order-Preserving Sign Flip) ====================

    static void SerializeInt32(int32_t val, char *dest) {
        uint32_t u = static_cast<uint32_t>(val) ^ 0x80000000u;
        SerializeUint32(u, dest);
    }

    static int32_t DeserializeInt32(const char *src) {
        uint32_t u = DeserializeUint32(src) ^ 0x80000000u;
        return static_cast<int32_t>(u);
    }

    static void SerializeInt64(int64_t val, char *dest) {
        uint64_t u = static_cast<uint64_t>(val) ^ 0x8000000000000000ull;
        SerializeUint64(u, dest);
    }

    static int64_t DeserializeInt64(const char *src) {
        uint64_t u = DeserializeUint64(src) ^ 0x8000000000000000ull;
        return static_cast<int64_t>(u);
    }

    // ==================== Floating Point (IEEE 754 Order-Preserving) ====================

    static void SerializeFloat(float val, char *dest) {
        uint32_t bits;
        std::memcpy(&bits, &val, sizeof(float));
        if ((bits & 0x80000000u) != 0) {
            bits = ~bits; // Negative number: invert all bits
        } else {
            bits |= 0x80000000u; // Positive number: flip sign bit
        }
        SerializeUint32(bits, dest);
    }

    static float DeserializeFloat(const char *src) {
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

    static void SerializeDouble(double val, char *dest) {
        uint64_t bits;
        std::memcpy(&bits, &val, sizeof(double));
        if ((bits & 0x8000000000000000ull) != 0) {
            bits = ~bits;
        } else {
            bits |= 0x8000000000000000ull;
        }
        SerializeUint64(bits, dest);
    }

    static double DeserializeDouble(const char *src) {
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

    static void SerializeString(std::string_view sv, char *dest, size_t dest_size) {
        size_t copy_len = std::min(sv.size(), dest_size);
        std::memcpy(dest, sv.data(), copy_len);
        if (copy_len < dest_size) {
            std::memset(dest + copy_len, 0, dest_size - copy_len);
        }
    }

    static std::string_view DeserializeString(const char *src, size_t src_size) {
        size_t len = 0;
        while (len < src_size && src[len] != '\0') {
            ++len;
        }
        return std::string_view(src, len);
    }

    // ==================== Generic Type Dispatchers ====================

    template <typename T>
    static void Serialize(const T &val, char *dest, size_t dest_size) {
        if constexpr (std::is_same_v<T, bool>) {
            assert(dest_size >= 1);
            dest[0] = val ? static_cast<char>(1) : static_cast<char>(0);
            if (dest_size > 1) {
                std::memset(dest + 1, 0, dest_size - 1);
            }
        } else if constexpr (std::is_integral_v<T>) {
            if constexpr (sizeof(T) == 1) {
                assert(dest_size >= 1);
                if constexpr (std::is_signed_v<T>) {
                    uint8_t u = static_cast<uint8_t>(val) ^ 0x80u;
                    dest[0] = static_cast<char>(u);
                } else {
                    dest[0] = static_cast<char>(val);
                }
                if (dest_size > 1) {
                    std::memset(dest + 1, 0, dest_size - 1);
                }
            } else if constexpr (sizeof(T) == 2) {
                assert(dest_size >= 2);
                uint16_t u = static_cast<uint16_t>(val);
                if constexpr (std::is_signed_v<T>) {
                    u ^= 0x8000u;
                }
                dest[0] = static_cast<char>((u >> 8) & 0xFF);
                dest[1] = static_cast<char>(u & 0xFF);
                if (dest_size > 2) {
                    std::memset(dest + 2, 0, dest_size - 2);
                }
            } else if constexpr (sizeof(T) == 4) {
                assert(dest_size >= 4);
                if constexpr (std::is_signed_v<T>) {
                    SerializeInt32(static_cast<int32_t>(val), dest);
                } else {
                    SerializeUint32(static_cast<uint32_t>(val), dest);
                }
                if (dest_size > 4) {
                    std::memset(dest + 4, 0, dest_size - 4);
                }
            } else if constexpr (sizeof(T) == 8) {
                assert(dest_size >= 8);
                if constexpr (std::is_signed_v<T>) {
                    SerializeInt64(static_cast<int64_t>(val), dest);
                } else {
                    SerializeUint64(static_cast<uint64_t>(val), dest);
                }
                if (dest_size > 8) {
                    std::memset(dest + 8, 0, dest_size - 8);
                }
            } else {
                static_assert(sizeof(T) <= 8, "Unsupported integer size for KeySerializer");
            }
        } else if constexpr (std::is_same_v<T, float>) {
            assert(dest_size >= sizeof(float));
            SerializeFloat(val, dest);
            if (dest_size > sizeof(float)) {
                std::memset(dest + sizeof(float), 0, dest_size - sizeof(float));
            }
        } else if constexpr (std::is_same_v<T, double>) {
            assert(dest_size >= sizeof(double));
            SerializeDouble(val, dest);
            if (dest_size > sizeof(double)) {
                std::memset(dest + sizeof(double), 0, dest_size - sizeof(double));
            }
        } else if constexpr (std::is_convertible_v<T, std::string_view>) {
            SerializeString(std::string_view(val), dest, dest_size);
        } else {
            // General trivially copyable fallback
            static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable for generic serialization");
            size_t copy_len = std::min(sizeof(T), dest_size);
            std::memcpy(dest, &val, copy_len);
            if (copy_len < dest_size) {
                std::memset(dest + copy_len, 0, dest_size - copy_len);
            }
        }
    }

    template <typename T>
    static T Deserialize(const char *src, size_t src_size) {
        if constexpr (std::is_same_v<T, bool>) {
            return src[0] != 0;
        } else if constexpr (std::is_integral_v<T>) {
            if constexpr (sizeof(T) == 1) {
                if constexpr (std::is_signed_v<T>) {
                    uint8_t u = static_cast<uint8_t>(src[0]) ^ 0x80u;
                    return static_cast<T>(static_cast<int8_t>(u));
                } else {
                    return static_cast<T>(static_cast<uint8_t>(src[0]));
                }
            } else if constexpr (sizeof(T) == 2) {
                uint16_t u = (static_cast<uint16_t>(static_cast<uint8_t>(src[0])) << 8) |
                             (static_cast<uint16_t>(static_cast<uint8_t>(src[1])));
                if constexpr (std::is_signed_v<T>) {
                    u ^= 0x8000u;
                    return static_cast<T>(static_cast<int16_t>(u));
                } else {
                    return static_cast<T>(u);
                }
            } else if constexpr (sizeof(T) == 4) {
                if constexpr (std::is_signed_v<T>) {
                    return static_cast<T>(DeserializeInt32(src));
                } else {
                    return static_cast<T>(DeserializeUint32(src));
                }
            } else if constexpr (sizeof(T) == 8) {
                if constexpr (std::is_signed_v<T>) {
                    return static_cast<T>(DeserializeInt64(src));
                } else {
                    return static_cast<T>(DeserializeUint64(src));
                }
            } else {
                static_assert(sizeof(T) <= 8, "Unsupported integer size for KeySerializer");
            }
        } else if constexpr (std::is_same_v<T, float>) {
            return DeserializeFloat(src);
        } else if constexpr (std::is_same_v<T, double>) {
            return DeserializeDouble(src);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return std::string(DeserializeString(src, src_size));
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return DeserializeString(src, src_size);
        } else {
            static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable for generic deserialization");
            T val;
            std::memcpy(&val, src, std::min(sizeof(T), src_size));
            return val;
        }
    }
};

/**
 * @class GenericKey
 * @brief Fixed-size byte array representation of database index keys in B+ Tree nodes.
 *
 * Provides standard-layout, trivially copyable key storage with compile-time sizing.
 * Supports direct memcmp lexicographical comparison when using KeySerializer.
 *
 * @tparam KeySize Size in bytes of the fixed key storage (e.g. 4, 8, 16, 32, 64).
 */
template <size_t KeySize>
class GenericKey {
public:
    static constexpr size_t KEY_SIZE = KeySize;
    static_assert(KeySize > 0, "GenericKey size must be greater than 0");

    /**
     * @brief Construct a zero-initialized GenericKey.
     */
    GenericKey() {
        data_.fill(0);
    }

    /**
     * @brief Construct GenericKey from raw bytes.
     * @param src Pointer to source buffer.
     */
    explicit GenericKey(const char *src) {
        SetFrom(src, KeySize);
    }

    /**
     * @brief Construct GenericKey from string_view.
     * @param sv String view.
     */
    explicit GenericKey(std::string_view sv) {
        SetFromString(sv);
    }

    /**
     * @brief Construct a GenericKey by serializing a typed value in order-preserving format.
     * @tparam T Value type.
     * @param val Value to serialize.
     * @return GenericKey<KeySize> Serialized key.
     */
    template <typename T>
    static GenericKey<KeySize> From(const T &val) {
        GenericKey<KeySize> key;
        KeySerializer::Serialize(val, key.GetData(), KeySize);
        return key;
    }

    /**
     * @brief Deserialize the key into a typed value.
     * @tparam T Destination value type.
     * @return T Deserialized value.
     */
    template <typename T>
    T CastTo() const {
        return KeySerializer::Deserialize<T>(data_.data(), KeySize);
    }

    // Direct buffer access
    char *GetData() { return data_.data(); }
    const char *GetData() const { return data_.data(); }
    constexpr size_t Size() const { return KeySize; }
    std::span<const char> AsSpan() const { return std::span<const char>(data_.data(), KeySize); }

    std::string_view AsStringView() const {
        return KeySerializer::DeserializeString(data_.data(), KeySize);
    }

    void SetFrom(const char *src, size_t len) {
        size_t copy_len = std::min(len, KeySize);
        std::memcpy(data_.data(), src, copy_len);
        if (copy_len < KeySize) {
            std::memset(data_.data() + copy_len, 0, KeySize - copy_len);
        }
    }

    void SetFromString(std::string_view sv) {
        SetFrom(sv.data(), sv.size());
    }

    // Comparison Operators (Lexicographical Byte Order)
    bool operator==(const GenericKey &other) const {
        return std::memcmp(data_.data(), other.data_.data(), KeySize) == 0;
    }

    bool operator!=(const GenericKey &other) const {
        return !(*this == other);
    }

    bool operator<(const GenericKey &other) const {
        return std::memcmp(data_.data(), other.data_.data(), KeySize) < 0;
    }

    bool operator<=(const GenericKey &other) const {
        return std::memcmp(data_.data(), other.data_.data(), KeySize) <= 0;
    }

    bool operator>(const GenericKey &other) const {
        return std::memcmp(data_.data(), other.data_.data(), KeySize) > 0;
    }

    bool operator>=(const GenericKey &other) const {
        return std::memcmp(data_.data(), other.data_.data(), KeySize) >= 0;
    }

    friend std::ostream &operator<<(std::ostream &os, const GenericKey &key) {
        os << "Key[0x";
        for (size_t i = 0; i < KeySize; ++i) {
            os << std::hex << std::setw(2) << std::setfill('0')
               << (static_cast<int>(key.data_[i]) & 0xFF);
        }
        os << std::dec << "]";
        return os;
    }

private:
    std::array<char, KeySize> data_;
};

static_assert(std::is_standard_layout_v<GenericKey<8>>, "GenericKey must satisfy standard layout");
static_assert(std::is_trivially_copyable_v<GenericKey<8>>, "GenericKey must be trivially copyable");

/**
 * @struct GenericComparator
 * @brief Fast 3-way memcmp comparator for GenericKey<KeySize>.
 */
template <size_t KeySize>
struct GenericComparator {
    int operator()(const GenericKey<KeySize> &a, const GenericKey<KeySize> &b) const {
        return std::memcmp(a.GetData(), b.GetData(), KeySize);
    }
};

/**
 * @struct KeyComparator
 * @brief Universal comparator template supporting both GenericKey and standard scalar types.
 */
template <typename KeyType>
struct KeyComparator {
    int operator()(const KeyType &a, const KeyType &b) const {
        if (a < b) return -1;
        if (b < a) return 1;
        return 0;
    }
};

template <size_t KeySize>
struct KeyComparator<GenericKey<KeySize>> {
    int operator()(const GenericKey<KeySize> &a, const GenericKey<KeySize> &b) const {
        return std::memcmp(a.GetData(), b.GetData(), KeySize);
    }
};

/**
 * @class BPlusTreePageSizeEstimator
 * @brief Compile-time and runtime utilities for calculating maximum node capacities from PAGE_SIZE.
 */
class BPlusTreePageSizeEstimator {
public:
    static constexpr size_t INTERNAL_HEADER_SIZE = 24; // sizeof(BPlusTreePage)
    static constexpr size_t LEAF_HEADER_RAW_SIZE = 28; // 24 + 4 (next_page_id)
    static constexpr size_t LEAF_HEADER_ALIGNED_SIZE = 32;

    /**
     * @brief Compute max capacity of an internal page at compile time or runtime.
     * @tparam KeyType Key type.
     * @tparam ValueType Pointer type (default: page_id_t).
     * @param page_size Physical page size in bytes (default: PAGE_SIZE = 4096).
     * @return int Max number of (key, child_page_id) pairs.
     */
    template <typename KeyType, typename ValueType = page_id_t>
    static constexpr int ComputeInternalMaxSize(size_t page_size = PAGE_SIZE) {
        using MappingType = std::pair<KeyType, ValueType>;
        return static_cast<int>((page_size - INTERNAL_HEADER_SIZE) / sizeof(MappingType));
    }

    /**
     * @brief Compute max capacity of a leaf page at compile time or runtime.
     * @tparam KeyType Key type.
     * @tparam ValueType Value type (default: RID).
     * @param page_size Physical page size in bytes (default: PAGE_SIZE = 4096).
     * @return int Max number of (key, RID) records.
     */
    template <typename KeyType, typename ValueType = RID>
    static constexpr int ComputeLeafMaxSize(size_t page_size = PAGE_SIZE) {
        using MappingType = std::pair<KeyType, ValueType>;
        return static_cast<int>((page_size - LEAF_HEADER_ALIGNED_SIZE) / sizeof(MappingType));
    }

    /**
     * @brief Compute max capacity for an internal page from raw byte sizes.
     * @param key_size Size of key in bytes.
     * @param value_size Size of child pointer in bytes (default: sizeof(page_id_t)).
     * @param page_size Physical page size (default: PAGE_SIZE).
     * @return int Max number of entries.
     */
    static constexpr int ComputeInternalMaxSizeFromKeySize(size_t key_size, size_t value_size = sizeof(page_id_t), size_t page_size = PAGE_SIZE) {
        size_t entry_size = ((key_size + value_size + 3) / 4) * 4;
        return static_cast<int>((page_size - INTERNAL_HEADER_SIZE) / entry_size);
    }

    /**
     * @brief Compute max capacity for a leaf page from raw byte sizes.
     * @param key_size Size of key in bytes.
     * @param value_size Size of record pointer in bytes (default: sizeof(RID)).
     * @param page_size Physical page size (default: PAGE_SIZE).
     * @return int Max number of entries.
     */
    static constexpr int ComputeLeafMaxSizeFromKeySize(size_t key_size, size_t value_size = sizeof(RID), size_t page_size = PAGE_SIZE) {
        size_t entry_size = ((key_size + value_size + 3) / 4) * 4;
        return static_cast<int>((page_size - LEAF_HEADER_ALIGNED_SIZE) / entry_size);
    }
};
