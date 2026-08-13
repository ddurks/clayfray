#pragma once

// M4.7 sword prop + arm IK. Debug: the hilt transform is driven by these
// panel controls so dragging it demonstrates the hands following via IK.
// Later the swing/hold pose drives the transform instead.
struct SwordParams {
    bool enabled = true;             // hold the sword + run arm IK
    float pos[3] = {0.f, 0.30f, 0.38f}; // hilt, world (in front of the chest)
    float yaw = 0.f;                 // radians about world Y
    float pitch = 0.7f;              // radians: +tilts blade up-forward from +Z
    float length = 0.85f;            // hilt -> tip, meters
    float radius = 0.018f;           // blade thickness
    float grip0 = 0.03f;             // near-hilt grip (one hand), m along blade
    float grip1 = 0.14f;             // stacked grip (other hand), m along blade
    float color[3] = {0.35f, 0.85f, 1.0f}; // debug lightsaber cyan (emissive)
};

// Look-dev parameters, exposed in the ImGui panel and packed into the
// uniform buffer by the renderer. Defaults target the Trap Door "day
// dungeon" rig: warm amber key pooling to black, faint cool rim.
struct LookParams {
    // close-in key = tight pool of light; distance ratio subject:edge is what
    // makes the darkness, not raw falloff
    float keyPos[3] = {-1.1f, 2.1f, 1.6f};
    float keyIntensity = 19.0f;
    float keyColor[3] = {1.0f, 0.70f, 0.40f};
    float keyFalloff = 1.4f; // attenuation = intensity / (1 + falloff * d^2)

    float rimDir[3] = {-0.5f, 0.35f, -1.0f};
    float rimIntensity = 0.95f;
    float rimColor[3] = {0.45f, 0.62f, 0.85f};

    float ambient[3] = {0.016f, 0.012f, 0.009f};
    float aoStrength = 1.15f;

    float detailAmount = 0.8f; // thumbprint/tool-mark normal perturbation
    float boilAmount = 0.5f;   // per-pose-step detail reseed (stop-motion boil)
    float shadowSoft = 10.0f;  // soft shadow sharpness k
    float sheenAmount = 0.008f; // dry clay barely sheens; plasticine was 0.04

    float grainAmount = 0.07f;
    float vignetteInner = 0.72f;
    float vignetteOuter = 1.60f;
    // gate weave (whole-frame drift): OFF by default — at reduced internal
    // resolution it amplifies into camera shake (user). Slider remains for
    // taste; unrelated to boil, which lives in surface detail.
    float weaveAmount = 0.0f;

    float exposure = 1.1f;
    float bloomAmount = 0.12f;
    float bloomThreshold = 0.75f;
    float debugMode = 0.f; // 1 = normals visualization (set via CLAYFRAY_DEBUG_NORMALS)
    // internal render scale (windowed): trace at a fraction of window size,
    // blit upscales. Chunky low-res + grain reads very stop-motion, and
    // traced pixels are the whole frame cost.
    float resScale = 0.5f; // user-approved default: chunky + fast

    // M4-P0 animation: play the asset's first clip, looped, sampled on the
    // 12 Hz pose grid. Off = rest pose. (Drives marbles + shadow proxy; the
    // voxel body articulates in M4-P1.)
    bool animPlay = true;
    float animSpeed = 1.0f;

    // M4.6 conservation: carved clay sploots onto the arena instead of
    // vanishing. Off = the pre-conservation vanish behavior.
    bool conserveClay = true;

    // M4.7 sword prop + arm IK (debug-controlled hilt transform).
    SwordParams sword;
};

// Conservation ledger readout for the panel (all volumes in m^3).
struct SplootStats {
    float carved = 0.f;    // total measured off the body
    float deposited = 0.f; // total landed (splats + re-sticks)
    float debt = 0.f;      // measured but not yet spawned as gobs
    float inFlight = 0.f;  // riding gobs right now
    int gobs = 0;
};

// Sculpt-mode UI state (windowed only).
struct BrushState {
    int mode = 0; // 0 orbit, 1 carve, 2 add
    float radius = 0.045f;
    float color[3] = {0.72f, 0.45f, 0.40f}; // terracotta — contamination preview
};

struct FrameInfo {
    float time = 0.f;      // seconds, smooth
    float poseTime = 0.f;  // quantized to 12 Hz (stop-motion pose steps)
    float grainFrame = 0.f; // integer counter at 25 Hz (film frames)
    int aaSamples = 1;     // rays per pixel axis (1 = 1 ray, 2 = 4 rays)
};
