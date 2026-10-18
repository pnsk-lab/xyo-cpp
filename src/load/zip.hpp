// xyo-cpp: 最小 ZIP リーダ（sb3 = zip コンテナ用）。
// stored / deflate のみ対応。deflate は zlib 生 inflate。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xyo {

struct ZipEntry {
    std::string name;
    uint32_t method = 0;          // 0=stored, 8=deflate
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t localHeaderOffset = 0;
};

/// ZIP アーカイブ（メモリ全体を保持、遅延展開）
class ZipReader {
public:
    /// data からローカルヘッダを読むので data は read() 呼び出し中生存必須。
    static bool open(const uint8_t* data, size_t size, ZipReader& out);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    const ZipEntry* find(const std::string& name) const;

    /// 展開して返す。失敗時 false。
    bool extract(const ZipEntry& e, std::vector<uint8_t>& out) const;

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::vector<ZipEntry> entries_;
};

} // namespace xyo
