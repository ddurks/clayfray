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

    // ---- slice sploot (M4.6 ledger, sword side) ----
    // A SLICE ejects ONE gob carrying the whole volume the cut removed,
    // launched as the blade leaves the body — not the ledger's default
    // dribble of 2..35 ml pellets, which reads as a sneeze rather than a
    // wound. The brush keeps the dribble; this is sword-only.
    bool sliceGob = true;    // off = blade carves dribble like the brush
    float sliceSpeed = 1.5f; // launch speed, m/s (the blade's shove)
    float sliceLift = 0.8f;  // extra +Y m/s so the blob arcs instead of skidding
    // 0 = fly purely along the blade's sweep, 1 = purely along the wound
    // normal (straight out of the cut). Between the two reads as clay thrown
    // off the edge of a moving blade.
    float sliceOut = 0.45f;
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
    // Which mitt axis threads the blade (0=x, 1=y, 2=z) and how far the mitt
    // is spun about it. Authoring-dependent, so these are dialled by eye in
    // the running app (ctl: hands.gripAxis / hands.gripRoll) rather than
    // derived — reasoning from the grab morph's delta axes got it wrong.
    int gripAxis = 2;
    float gripRoll = -1.5f;  // radians about the handle: where fingers point
    // Lateral offset per hand, in sword radii. ZERO now, and the reason it
    // used to be 2.5 is worth keeping: under the old bone rig the mitts sat on
    // OPPOSITE FACES of the blade, pushed half a blade-width to each side,
    // because nothing threaded the handle. The brush rig puts the blade
    // THROUGH the gap between the fingers (gripAxis), so both hands belong on
    // the blade's own axis and any offset just slides them off it.
    float gripSpread = 0.f;
    // Finger curl: rotates the thumb/finger subtrees ABOUT THE HANDLE AXIS so
    // the digits wrap onto the blade. The wrist is deliberately untouched —
    // rolling the whole mitt moves the palm off the grip, which is a different
    // (and wrong) thing. Mirrored per hand so both curl inward.
    float gripCurl = 0.5f;    // radians, per digit
};

// M4.8 gaze. The eye bones are leaves under the head, so this is a pure
// rotation per eye toward whatever the fighter is looking at (the camera, for
// now). Quantized to the 12 Hz pose grid like everything else that moves.
// Whether the ROOT translation steps on the 12 Hz pose grid like the rest of
// the stop-motion, or slides at 60 Hz.
//
// DEFAULT OFF, and the reason is worth keeping: trap 4 covers the POSE, and
// extending it to translation does not work at this frame rate. Real
// stop-motion animates on 2s at 24 fps, so a held position spans 2 frames.
// We present at 60, so it spans 5 — and a ~9 cm translation jump (1.1 m/s
// over 1/12 s) is nowhere near as forgiving as a held pose or a boil reseed.
// It reads as jumpy whichever way the camera is handled: chase the sim
// position and the subject jumps inside the frame, chase the stepped position
// and the whole world jumps. There is no framing that hides it.
//
// The cost of leaving it off is that walking cannot reuse frames (the root is
// a traced input that changes every frame), so motion runs at the raw frame
// cost. That is a rendering problem to solve in the renderer, not by making
// the movement look worse.
//
// It was flipped ON once, for that reuse cost, and flipped back after playing
// it. DON'T FLIP IT AGAIN ON A REUSE MEASUREMENT — reuse % is the wrong
// scoreboard here, and it is convincing:
//
//   Frame reuse does not produce images, it only makes DUPLICATE frames
//   cheap. Sliding, every frame is unique and costs the full trace: ~17
//   unique images/s. Stepped, the trace happens 12x/s and ~48 duplicates
//   ride along: the fps counter reads ~60 while the eye gets 12. Stepping
//   spends motion fidelity (17 -> 12 unique images/s) to buy a bigger
//   number. The reuse rate went 0% -> 73% and the game looked worse.
//
// Reuse is still worth having for what it was built for — a still camera,
// where the duplicates are duplicates of a frame nothing was changing anyway.
// It is not a lever to make motion cheaper. Motion gets cheaper by making the
// TRACE cheaper.
struct MotionParams {
    bool stepRoot = false;
};

