// xyo-cpp: process-wide Scratch list settings.
#include "slist.hpp"

#include <cstdlib>
#include <cstring>

extern "C" uint32_t xyo_list_limit = 0;

namespace {
uint32_t initialListStorageMode() {
    const char* mode = std::getenv("XYO_LIST_STORAGE");
    if (!mode || std::strcmp(mode, "flat") == 0) return 0;
    if (std::strcmp(mode, "sbo4") == 0) return 1;
    if (std::strcmp(mode, "sbo8") == 0) return 2;
    if (std::strcmp(mode, "flat15") == 0) return 3;
    if (std::strcmp(mode, "deque") == 0) return 4;
    if (std::strcmp(mode, "gap") == 0) return 5;
    if (std::strcmp(mode, "chunked") == 0) return 6;
    if (std::strcmp(mode, "adaptive") == 0) return 7;
    return 0;
}
} // namespace

extern "C" uint32_t xyo_list_storage_mode = initialListStorageMode();
