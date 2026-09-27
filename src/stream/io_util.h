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
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <vector>

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

// Read-only memory map of a whole file. Pages load on first touch and the OS
// evicts them under memory pressure, so an asset larger than RAM can be
// opened, and several handles on one file share the same pages.
struct MappedFile {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#endif

    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() { close(); }

    bool open(const char* path) {
        close();
#ifdef _WIN32
        // FILE_SHARE_DELETE: the file can still be renamed or deleted (it goes once unmapped)
        file = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0) { close(); return false; }
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) { close(); return false; }
        data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!data) { close(); return false; }
        size = uint64_t(sz.QuadPart);
        return true;
#else
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
        void* p = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return false;
        madvise(p, size_t(st.st_size), MADV_RANDOM);
        data = static_cast<const uint8_t*>(p);
        size = uint64_t(st.st_size);
        return true;
#endif
    }

    void close() {
#ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        mapping = nullptr;
        file = INVALID_HANDLE_VALUE;
#else
        if (data) munmap(const_cast<uint8_t*>(data), size_t(size));
#endif
        data = nullptr;
        size = 0;
    }

    // Ask the OS to start reading these byte ranges (offset, length) in the background.
    void prefetch(const uint64_t* ranges, size_t count) const {
        if (!data || count == 0) return;
#ifdef _WIN32
        struct Range { void* address; size_t bytes; };  // WIN32_MEMORY_RANGE_ENTRY (Windows 8+ headers)
        typedef BOOL(WINAPI * PrefetchFn)(HANDLE, ULONG_PTR, Range*, ULONG);
        static PrefetchFn fn = reinterpret_cast<PrefetchFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory")));
        if (!fn) return;  // before Windows 8: pages still load on demand
        std::vector<Range> e(count);
        for (size_t i = 0; i < count; ++i) {
            e[i].address = const_cast<uint8_t*>(data + ranges[2 * i]);
            e[i].bytes = size_t(ranges[2 * i + 1]);
        }
        fn(GetCurrentProcess(), ULONG_PTR(count), e.data(), 0);
#else
        const uint64_t page = uint64_t(sysconf(_SC_PAGESIZE));
        for (size_t i = 0; i < count; ++i) {
            uint64_t start = ranges[2 * i] / page * page;
            uint64_t end = ranges[2 * i] + ranges[2 * i + 1];
            madvise(const_cast<uint8_t*>(data + start), size_t(end - start), MADV_WILLNEED);
        }
#endif
    }
};

}  // namespace vgeo_io
