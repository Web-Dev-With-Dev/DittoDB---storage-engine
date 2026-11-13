#include <cstring>
#include "tuple.h"

Tuple::Tuple(RID rid) : rid_(rid) {}

Tuple::Tuple(const char *data, uint32_t size, RID rid)
    : rid_(rid), data_(data, data + size) {}

Tuple::Tuple(std::string_view sv, RID rid)
    : rid_(rid), data_(sv.begin(), sv.end()) {}

Tuple::Tuple(const std::string &str, RID rid)
    : rid_(rid), data_(str.begin(), str.end()) {}

Tuple::Tuple(const char *str, RID rid)
    : rid_(rid), data_(str, str + (str ? std::strlen(str) : 0)) {}

Tuple::Tuple(std::vector<char> data, RID rid)
    : rid_(rid), data_(std::move(data)) {}

std::string_view Tuple::AsStringView() const {
    return std::string_view(data_.data(), data_.size());
}

std::span<const char> Tuple::AsSpan() const {
    return std::span<const char>(data_.data(), data_.size());
}

bool Tuple::operator==(const Tuple &other) const {
    return rid_ == other.rid_ && data_ == other.data_;
}

bool Tuple::operator!=(const Tuple &other) const {
    return !(*this == other);
}

