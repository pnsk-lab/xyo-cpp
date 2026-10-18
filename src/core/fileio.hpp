// xyo-cpp: UTF-8 パスのファイル I/O ヘルパ。
//
// POSIX ではパスはそのままバイト列だが、Windows の CRT は狭い文字列を
// ANSI コードページとして解釈するため、日本語などのパスが開けない。
// UTF-8 → UTF-16 へ変換して wide API に渡す。
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace xyo {

/// UTF-8 パスで C ストリームを開く（Windows では _wfopen）。
std::FILE* fopenUtf8(const char* path, const char* mode);

/// UTF-8 パス → std::filesystem::path。
/// Windows では UTF-16 として構築する（ifstream 等が wide API を使う）。
std::filesystem::path pathFromUtf8(const char* path);
std::filesystem::path pathFromUtf8(const std::string& path);

/// 実行ファイル自身の UTF-8 パス（Linux: /proc/self/exe、
/// Windows: GetModuleFileNameW）。取得できなければ空。
std::string selfExePath();

} // namespace xyo
