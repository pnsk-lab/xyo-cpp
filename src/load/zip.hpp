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

/// ZIP エントリ名が deflate 圧縮に向くか（scratch-vm の
/// COMPRESSABLE_FORMATS = .json/.svg/.wav/.ttf/.otf に準拠）。
bool zipShouldCompress(const std::string& name);

/// 最小 ZIP ライタ（sb3 保存用）。stored と deflate の両対応。
class ZipWriter {
public:
    /// 無圧縮（stored）で追加する。
    void addFile(const std::string& name, const std::vector<uint8_t>& data);

    /// deflate で追加する。level 0 または圧縮結果が元より大きい場合は
    /// 自動的に stored へフォールバックする（常に compressed <= raw）。
    void addFileCompressed(const std::string& name,
                           const std::vector<uint8_t>& data,
                           int level = 6);

    size_t entryCount() const { return entries_.size(); }
    /// 実際に deflate で書かれるエントリ数（圧縮結果が大きい場合は
    /// stored にフォールバックするため、追加時の指定とは一致しない）。
    size_t compressedCount() const;

    /// 最終化して ZIP バイト列を返す。
    std::vector<uint8_t> finish();

private:
    struct Entry {
        std::string name;
        std::vector<uint8_t> data;   // 実際に zip へ書くバイト列
        uint32_t crc = 0;
        uint32_t uncompressedSize = 0;
        uint16_t method = 0;         // 0=stored, 8=deflate
    };
    std::vector<Entry> entries_;
};

} // namespace xyo
