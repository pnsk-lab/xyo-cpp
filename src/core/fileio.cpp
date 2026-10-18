// xyo-cpp: UTF-8 パスのファイル I/O 実装（fileio.hpp 参照）。
#include "fileio.hpp"

#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX          // std::min/std::max をマクロで壊させない
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace xyo {

#ifdef _WIN32
namespace {

std::wstring widen(const char* utf8) {
    if (!utf8) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    return w;
}

std::string narrow(const wchar_t* wide) {
    if (!wide) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace
#endif

std::FILE* fopenUtf8(const char* path, const char* mode) {
#ifdef _WIN32
    const std::wstring wp = widen(path);
    std::wstring wm;
    for (const char* p = mode; p && *p; ++p)
        wm.push_back(static_cast<wchar_t>(*p));
    if (wp.empty()) return nullptr;
    return _wfopen(wp.c_str(), wm.c_str());
#else
    return std::fopen(path, mode);
#endif
}

std::filesystem::path pathFromUtf8(const char* path) {
    if (!path) return {};
#ifdef _WIN32
    return std::filesystem::path(widen(path));
#else
    return std::filesystem::path(path);
#endif
}

std::filesystem::path pathFromUtf8(const std::string& path) {
    return pathFromUtf8(path.c_str());
}

std::string selfExePath() {
#ifdef _WIN32
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) {
            buf.resize(n + 1);
            buf[n] = L'\0';
            return narrow(buf.data());
        }
        buf.resize(buf.size() * 2);
    }
#elif defined(__linux__)
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
#else
    return {};
#endif
}

} // namespace xyo
