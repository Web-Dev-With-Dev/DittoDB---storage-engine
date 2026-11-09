#include "rid.h"

std::string RID::ToString() const {
    return "[" + std::to_string(page_id_) + ", " + std::to_string(slot_num_) + "]";
}

std::ostream &operator<<(std::ostream &os, const RID &rid) {
    os << rid.ToString();
    return os;
}

