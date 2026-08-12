#include "renderer.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

std::string shaderPath(const char* name) {
    return std::string(CLAYFRAY_SHADER_DIR) + "/" + name;
}

std::string readFileRaw(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "failed to read %s\n", path.c_str());
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Stitch //#include name.wgsl lines (recursive, no duplicate guard — the
// include graph is a tree by construction).
std::string loadShader(const char* name) {
    std::string src = readFileRaw(shaderPath(name));
    std::stringstream out;
    std::stringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        const std::string tag = "//#include ";
        if (line.rfind(tag, 0) == 0) {
            std::string inc = line.substr(tag.size());
            while (!inc.empty() && (inc.back() == ' ' || inc.back() == '\r')) inc.pop_back();
            out << loadShader(inc.c_str()) << "\n";
        } else {
            out << line << "\n";
        }
    }
    return out.str();
}

// Newest mtime across the shader directory; drives hot reload.
long shaderDirStamp() {
    long newest = 0;
    DIR* dir = opendir(CLAYFRAY_SHADER_DIR);
    if (!dir) return 0;
    while (dirent* e = readdir(dir)) {
        std::string n = e->d_name;
        if (n.size() < 5 || n.substr(n.size() - 5) != ".wgsl") continue;
        struct stat st{};
        if (stat(shaderPath(n.c_str()).c_str(), &st) == 0 && (long)st.st_mtime > newest) {
            newest = (long)st.st_mtime;
        }
    }
    closedir(dir);
    return newest;
}

wgpu::ShaderModule makeModule(wgpu::Device& device, const std::string& src,
                              const char* label) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = src.c_str();
    wgpu::ShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl;
    desc.label = label;
    return device.CreateShaderModule(&desc);
}

} // namespace

bool Renderer::init(Gpu& gpu, int width, int height) {
    gpu_ = &gpu;
    width_ = width;
    height_ = height;

    wgpu::SamplerDescriptor sampDesc{};
    sampDesc.magFilter = wgpu::FilterMode::Linear;
    sampDesc.minFilter = wgpu::FilterMode::Linear;
    sampDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
    sampDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
    sampler_ = gpu_->device.CreateSampler(&sampDesc);

    wgpu::BufferDescriptor ubDesc{};
    ubDesc.size = kUniformSlots * 16;
    ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    uniformBuf_ = gpu_->device.CreateBuffer(&ubDesc);

    wgpu::BufferDescriptor pickDesc{};
    pickDesc.size = 16;
    pickDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
    pickOut_ = gpu_->device.CreateBuffer(&pickDesc);
    wgpu::BufferDescriptor pickReadDesc{};
    pickReadDesc.size = 16;
    pickReadDesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    pickRead_ = gpu_->device.CreateBuffer(&pickReadDesc);

    // OPT-IN: Dawn's Metal pass-boundary timestamps destabilize heavy
    // multi-pass frames on Apple Silicon (dropped command buffers -> blank
    // frames) and report dubious values. Wall-clock benchmarking is the
    // default tool here; timestamps work properly on D3D12/Vulkan.
    if (gpu_->hasTimestamps && std::getenv("CLAYFRAY_TS")) {
        wgpu::QuerySetDescriptor qsDesc{};
        qsDesc.type = wgpu::QueryType::Timestamp;
        qsDesc.count = 4;
        querySet_ = gpu_->device.CreateQuerySet(&qsDesc);
        wgpu::BufferDescriptor rDesc{};
        rDesc.size = 4 * 8;
        rDesc.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
        queryResolve_ = gpu_->device.CreateBuffer(&rDesc);
        wgpu::BufferDescriptor mDesc{};
        mDesc.size = 4 * 8;
        mDesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        queryRead_ = gpu_->device.CreateBuffer(&mDesc);
    }

    if (!brick_.init(gpu, loadShader("edit.wgsl"), loadShader("jfa.wgsl"),
                     loadShader("redistance.wgsl"), loadShader("voxelize.wgsl")))
        return false;

    // default marbles = the classic hand-coded eyes (linearized colors)
    auto lin = [](float c) { return std::pow(c, 2.2f); };
    for (int s : {1, -1}) {
        MarbleProp eye{};
        eye.pos[0] = s * 0.084f; eye.pos[1] = 1.055f; eye.pos[2] = 0.055f;
        eye.radius = 0.078f;
        eye.color[0] = lin(0.84f); eye.color[1] = lin(0.82f); eye.color[2] = lin(0.72f);
        marbles_.push_back(eye);
        MarbleProp pupil{};
        pupil.pos[0] = s * 0.094f; pupil.pos[1] = 1.069f; pupil.pos[2] = 0.123f;
        pupil.radius = 0.026f;
        pupil.color[0] = pupil.color[1] = pupil.color[2] = 0.015f;
        marbles_.push_back(pupil);
    }
    buildTargets();
    if (!buildPipelines()) return false;
    buildBindGroups();
    shaderDirStamp_ = shaderDirStamp();
    return true;
}

