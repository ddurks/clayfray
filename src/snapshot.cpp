#include "snapshot.h"

#include <cstring>

namespace {
constexpr char kMagic[4] = {'C', 'F', 'S', 'N'};
// 2: BrickEdit gained worldPos (rest-space pos + world wound, M5 locomotion)
// 3: BrickEdit gained the capsule brush (segment/posB) for blade cuts
// 4: kGrid 74 -> 50 (kVoxel 2.73 -> 4.05 mm); every pool/cell size changed
constexpr uint32_t kVersion = 4;

uint32_t tagWord(const char tag[4]) {
    uint32_t w;
    std::memcpy(&w, tag, 4);
    return w;
}
} // namespace

bool SnapWriter::open(const std::string& path) {
    f_ = std::fopen(path.c_str(), "wb");
    ok_ = f_ != nullptr;
    if (ok_) {
        ok_ = std::fwrite(kMagic, 1, 4, f_) == 4 &&
              std::fwrite(&kVersion, 4, 1, f_) == 1;
    }
    return ok_;
}

void SnapWriter::section(const char tag[4], const void* data, uint64_t size) {
    if (!ok_) return;
    uint32_t t = tagWord(tag);
    ok_ = std::fwrite(&t, 4, 1, f_) == 1 && std::fwrite(&size, 8, 1, f_) == 1 &&
          (size == 0 || std::fwrite(data, 1, size, f_) == size);
}

bool SnapWriter::close() {
    if (!f_) return false;
    bool ok = ok_ && std::fflush(f_) == 0 && std::ferror(f_) == 0;
    std::fclose(f_);
    f_ = nullptr;
    return ok;
}

bool SnapReader::open(const std::string& path) {
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) return false;
    char magic[4];
    uint32_t ver = 0;
    if (std::fread(magic, 1, 4, f_) != 4 || std::memcmp(magic, kMagic, 4) != 0 ||
        std::fread(&ver, 4, 1, f_) != 1 || ver != kVersion) {
        std::fprintf(stderr, "[snap] %s: not a CFSN v%u snapshot\n", path.c_str(),
                     kVersion);
        return false;
    }
    // long-based ftell/fseek caps files at 2 GB on Windows (long is 32-bit
    // there); snapshots are ~80 MB, revisit if they ever grow 25x
    for (;;) {
        Sec s{};
        if (std::fread(&s.tag, 4, 1, f_) != 1) break; // EOF
        if (std::fread(&s.size, 8, 1, f_) != 1) return false;
        long off = std::ftell(f_);
        if (off < 0) return false;
        s.off = (uint64_t)off;
        if (std::fseek(f_, (long)s.size, SEEK_CUR) != 0) return false;
        secs_.push_back(s);
    }
    return true;
}

SnapReader::~SnapReader() {
    if (f_) std::fclose(f_);
}

const SnapReader::Sec* SnapReader::find(const char tag[4]) const {
    uint32_t t = tagWord(tag);
    for (const Sec& s : secs_) {
        if (s.tag == t) return &s;
    }
    return nullptr;
}

bool SnapReader::has(const char tag[4]) const { return find(tag) != nullptr; }

uint64_t SnapReader::size(const char tag[4]) const {
    const Sec* s = find(tag);
    return s ? s->size : 0;
}

bool SnapReader::read(const char tag[4], void* out, uint64_t size) const {
    const Sec* s = find(tag);
    if (!s || s->size != size) return false;
    if (std::fseek(f_, (long)s->off, SEEK_SET) != 0) return false;
    return size == 0 || std::fread(out, 1, size, f_) == size;
}

std::vector<uint8_t> SnapReader::blob(const char tag[4]) const {
    const Sec* s = find(tag);
    std::vector<uint8_t> out;
    if (!s) return out;
    out.resize(s->size);
    if (!read(tag, out.data(), s->size)) out.clear();
    return out;
}

std::vector<uint8_t> readbackBuffer(Gpu& gpu, wgpu::Buffer src, uint64_t size) {
    std::vector<uint8_t> out;
    wgpu::BufferDescriptor desc{};
    desc.label = "snapshot readback";
    desc.size = size;
    desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer rb = gpu.device.CreateBuffer(&desc);
    wgpu::CommandEncoder enc = gpu.device.CreateCommandEncoder();
    enc.CopyBufferToBuffer(src, 0, rb, 0, size);
    wgpu::CommandBuffer cmd = enc.Finish();
    gpu.queue.Submit(1, &cmd);
    bool ok = false;
    wgpu::Future f = rb.MapAsync(wgpu::MapMode::Read, 0, size,
                                 wgpu::CallbackMode::WaitAnyOnly,
                                 [&ok](wgpu::MapAsyncStatus s, wgpu::StringView) {
                                     ok = s == wgpu::MapAsyncStatus::Success;
                                 });
    gpu.instance.WaitAny(f, UINT64_MAX);
    if (!ok) return out;
    out.resize(size);
    std::memcpy(out.data(), rb.GetConstMappedRange(0, size), size);
    rb.Unmap();
    return out;
}
