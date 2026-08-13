#pragma once
#include <functional>
#include <string>
#include <vector>

#include "anim.h"
#include "brick.h"
#include "camera.h"
#include "gpu.h"
#include "params.h"

// Frame pipeline: brick edits (if any) -> trace (compute sphere tracer ->
// HDR) -> post (film look -> rgba8 presentTex) -> blit (swapchain + ImGui).
// Screenshot mode reads presentTex back. Shaders hot-reload from source,
// stitched together by a //#include preprocessor (WGSL has none).
class Renderer {
  public:
    bool init(Gpu& gpu, int width, int height);
    void resize(int width, int height);
    bool reloadShadersIfChanged();
    void render(const OrbitCamera& cam, const LookParams& look, const FrameInfo& frame,
                wgpu::TextureView swapchainView,
                const std::function<void(wgpu::RenderPassEncoder&)>& uiCallback);
    bool screenshot(const std::string& path);

    // sculpting
    void setPickUV(float u, float v) { pickU_ = u; pickV_ = v; }
    bool pickValid() const { return pickValid_; }
    const float* pickPos() const { return pickPos_; }
    void queueBrickEdit(const BrickEdit& e) { brick_.queueEdit(e); }
    BrickSystem& brick() { return brick_; }
    void setCharacter(CharacterAsset asset);

    int width() const { return width_; }
    int height() const { return height_; }

    // smoothed GPU pass times, ms (0 when timestamps unsupported)
    float traceMs() const { return traceMs_; }
    float postMs() const { return postMs_; }

  private:
    bool buildPipelines();
    void buildTargets();
    void buildBindGroups();
    // 13 look + mouse + 8 marbles x2 + marbleMeta + capsMeta + capsCenter +
    // 16 capsules x2 + boneMeta + 16 pieces x12 (invSkin mat4, forward skin
    // mat4, aabb lo/hi, rest capsule a/b)
    static constexpr int kUniformSlots = 258;
    void packUniforms(const OrbitCamera& cam, const LookParams& look,
                      const FrameInfo& frame, float out[kUniformSlots][4]) const;
    std::vector<MarbleProp> marbles_;
    // P0 animation: skeleton + clips kept CPU-side; pose sampled at the 12 Hz
    // quantized clock; marbles and the capsule shadow proxy ride the skin
    // matrices. (The voxel body stays in rest pose until M4-P1.)
    std::vector<AssetBone> bones_;
    std::vector<AnimClip> clips_;
    std::vector<BoneCapsule> capsules_;
    std::vector<float> skinMats_;
    float animT_ = 0.f;
    void encodePick(wgpu::CommandEncoder& enc);
    void pollPick();

    Gpu* gpu_ = nullptr;
    BrickSystem brick_;
    int width_ = 0, height_ = 0;

    wgpu::Texture hdrTex_, presentTex_;
    wgpu::TextureView hdrView_, presentView_;
    wgpu::Sampler sampler_;
    wgpu::Buffer uniformBuf_;

    wgpu::ComputePipeline tracePipeline_, pickPipeline_;
    wgpu::RenderPipeline postPipeline_, blitPipeline_;
    wgpu::BindGroup traceBind_, traceBrickBind_, postBind_, blitBind_;
    wgpu::BindGroup pickBind_, pickBrickBind_;
    wgpu::Buffer pickOut_, pickRead_;
    bool pickMapPending_ = false;
    float pickU_ = 0.5f, pickV_ = 0.5f;
    bool pickValid_ = false;
    float pickPos_[3] = {0, 0, 0};

    long shaderDirStamp_ = 0;

    wgpu::QuerySet querySet_;
    wgpu::Buffer queryResolve_, queryRead_;
    bool queryMapPending_ = false;
    float traceMs_ = 0.f, postMs_ = 0.f;
};