void Renderer::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    buildTargets();
    buildBindGroups();
}

void Renderer::buildTargets() {
    wgpu::TextureDescriptor hdrDesc{};
    hdrDesc.size = {(uint32_t)width_, (uint32_t)height_, 1};
    hdrDesc.format = wgpu::TextureFormat::RGBA16Float;
    hdrDesc.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding;
    hdrTex_ = gpu_->device.CreateTexture(&hdrDesc);
    hdrView_ = hdrTex_.CreateView();

    wgpu::TextureDescriptor presDesc{};
    presDesc.size = {(uint32_t)width_, (uint32_t)height_, 1};
    presDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    presDesc.usage = wgpu::TextureUsage::RenderAttachment |
                     wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc;
    presentTex_ = gpu_->device.CreateTexture(&presDesc);
    presentView_ = presentTex_.CreateView();
}

bool Renderer::buildPipelines() {
    std::string traceSrc = loadShader("trace.wgsl");
    std::string postSrc = loadShader("post.wgsl");
    std::string blitSrc = loadShader("blit.wgsl");
    std::string pickSrc = loadShader("pick.wgsl");
    if (traceSrc.empty() || postSrc.empty() || blitSrc.empty() || pickSrc.empty())
        return false;

    wgpu::Device& dev = gpu_->device;
    {
        wgpu::ShaderModule mod = makeModule(dev, traceSrc, "trace");
        wgpu::ComputePipelineDescriptor desc{};
        desc.label = "trace";
        desc.compute.module = mod;
        desc.compute.entryPoint = "cs";
        tracePipeline_ = dev.CreateComputePipeline(&desc);
    }
    {
        wgpu::ShaderModule mod = makeModule(dev, pickSrc, "pick");
        wgpu::ComputePipelineDescriptor desc{};
        desc.label = "pick";
        desc.compute.module = mod;
        desc.compute.entryPoint = "cs";
        pickPipeline_ = dev.CreateComputePipeline(&desc);
    }
    {
        wgpu::ShaderModule mod = makeModule(dev, postSrc, "post");
        wgpu::ColorTargetState target{};
        target.format = wgpu::TextureFormat::RGBA8Unorm;
        wgpu::FragmentState frag{};
        frag.module = mod;
        frag.entryPoint = "fs";
        frag.targetCount = 1;
        frag.targets = &target;
        wgpu::RenderPipelineDescriptor desc{};
        desc.label = "post";
        desc.vertex.module = mod;
        desc.vertex.entryPoint = "vs";
        desc.fragment = &frag;
        desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        postPipeline_ = dev.CreateRenderPipeline(&desc);
    }
    if (gpu_->surface) {
        wgpu::ShaderModule mod = makeModule(dev, blitSrc, "blit");
        wgpu::ColorTargetState target{};
        target.format = gpu_->surfaceFormat;
        wgpu::FragmentState frag{};
        frag.module = mod;
        frag.entryPoint = "fs";
        frag.targetCount = 1;
        frag.targets = &target;
        wgpu::RenderPipelineDescriptor desc{};
        desc.label = "blit";
        desc.vertex.module = mod;
        desc.vertex.entryPoint = "vs";
        desc.fragment = &frag;
        desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        blitPipeline_ = dev.CreateRenderPipeline(&desc);
    }
    return tracePipeline_ && postPipeline_ && pickPipeline_;
}