struct GazeParams {
    bool track = true;         // eyes follow the camera as it orbits
    float maxAngle = 1.5708f;  // clamp cone off the head's forward, radians
};

// M-PERF: the affine body's procedural motion.
//
// This REPLACES the skeletal clips as the driver of the blob's shape. The
// animation the game wants is a squish-and-spring (idle) and a squish, hop and
// forward lean (walk) — that is one non-uniform scale plus a shear plus a
// translation, i.e. one matrix, i.e. nothing worth skinning per sample. The
// 45-track 'bounce'/'idle' clips still exist in the asset and still drive the
// 13-piece path (look.affineRig off), which is what keeps the A/B honest; they
// are simply not sampled while the affine rig is on.
//
// DETERMINISM: the spring is integrated ONLY on 12 Hz pose steps, at a dt read
// off frame.poseTime, with a fixed substep count and no RNG and no wall clock.
// So --replay reproduces it exactly, and — the reason it is on the pose grid
// rather than the frame clock — a standing fighter's uniforms stop changing
// between pose steps, which is what lets frame reuse keep firing (trap 4).
// Defaults tuned by simulating the integrator at the 12 Hz step it actually
// runs at (an impulse response at 60 Hz would be a different curve): they land
// the walk at about -19% squish / +4% stretch and the idle breath at about
// -9%, which is a claymation range rather than a jelly one.
struct RigParams {
    float squishK = 60.f;     // spring stiffness (rad/s)^2; period ~0.81 s
    float squishDamp = 4.5f;  // velocity damping; lower = more overshoot/bounce
    float squishKick = 2.4f;  // impulse per footfall
    float gaitHz = 2.2f;      // footfalls per second while moving
    float idleHz = 0.5f;      // breaths per second at rest
    float idleScale = 0.45f;  // idle impulse as a fraction of the walk's
    // Metres of lift per unit of STRETCH, walk only. Stretch peaks near +0.04,
    // so 0.45 is a ~2 cm hop on a 0.69 m body — a skip, not a leap.
    float hop = 0.45f;
    float widen = 0.5f;       // sideways bulge per unit of squish
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
    // FIXED trace resolution, overriding resScale when both are > 0. Frame
    // cost is per TRACED PIXEL, so a resScale-derived size makes two machines
    // incomparable the moment their windows or backing scales differ — and
    // "same window size" is not the same thing as "same traced pixels" on a
    // display that reports backing pixels. Pin this and the number means the
    // same thing everywhere. `--res WxH` sets it; the startup [res] line
    // always prints what is actually being traced, so a mismatch is visible
    // instead of inferred.
    int traceW = 0, traceH = 0; // 0,0 = derive from the window via resScale

    // M4-P0 animation: play the asset's first clip, looped, sampled on the
    // 12 Hz pose grid. Off = rest pose. (Drives marbles + shadow proxy; the
    // voxel body articulates in M4-P1.)
    bool animPlay = true;
    float animSpeed = 1.0f;

    // M4.6 conservation: carved clay sploots onto the arena instead of
    // vanishing. Off = the pre-conservation vanish behavior.
    bool conserveClay = true;

    // M-PERF: collapse articulation to an affine body + two rigid mitts.
    // Off = the M4-P1 13-piece inverse-LBS warp. Both live in one binary so
    // the pair can be benchmarked without a shader edit (a shader edit forces
    // a cold pipeline compile on the next launch, which has faked wins here
    // twice — see the benchmarking notes in CLAUDE.md).
    // CLAYFRAY_NO_AFFINE=1 forces it off regardless.
    bool affineRig = true;

    // M4.7 sword prop + floating-hand IK (debug-controlled hilt transform).
    SwordParams sword;
    HandParams hands;
    GazeParams gaze;
    MotionParams motion;
    RigParams rig;
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
