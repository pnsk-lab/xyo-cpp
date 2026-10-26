#include "zip.hpp"

#include <cctype>
#include <cstring>
#include <string>

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

// ---------------------------------------------------------------------------
// ZipWriter（stored / deflate）
// ---------------------------------------------------------------------------

namespace {
void wr16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v & 0xFF));
    b.push_back(uint8_t((v >> 8) & 0xFF));
}
void wr32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v & 0xFF));
    b.push_back(uint8_t((v >> 8) & 0xFF));
    b.push_back(uint8_t((v >> 16) & 0xFF));
    b.push_back(uint8_t((v >> 24) & 0xFF));
}

/// raw deflate（zip の method 8 は zlib ヘッダ無しのストリーム）。
std::vector<uint8_t> deflateRaw(const std::vector<uint8_t>& in, int level) {
    if (in.empty() || level <= 0 || level > 9) return {};
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    // deflateBound は uLong 長。sb3 の 1 エントリは 4GiB 未満なので uInt で足りる。
    uLong bound = deflateBound(&zs, (uLong)in.size());
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = (uInt)in.size();
    zs.next_out = out.data();
    zs.avail_out = (uInt)out.size();
    int rc = deflate(&zs, Z_FINISH);
    const size_t n = size_t(zs.total_out);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return {};
    out.resize(n);
    return out;
}
} // namespace

bool zipShouldCompress(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return false;
    std::string ext;
    ext.reserve(name.size() - dot - 1);
    for (size_t i = dot + 1; i < name.size(); ++i)
        ext.push_back(char(std::tolower((unsigned char)name[i])));
    return ext == "json" || ext == "svg" || ext == "wav" || ext == "ttf" ||
           ext == "otf";
}

void ZipWriter::addFile(const std::string& name, const std::vector<uint8_t>& data) {
    Entry e;
    e.name = name;
    e.data = data;
    e.crc = crc32(0, data.data(), (uInt)data.size());
    e.uncompressedSize = (uint32_t)data.size();
    e.method = 0;
    entries_.push_back(std::move(e));
}

void ZipWriter::addFileCompressed(const std::string& name,
                                  const std::vector<uint8_t>& data,
                                  int level) {
    Entry e;
    e.name = name;
    e.crc = crc32(0, data.data(), (uInt)data.size());
    e.uncompressedSize = (uint32_t)data.size();
    std::vector<uint8_t> comp = deflateRaw(data, level);
    // 圧縮して小さくならない（PNG/MP3 等）場合は stored のままにする。
    if (!comp.empty() && comp.size() < data.size()) {
        e.data = std::move(comp);
        e.method = 8;
    } else {
        e.data = data;
        e.method = 0;
    }
    entries_.push_back(std::move(e));
}

size_t ZipWriter::compressedCount() const {
    size_t n = 0;
    for (const Entry& e : entries_)
        if (e.method == 8) ++n;
    return n;
}

std::vector<uint8_t> ZipWriter::finish() {
    std::vector<uint8_t> out;
    std::vector<uint32_t> offsets;
    for (const Entry& e : entries_) {
        offsets.push_back((uint32_t)out.size());
        // local file header
        wr32(out, 0x04034B50);
        wr16(out, 20);                 // version needed
        wr16(out, 0);                  // flags
        wr16(out, e.method);           // method = stored / deflate
        wr16(out, 0);                  // time
        wr16(out, 0x21);               // date (1980-01-01)
        wr32(out, e.crc);
        wr32(out, (uint32_t)e.data.size());
        wr32(out, e.uncompressedSize);
        wr16(out, (uint16_t)e.name.size());
        wr16(out, 0);                  // extra len
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.data.begin(), e.data.end());
    }
    uint32_t cdOffset = (uint32_t)out.size();
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        wr32(out, 0x02014B50);
        wr16(out, 20);                 // version made by
        wr16(out, 20);                 // version needed
        wr16(out, 0);                  // flags
        wr16(out, e.method);
        wr16(out, 0);                  // time
        wr16(out, 0x21);               // date
        wr32(out, e.crc);
        wr32(out, (uint32_t)e.data.size());
        wr32(out, e.uncompressedSize);
        wr16(out, (uint16_t)e.name.size());
        wr16(out, 0);                  // extra
        wr16(out, 0);                  // comment
        wr16(out, 0);                  // disk
        wr16(out, 0);                  // internal attrs
        wr32(out, 0);                  // external attrs
        wr32(out, offsets[i]);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    uint32_t cdSize = (uint32_t)(out.size() - cdOffset);
    wr32(out, 0x06054B50);
    wr16(out, 0);                      // disk
    wr16(out, 0);                      // cd disk
    wr16(out, (uint16_t)entries_.size());
    wr16(out, (uint16_t)entries_.size());
    wr32(out, cdSize);
    wr32(out, cdOffset);
    wr16(out, 0);                      // comment len
    return out;
}

} // namespace xyo