void Renderer::buildBindGroups() {
    wgpu::Device& dev = gpu_->device;
    {
        wgpu::BindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].buffer = uniformBuf_;
        entries[1].binding = 1;
        entries[1].textureView = hdrView_;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = tracePipeline_.GetBindGroupLayout(0);
        desc.entryCount = 2;
        desc.entries = entries;
        traceBind_ = dev.CreateBindGroup(&desc);
    }
    {
        wgpu::BindGroupEntry entries[5] = {};
        entries[0].binding = 0;
        entries[0].buffer = brick_.indirection;
        entries[1].binding = 1;
        entries[1].buffer = brick_.distPool;
        entries[2].binding = 2;
        entries[2].buffer = brick_.albedoPool;
        entries[3].binding = 3;
        entries[3].buffer = brick_.seeds;
        entries[4].binding = 4;
        entries[4].buffer = brick_.coarse;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = tracePipeline_.GetBindGroupLayout(1);
        desc.entryCount = 5;
        desc.entries = entries;
        traceBrickBind_ = dev.CreateBindGroup(&desc);
    }
    {
        wgpu::BindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].buffer = uniformBuf_;
        entries[1].binding = 1;
        entries[1].buffer = pickOut_;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pickPipeline_.GetBindGroupLayout(0);
        desc.entryCount = 2;
        desc.entries = entries;
        pickBind_ = dev.CreateBindGroup(&desc);
    }
    {
        // pick's charDist touches indirection + dist + seeds; auto layout
        // excludes the unused albedo binding
        wgpu::BindGroupEntry entries[3] = {};
        entries[0].binding = 0;
        entries[0].buffer = brick_.indirection;
        entries[1].binding = 1;
        entries[1].buffer = brick_.distPool;
        entries[2].binding = 3;
        entries[2].buffer = brick_.seeds;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pickPipeline_.GetBindGroupLayout(1);
        desc.entryCount = 3;
        desc.entries = entries;
        pickBrickBind_ = dev.CreateBindGroup(&desc);
    }
    {
        wgpu::BindGroupEntry entries[3] = {};
        entries[0].binding = 0;
        entries[0].buffer = uniformBuf_;
        entries[1].binding = 1;
        entries[1].textureView = hdrView_;
        entries[2].binding = 2;
        entries[2].sampler = sampler_;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = postPipeline_.GetBindGroupLayout(0);
        desc.entryCount = 3;
        desc.entries = entries;
        postBind_ = dev.CreateBindGroup(&desc);
    }
    if (blitPipeline_) {
        wgpu::BindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].textureView = presentView_;
        entries[1].binding = 1;
        entries[1].sampler = sampler_;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = blitPipeline_.GetBindGroupLayout(0);
        desc.entryCount = 2;
        desc.entries = entries;
        blitBind_ = dev.CreateBindGroup(&desc);
    }
}

bool Renderer::reloadShadersIfChanged() {
    long stamp = shaderDirStamp();
    if (stamp == shaderDirStamp_) return false;
    shaderDirStamp_ = stamp;
    std::printf("reloading shaders\n");
    if (buildPipelines()) buildBindGroups();
    // character source may have changed; rebuild the volume from it
    brick_.requestBake();
    return true;
}

