#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "anim.h"
#include "brick.h"
#include "camera.h"
#include "gpu.h"
#include "ground.h"
#include "params.h"

// Frame pipeline: brick edits (if any) -> trace (compute sphere tracer ->
// HDR) -> post (film look -> rgba8 presentTex) -> blit (swapchain + ImGui).
// Screenshot mode reads presentTex back. Shaders hot-reload from source,
// stitched together by a //#include preprocessor (WGSL has none).
class Renderer {
  public:
    // Flip the alive sentinel so any MapAsync callback (pick, GPU timestamp)
    // still queued at teardown bails before touching destroyed buffers.
    ~Renderer() { *alive_ = false; }

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
    const float* pickNormal() const { return pickNormal_; }
    float pickMat() const { return pickMat_; }
    // headless serve mode runs the pick pass too so ctl `probe` works
    void setAlwaysPick(bool v) { alwaysPick_ = v; }
    // snapshots the pick normal/albedo into the edit so conservation gobs
    // launch outward from the wound wearing its color; returns the resolved
    // edit so the ctl journal can record it pick-independently
    BrickEdit queueBrickEdit(BrickEdit e);
    BrickSystem& brick() { return brick_; }
    void setCharacter(CharacterAsset asset);
    const SplootStats& sploot() const { return sploot_; }

    // Sim-state snapshots (M-DEV): body volume + ground field + ledger +
    // gobs + sim time. Save drains in-flight volume measurements first so
    // the ledger in the file is settled. Load cancels queued bake/import.
    bool saveSnapshot(const std::string& path, double simT,
                      const std::string& charPath);
    bool loadSnapshot(const std::string& path, double* simT,
                      const std::string& charPath);
    // Block until queued volume measurements have landed in measured_ —
    // pins their arrival frame so journal replay is deterministic.
    void syncMeasurements();
    // Fold any completed measurements into the ledger without rendering.
    // The serve loop calls this while paused/idle so ctl `stats` doesn't
    // report a stale ledger (absorption otherwise only runs in render()).
    void pumpLedger() {
        brick_.pollVolumes();
        absorbMeasured();
    }

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
    // mat4, aabb lo/hi, rest capsule a/b) + gobMeta + 12 gobs x2 + groundMeta
    // + swordA/swordB/swordCol.
    // MUST match the Uniforms struct in trace.wgsl AND pick.wgsl — and a
    // mismatch only bites after a rebuild (shaders hot-load, binaries don't).
    static constexpr int kUniformSlots = 288;
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
    // M4.7: arm IK chains (built at setCharacter from bone names), and the
    // computed sword geometry for a frame (hilt/tip/two grips, world).
    std::vector<ArmIkChain> armIk_;
    void swordGeometry(const SwordParams& s, float hilt[3], float tip[3],
                       float gripA[3], float gripB[3]) const;
    void encodePick(wgpu::CommandEncoder& enc);
    void pollPick();

    // M4.6 conservation: carved volume -> ledger -> ballistic gobs ->
    // ground splats / body re-sticks. Visual sim, non-deterministic side.
    struct Gob {
        float pos[3], vel[3], disp[3]; // disp = 12 Hz-stepped display pos
        float radius, vol, grace;      // grace: skip body collision at launch
        float col[3];
    };
    void updateConservation(const LookParams& look, const FrameInfo& frame);
    void absorbMeasured(); // drain brick measurements into the ledger

    Gpu* gpu_ = nullptr;
    BrickSystem brick_;
    GroundClay ground_;
    std::vector<Gob> gobs_;
    SplootStats sploot_;
    float lastWound_[3] = {0, 0.5f, 0};
    float woundDir_[3] = {0, 1, 0};
    float woundCol_[3] = {0.024f, 0.19f, 0.25f};
    bool haveWound_ = false;
    float lastSimTime_ = -1.f, lastPoseTime_ = -1.f;
    uint32_t gobSeed_ = 0x9e3779b9u;
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
    bool alwaysPick_ = false;
    float pickU_ = 0.5f, pickV_ = 0.5f;
    bool pickValid_ = false;
    float pickPos_[3] = {0, 0, 0};
    float pickNormal_[3] = {0, 1, 0};
    float pickAlbedo_[3] = {0.024f, 0.19f, 0.25f};
    float pickMat_ = 0.f;

    long shaderDirStamp_ = 0;

    wgpu::QuerySet querySet_;
    wgpu::Buffer queryResolve_, queryRead_;
    bool queryMapPending_ = false;
    float traceMs_ = 0.f, postMs_ = 0.f;

    // Shared with in-flight MapAsync callbacks; false once destroyed.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
