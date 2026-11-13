#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "rid.h"

/**
 * @class Tuple
 * @brief Represents a physical record / tuple stored in a TablePage.
 *
 * Encapsulates:
 * - Logical address identifier (RID: page_id and slot_num).
 * - Binary payload data (std::vector<char>).
 */
class Tuple {
private:
    RID rid_{};
    std::vector<char> data_;

public:
    /**
     * @brief Construct an empty default Tuple with invalid RID and empty data.
     */
    Tuple() = default;

    /**
     * @brief Construct a Tuple with a given RID and empty data.
     * @param rid The record identifier.
     */
    explicit Tuple(RID rid);

    /**
     * @brief Construct a Tuple from a raw byte buffer.
     * @param data Pointer to raw bytes.
     * @param size Number of bytes.
     * @param rid Optional record identifier (default: invalid RID).
     */
    Tuple(const char *data, uint32_t size, RID rid = {});

    /**
     * @brief Construct a Tuple from a std::string_view.
     * @param sv String view of bytes.
     * @param rid Optional record identifier (default: invalid RID).
     */
    Tuple(std::string_view sv, RID rid = {});

    /**
     * @brief Construct a Tuple from a std::string.
     * @param str Source string.
     * @param rid Optional record identifier (default: invalid RID).
     */
    Tuple(const std::string &str, RID rid = {});

    /**
     * @brief Construct a Tuple from a null-terminated C string.
     * @param str Null-terminated string.
     * @param rid Optional record identifier (default: invalid RID).
     */
    Tuple(const char *str, RID rid = {});

    /**
     * @brief Construct a Tuple by moving an existing vector of bytes.
     * @param data Vector of bytes.
     * @param rid Optional record identifier (default: invalid RID).
     */
    Tuple(std::vector<char> data, RID rid = {});

    /**
     * @brief Get the RID associated with this tuple.
     * @return RID record identifier.
     */
    RID GetRid() const { return rid_; }

    /**
     * @brief Set the RID for this tuple.
     * @param rid New record identifier.
     */
    void SetRid(RID rid) { rid_ = rid; }

    /**
     * @brief Get immutable pointer to raw tuple bytes.
     * @return const char* Pointer to buffer.
     */
    const char *GetData() const { return data_.data(); }

    /**
     * @brief Get mutable pointer to raw tuple bytes.
     * @return char* Pointer to buffer.
     */
    char *GetData() { return data_.data(); }

    /**
     * @brief Get byte length of tuple payload data.
     * @return Size in bytes.
     */
    uint32_t GetLength() const { return static_cast<uint32_t>(data_.size()); }

    /**
     * @brief Get byte size of tuple payload data (alias for GetLength()).
     * @return Size in bytes.
     */
    uint32_t GetSize() const { return static_cast<uint32_t>(data_.size()); }

    /**
     * @brief View tuple payload as std::string_view without copying.
     * @return String view of tuple data.
     */
    std::string_view AsStringView() const;

    /**
     * @brief View tuple payload as std::span<const char> without copying.
     * @return Byte span.
     */
    std::span<const char> AsSpan() const;

    /**
     * @brief Check whether this tuple contains zero bytes of data.
     * @return true if empty, false otherwise.
     */
    bool IsEmpty() const { return data_.empty(); }

    /**
     * @brief Equality comparison between two tuples (both RID and data must match).
     */
    bool operator==(const Tuple &other) const;

    /**
     * @brief Inequality comparison between two tuples.
     */
    bool operator!=(const Tuple &other) const;
};