void Renderer::packUniforms(const OrbitCamera& cam, const LookParams& look,
                            const FrameInfo& frame, float out[kUniformSlots][4]) const {
    Vec3 fwd, right, up;
    cam.basis(fwd, right, up);
    Vec3 pos = cam.pos();
    float aspect = (float)width_ / (float)height_;

    float packed[14][4] = {
        {pos.x, pos.y, pos.z, cam.fovY},
        {right.x, right.y, right.z, aspect},
        {up.x, up.y, up.z, frame.time},
        {fwd.x, fwd.y, fwd.z, frame.poseTime},
        {(float)width_, (float)height_, frame.grainFrame, (float)frame.aaSamples},
        {look.keyPos[0], look.keyPos[1], look.keyPos[2], look.keyIntensity},
        {look.keyColor[0], look.keyColor[1], look.keyColor[2], look.keyFalloff},
        {look.rimDir[0], look.rimDir[1], look.rimDir[2], look.rimIntensity},
        {look.rimColor[0], look.rimColor[1], look.rimColor[2], 0.f},
        {look.ambient[0], look.ambient[1], look.ambient[2], look.aoStrength},
        {look.detailAmount, look.boilAmount, look.shadowSoft, look.sheenAmount},
        {look.grainAmount, look.vignetteInner, look.vignetteOuter, look.weaveAmount},
        {look.exposure, look.bloomAmount, look.bloomThreshold, look.debugMode},
        {pickU_, pickV_, 0.f, 0.f},
    };
    std::memset(out, 0, kUniformSlots * 16);
    std::memcpy(out, packed, sizeof(packed));
    int n = std::min((int)marbles_.size(), 8);
    for (int i = 0; i < n; i++) {
        const MarbleProp& m = marbles_[i];
        float* slotA = out[14 + i * 2];
        float* slotB = out[15 + i * 2];
        slotA[0] = m.pos[0]; slotA[1] = m.pos[1]; slotA[2] = m.pos[2];
        slotA[3] = m.radius;
        slotB[0] = m.color[0]; slotB[1] = m.color[1]; slotB[2] = m.color[2];
    }
    out[30][0] = (float)n;
}

void Renderer::encodePick(wgpu::CommandEncoder& enc) {
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(pickPipeline_);
    pass.SetBindGroup(0, pickBind_);
    pass.SetBindGroup(1, pickBrickBind_);
    pass.DispatchWorkgroups(1);
    pass.End();
    if (!pickMapPending_) {
        enc.CopyBufferToBuffer(pickOut_, 0, pickRead_, 0, 16);
    }
}

void Renderer::pollPick() {
    if (pickMapPending_) return;
    pickMapPending_ = true;
    pickRead_.MapAsync(wgpu::MapMode::Read, 0, 16, wgpu::CallbackMode::AllowSpontaneous,
                       [this](wgpu::MapAsyncStatus status, wgpu::StringView) {
                           if (status == wgpu::MapAsyncStatus::Success) {
                               const float* d =
                                   (const float*)pickRead_.GetConstMappedRange(0, 16);
                               pickPos_[0] = d[0];
                               pickPos_[1] = d[1];
                               pickPos_[2] = d[2];
                               pickValid_ = d[3] > 0.5f;
                               pickRead_.Unmap();
                           }
                           pickMapPending_ = false;
                       });
}

