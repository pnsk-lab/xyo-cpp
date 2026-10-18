#include "zip.hpp"

#include <cstring>

#include <zlib.h>

namespace xyo {

namespace {
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
} // namespace

bool ZipReader::open(const uint8_t* data, size_t size, ZipReader& out) {
    if (!data || size < 22) return false;
    // EOCD を末尾から探す（コメント長分だけ遡る）
    size_t lo = size >= 22 ? size - 22 : 0;
    size_t pos = size_t(-1);
    size_t scanStart = size > 22 + 0xFFFF ? size - 22 - 0xFFFF : 0;
    for (size_t i = lo; i >= scanStart; --i) {
        if (data[i] == 0x50 && data[i + 1] == 0x4B && data[i + 2] == 0x05 &&
            data[i + 3] == 0x06) {
            pos = i;
            break;
        }
        if (i == 0) break;
    }
    if (pos == size_t(-1)) return false;
    uint32_t cdCount = rd16(data + pos + 10);
    uint32_t cdOffset = rd32(data + pos + 16);
    const uint8_t* p = data + cdOffset;
    const uint8_t* end = data + size;
    out.data_ = data;
    out.size_ = size;
    out.entries_.clear();
    for (uint32_t i = 0; i < cdCount; ++i) {
        if (p + 46 > end || rd32(p) != 0x02014B50) return false;
        ZipEntry e;
        e.method = rd16(p + 10);
        e.compressedSize = rd32(p + 20);
        e.uncompressedSize = rd32(p + 24);
        uint16_t nameLen = rd16(p + 28);
        uint16_t extraLen = rd16(p + 30);
        uint16_t commentLen = rd16(p + 32);
        e.localHeaderOffset = rd32(p + 42);
        if (p + 46 + nameLen > end) return false;
        e.name.assign(reinterpret_cast<const char*>(p + 46), nameLen);
        out.entries_.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

const ZipEntry* ZipReader::find(const std::string& name) const {
    for (const ZipEntry& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

bool ZipReader::extract(const ZipEntry& e, std::vector<uint8_t>& out) const {
    const uint8_t* lh = data_ + e.localHeaderOffset;
    if (lh + 30 > data_ + size_ || rd32(lh) != 0x04034B50) return false;
    uint16_t nameLen = rd16(lh + 26);
    uint16_t extraLen = rd16(lh + 28);
    const uint8_t* src = lh + 30 + nameLen + extraLen;
    if (src + e.compressedSize > data_ + size_) return false;

    out.resize(e.uncompressedSize);
    if (e.method == 0) {   // stored
        if (e.compressedSize != e.uncompressedSize) return false;
        std::memcpy(out.data(), src, e.uncompressedSize);
        return true;
    }
    if (e.method == 8) {   // deflate（生ストリーム）
        z_stream zs{};
        zs.next_in = const_cast<Bytef*>(src);
        zs.avail_in = e.compressedSize;
        zs.next_out = out.data();
        zs.avail_out = e.uncompressedSize;
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
        int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        return rc == Z_STREAM_END && zs.total_out == e.uncompressedSize;
    }
    return false;
}

} // namespace xyo
