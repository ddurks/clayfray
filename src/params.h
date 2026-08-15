#pragma once

// M5 locomotion: where the fighter stands and which way it is going. The
// whole skeleton is premultiplied by this, so pose clips stay authored in
// character space and the sword/hands follow for free.
struct FighterPose {
    float pos[3] = {0.f, 0.f, 0.f}; // world, feet on the arena floor
    float yaw = 0.f;                // facing, radians about +Y
    float lean = 0.f;               // radians tilted INTO the direction of travel
    bool moving = false;            // picks the bounce clip over idle
};

// M4.7 sword prop + hand IK. Debug: the hilt transform is driven by these
// panel controls so dragging it demonstrates the hands following via IK.
// Later the swing/hold pose drives the transform instead.
struct SwordParams {
    bool enabled = true;             // hold the sword + run hand IK
    // carried: `pos`/`yaw` are read in CHARACTER space and ride the fighter's
    // root, so walking carries the sword and the hands keep their grip. Off =
    // the old debug behaviour, hilt placed directly in world space.
    bool carry = true;
    float pos[3] = {0.16f, 0.26f, 0.30f}; // hilt, in front of and beside the chest
    float yaw = 0.f;                 // radians about world Y
    // guard pose: blade STRAIGHT UP (pitch = pi/2 makes dir = +Y). The swing
    // drops it to horizontal and sweeps; at rest it stands vertical.
    float pitch = 1.5708f;
    float length = 0.85f;            // hilt -> tip, meters
    float radius = 0.018f;           // blade thickness
    // grip spacing must clear the mitt's own thickness along the handle
    // (~0.10 m on this rig) or the two hands interpenetrate
    float grip0 = 0.05f;             // near-hilt grip (one hand), m along blade
    float grip1 = 0.19f;             // stacked grip (other hand), m along blade
    float color[3] = {0.35f, 0.85f, 1.0f}; // debug lightsaber cyan (emissive)
};

// M4.7 floating hands. The rig has no arms — the mitts are detached, and
// the ONLY thing holding them to the fighter is `reach`, a max distance from
// the blob body's centre of mass. Push the sword past that and the hands
// stop following, which reads as the grip slipping rather than the arms
// stretching (there are no arms to stretch).
struct HandParams {
    bool ik = true;          // hands follow the sword grips
    float reach = 0.f;       // max m from body COM; 0 = auto from the rest rig
    float reachScale = 1.5f; // auto reach = rest COM->wrist distance * this
    float palmFrac = 0.6f;   // where along wrist->fingertip the grip sits
    bool orient = true;      // wrap the mitts around the handle
    float gripRoll = -1.5f;  // radians about the handle: where fingers point
    // Two-handed grip. The mitts sit on OPPOSITE faces of the blade rather
    // than both on its axis — each offset sideways by half the blade's width,
    // so `gripSpread` is measured in sword radii (1 = exactly half the width
    // to each side, i.e. each palm on its own face). Then each hand rolls
    // INWARD by gripCurl, mirrored, so the fingers wrap toward each other the
    // way they do around a real handle instead of both pointing the same way.
    float gripSpread = 2.5f;  // lateral offset per hand, in sword radii
    // Finger curl: rotates the thumb/finger subtrees ABOUT THE HANDLE AXIS so
    // the digits wrap onto the blade. The wrist is deliberately untouched —
    // rolling the whole mitt moves the palm off the grip, which is a different
    // (and wrong) thing. Mirrored per hand so both curl inward.
    float gripCurl = 0.5f;    // radians, per digit
};

// M4.8 gaze. The eye bones are leaves under the head, so this is a pure
// rotation per eye toward whatever the fighter is looking at (the camera, for
// now). Quantized to the 12 Hz pose grid like everything else that moves.
struct GazeParams {
    bool track = true;         // eyes follow the camera as it orbits
    float maxAngle = 1.5708f;  // clamp cone off the head's forward, radians
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

    // M4.7 sword prop + floating-hand IK (debug-controlled hilt transform).
    SwordParams sword;
    HandParams hands;
    GazeParams gaze;
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