void Renderer::render(const OrbitCamera& cam, const LookParams& look,
                      const FrameInfo& frame, wgpu::TextureView swapchainView,
                      const std::function<void(wgpu::RenderPassEncoder&)>& uiCallback) {
    float uniforms[kUniformSlots][4];
    packUniforms(cam, look, frame, uniforms);
    gpu_->queue.WriteBuffer(uniformBuf_, 0, uniforms, sizeof(uniforms));

    wgpu::CommandEncoder encoder = gpu_->device.CreateCommandEncoder();
    brick_.encode(encoder);

    {
        wgpu::PassTimestampWrites tsw{};
        wgpu::ComputePassDescriptor passDesc{};
        if (querySet_) {
            tsw.querySet = querySet_;
            tsw.beginningOfPassWriteIndex = 0;
            tsw.endOfPassWriteIndex = 1;
            passDesc.timestampWrites = &tsw;
        }
        wgpu::ComputePassEncoder pass = encoder.BeginComputePass(&passDesc);
        pass.SetPipeline(tracePipeline_);
        pass.SetBindGroup(0, traceBind_);
        pass.SetBindGroup(1, traceBrickBind_);
        pass.DispatchWorkgroups((width_ + 7) / 8, (height_ + 7) / 8);
        pass.End();
    }
    bool doPick = swapchainView != nullptr; // windowed only
    if (doPick) encodePick(encoder);
    {
        wgpu::PassTimestampWrites tsw{};
        wgpu::RenderPassColorAttachment att{};
        att.view = presentView_;
        att.loadOp = wgpu::LoadOp::Clear;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor desc{};
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &att;
        if (querySet_) {
            tsw.querySet = querySet_;
            tsw.beginningOfPassWriteIndex = 2;
            tsw.endOfPassWriteIndex = 3;
            desc.timestampWrites = &tsw;
        }
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
        pass.SetPipeline(postPipeline_);
        pass.SetBindGroup(0, postBind_);
        pass.Draw(3);
        pass.End();
    }
    if (swapchainView && blitPipeline_) {
        wgpu::RenderPassColorAttachment att{};
        att.view = swapchainView;
        att.loadOp = wgpu::LoadOp::Clear;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor desc{};
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &att;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
        pass.SetPipeline(blitPipeline_);
        pass.SetBindGroup(0, blitBind_);
        pass.Draw(3);
        if (uiCallback) uiCallback(pass);
        pass.End();
    }

    bool queryThisFrame = querySet_ && !queryMapPending_;
    if (queryThisFrame) {
        encoder.ResolveQuerySet(querySet_, 0, 4, queryResolve_, 0);
        encoder.CopyBufferToBuffer(queryResolve_, 0, queryRead_, 0, 4 * 8);
    }
    wgpu::CommandBuffer cmd = encoder.Finish();
    gpu_->queue.Submit(1, &cmd);
    brick_.finishCapacityPoll();
    if (queryThisFrame) {
        queryMapPending_ = true;
        queryRead_.MapAsync(wgpu::MapMode::Read, 0, 4 * 8,
                            wgpu::CallbackMode::AllowSpontaneous,
                            [this](wgpu::MapAsyncStatus status, wgpu::StringView) {
                                if (status == wgpu::MapAsyncStatus::Success) {
                                    const uint64_t* t =
                                        (const uint64_t*)queryRead_.GetConstMappedRange(
                                            0, 4 * 8);
                                    float tr = (float)((t[1] - t[0]) * 1e-6);
                                    float po = (float)((t[3] - t[2]) * 1e-6);
                                    traceMs_ = traceMs_ * 0.9f + tr * 0.1f;
                                    postMs_ = postMs_ * 0.9f + po * 0.1f;
                                    queryRead_.Unmap();
                                }
                                queryMapPending_ = false;
                            });
    }
    if (doPick) pollPick();
}

bool Renderer::screenshot(const std::string& path) {
    const uint32_t bytesPerRow = ((uint32_t)width_ * 4 + 255) & ~255u;
    const uint64_t bufSize = (uint64_t)bytesPerRow * height_;

    wgpu::BufferDescriptor bufDesc{};
    bufDesc.size = bufSize;
    bufDesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = gpu_->device.CreateBuffer(&bufDesc);

    wgpu::CommandEncoder encoder = gpu_->device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src{};
    src.texture = presentTex_;
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = readback;
    dst.layout.bytesPerRow = bytesPerRow;
    dst.layout.rowsPerImage = (uint32_t)height_;
    wgpu::Extent3D extent = {(uint32_t)width_, (uint32_t)height_, 1};
    encoder.CopyTextureToBuffer(&src, &dst, &extent);
    wgpu::CommandBuffer cmd = encoder.Finish();
    gpu_->queue.Submit(1, &cmd);

    bool ok = false;
    wgpu::Future f = readback.MapAsync(
        wgpu::MapMode::Read, 0, bufSize, wgpu::CallbackMode::WaitAnyOnly,
        [&ok](wgpu::MapAsyncStatus status, wgpu::StringView msg) {
            ok = status == wgpu::MapAsyncStatus::Success;
            if (!ok)
                std::fprintf(stderr, "readback map failed: %.*s\n", (int)msg.length,
                             msg.data);
        });
    gpu_->instance.WaitAny(f, UINT64_MAX);
    if (!ok) return false;

    const uint8_t* data = (const uint8_t*)readback.GetConstMappedRange(0, bufSize);
    std::vector<uint8_t> pixels((size_t)width_ * height_ * 4);
    for (int y = 0; y < height_; y++) {
        std::memcpy(pixels.data() + (size_t)y * width_ * 4, data + (size_t)y * bytesPerRow,
                    (size_t)width_ * 4);
    }
    readback.Unmap();

    int rc = stbi_write_png(path.c_str(), width_, height_, 4, pixels.data(), width_ * 4);
    if (rc) std::printf("wrote %s (%dx%d)\n", path.c_str(), width_, height_);
    return rc != 0;
}
