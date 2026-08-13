#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gpu.h"

// Tagged-section binary snapshot file ("CFSN" v1). Each section is
// (4-char tag, u64 byte size, raw bytes); readers index by tag, so files
// stay loadable as sections are added or reordered. Same-build only — the
// payloads are raw struct/GPU-buffer memory, not a portable interchange.
class SnapWriter {
  public:
    bool open(const std::string& path);
    void section(const char tag[4], const void* data, uint64_t size);
    bool close(); // false if any write failed

  private:
    FILE* f_ = nullptr;
    bool ok_ = false;
};

class SnapReader {
  public:
    bool open(const std::string& path);
    ~SnapReader();
    bool has(const char tag[4]) const;
    uint64_t size(const char tag[4]) const;
    // Exact-size read; false on missing tag or size mismatch.
    bool read(const char tag[4], void* out, uint64_t size) const;
    std::vector<uint8_t> blob(const char tag[4]) const;

  private:
    struct Sec {
        uint32_t tag;
        uint64_t off, size;
    };
    const Sec* find(const char tag[4]) const;
    FILE* f_ = nullptr;
    std::vector<Sec> secs_;
};

// Blocking GPU->CPU buffer readback (dev tooling only — a save hitch is
// fine). Empty vector on failure. `size` must be a multiple of 4.
std::vector<uint8_t> readbackBuffer(Gpu& gpu, wgpu::Buffer src, uint64_t size);
