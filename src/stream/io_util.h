// UTF-8 path file helpers (Blender hands us UTF-8; Windows needs wide paths).
#pragma once

#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#endif

namespace vgeo_io {

#ifdef _WIN32
inline std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}
inline FILE* open_utf8(const char* path, const char* mode) {
    std::wstring m = widen(mode);
    return _wfopen(widen(path).c_str(), m.c_str());
}
inline bool seek(FILE* f, uint64_t at) { return _fseeki64(f, int64_t(at), SEEK_SET) == 0; }
inline int64_t tell(FILE* f) { return _ftelli64(f); }
inline bool replace_utf8(const char* from, const char* to) {
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
inline void remove_utf8(const char* path) { _wremove(widen(path).c_str()); }
#else
inline FILE* open_utf8(const char* path, const char* mode) { return std::fopen(path, mode); }
inline bool seek(FILE* f, uint64_t at) { return fseeko(f, off_t(at), SEEK_SET) == 0; }
inline int64_t tell(FILE* f) { return int64_t(ftello(f)); }
inline bool replace_utf8(const char* from, const char* to) { return std::rename(from, to) == 0; }
inline void remove_utf8(const char* path) { std::remove(path); }
#endif

}  // namespace vgeo_io
