#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <string>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "asset.h"
#include "camera.h"
#include "ctl.h"
#include "gpu.h"
#include "params.h"
#include "renderer.h"
#include "ui.h"

namespace {

constexpr double kTickRate = 60.0;
constexpr double kTickDt = 1.0 / kTickRate;

// GPU backpressure for the headless loops (--serve and --replay/--screenshot).
// Both advance a FIXED 1/60 s per iteration on purpose — that determinism is
// what makes replays exact — so neither is paced by wall clock, and with no
// swapchain there is no Fifo Present to block on either. Unthrottled, the CPU
// encodes ~1400 frames/s while the GPU retires far fewer, Dawn's pending-
// submission memory grows without bound, and vkQueueSubmit dies with
// VK_ERROR_OUT_OF_DEVICE_MEMORY within seconds. Every MapAsync after that
// fails "[Device] is lost", which is what made serve-mode `shot` look like a
// readback bug. Capping the queue depth costs nothing and fixes it.
constexpr int kMaxFramesInFlight = 2;

// Frames per second over a wall-clock window.
//
// NOT an exponential average of 1/dt, which is what this used to be and which
// reads HIGH whenever frame times vary: averaging reciprocals weights cheap
// frames far more than expensive ones. Frame reuse makes dt strongly bimodal
// here — roughly two in three frames re-present the previous trace for
// almost nothing while the third pays for everything — so the cheap frames
// dominated and the HUD claimed 85 fps on a loop measured at 52. It looked
// right on Windows only because vsync pinned nearly every frame to 16.7 ms,
// which flattens the distribution and hides the bias.
//
// Counting frames over a fixed window is the statistic the benchmarking notes
// in CLAUDE.md ask for: wall clock per PRESENTED frame.
struct FpsMeter {
    double window = 0.0;
    int frames = 0;
    float value = 0.f;

    float tick(double dt) {
        window += dt;
        frames++;
        if (window >= 0.5) { // ~2 updates/s: steady enough to read
            value = (float)(frames / window);
            window = 0.0;
            frames = 0;
        }
        return value;
    }
};

// Deterministic gameplay core. M5 test harness: one fighter walks the arena
// under WASD and swings its sword on a keypress. Deterministic by the house
// rules — fixed 60 Hz tick, seeded RNG, no wall clock in here — so a journal
// replay reproduces a run exactly. (After a snapshot load the tick count is
// resynced to the restored sim time.)
struct GameState {
    uint64_t tickCount = 0;

    FighterPose fighter;
    float vel[3] = {0.f, 0.f, 0.f};

    // desired travel direction in WORLD space, latched from the keyboard each
    // frame (camera-relative, resolved by the caller). Zero = stand still.
    float moveX = 0.f, moveZ = 0.f;
    bool swingRequested = false;

    // swing: a random arc swept across the front over kSwingDur seconds
    bool swinging = false;
    float swingT = 0.f, swingFrom = 0.f, swingTo = 0.f, swingArc = 0.f;
    uint32_t rng = 0x9E3779B9u;

    static constexpr float kMaxSpeed = 1.1f;   // m/s
    static constexpr float kAccel = 7.0f;      // m/s^2
    static constexpr float kTurnRate = 7.0f;   // rad/s
    static constexpr float kMaxLean = 0.30f;   // rad at full speed
    // A flourished swing is SLOW enough to read as claymation and slow enough
    // that the cut substeps cover its sweep without a bigger op budget.
    static constexpr float kSwingDur = 0.80f;  // s
    static constexpr float kBobAmp = 0.012f;   // m, held-sword breathing
    static constexpr float kBobTilt = 0.05f;   // rad

    float rand01() {
        rng = rng * 1664525u + 1013904223u;
        return (float)((rng >> 8) & 0xFFFFFFu) / 16777216.f;
    }

    void tick() {
        tickCount++;
        const float dt = (float)kTickDt;

        float want[3] = {0.f, 0.f, 0.f};
        float dl = std::sqrt(moveX * moveX + moveZ * moveZ);
        if (dl > 1e-3f) {
            want[0] = moveX / dl * kMaxSpeed;
            want[2] = moveZ / dl * kMaxSpeed;
        }
        for (int a = 0; a < 3; a += 2) {
            float d = want[a] - vel[a];
            float m = kAccel * dt;
            vel[a] += std::max(-m, std::min(d, m));
        }
        fighter.pos[0] += vel[0] * dt;
        fighter.pos[2] += vel[2] * dt;

        float sp = std::sqrt(vel[0] * vel[0] + vel[2] * vel[2]);
        fighter.moving = sp > 0.10f;
        if (fighter.moving) {
            // face the way we are going, then tip into it: shortest-arc turn
            float target = std::atan2(vel[0], vel[2]);
            float d = target - fighter.yaw;
            while (d > 3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            float m = kTurnRate * dt;
            fighter.yaw += std::max(-m, std::min(d, m));
            // lean scales with speed, and eases off while still turning hard
            // so a pivot doesn't throw the body sideways
            float aim = kMaxLean * (sp / kMaxSpeed) * (1.f - std::min(std::fabs(d), 1.f));
            fighter.lean += (aim - fighter.lean) * std::min(1.f, 6.f * dt);
        } else {
            fighter.lean += (0.f - fighter.lean) * std::min(1.f, 6.f * dt);
        }

        if (swingRequested && !swinging) {
            swinging = true;
            swingT = 0.f;
            // a random arc ACROSS the front: start on one side, finish on the
            // other, with a randomized rise so repeats don't read identically
            float side = rand01() < 0.5f ? 1.f : -1.f;
            swingFrom = side * (1.05f + rand01() * 0.25f);
            swingTo = -side * (1.15f + rand01() * 0.30f);
            swingArc = 0.25f + rand01() * 0.35f;
        }
        swingRequested = false;
        if (swinging) {
            swingT += dt;
            if (swingT >= kSwingDur) swinging = false;
        }
    }

    // Sword offsets (character space) on top of the vertical guard pose.
    // Idle/moving: a slight bob so the held blade breathes. Swinging: a
    // three-beat flourish — wind up past the start, sweep flat and fast
    // across the front, then follow through and recover to vertical.
    // `poseT` is the 12 Hz quantized clock, so the bob steps with the
    // stop-motion instead of sliding at frame rate (CLAUDE.md trap 4).
    void swordOffset(float poseT, float& yaw, float& pitch, float& lift) const {
        // bob rides whether we are moving or not, a touch stronger on the move
        float amp = kBobAmp * (fighter.moving ? 1.6f : 1.0f);
        float rate = fighter.moving ? 7.5f : 3.0f;
        lift = amp * std::sin(poseT * rate);
        yaw = 0.f;
        pitch = kBobTilt * std::sin(poseT * rate * 0.5f);
        if (!swinging) return;

        float u = std::min(1.f, swingT / kSwingDur);
        const float kWind = 0.28f, kCut = 0.62f; // beat boundaries
        if (u < kWind) {
            // 1. wind up: rotate BACK past the start, blade still high
            float a = u / kWind;
            float e = a * a * (3.f - 2.f * a);
            yaw = swingFrom * e;
            pitch = -0.35f * e; // tip back over the shoulder
        } else if (u < kCut) {
            // 2. the cut: flat and fast across the front
            float a = (u - kWind) / (kCut - kWind);
            float e = a * a * (3.f - 2.f * a);
            yaw = swingFrom + (swingTo - swingFrom) * e;
            // Flatten to HORIZONTAL early in the beat and stay there, so the
            // blade sweeps flat across the whole strike instead of only
            // arriving horizontal at the end of it.
            float pe = std::min(1.f, a / 0.30f);
            pe = pe * pe * (3.f - 2.f * pe);
            pitch = -0.35f + (-1.5708f + 0.35f) * pe;
            // ride the flat sweep up to chest height, where an opponent's
            // torso actually is — at the hilt's resting height it scythes
            // past their shins
            lift += 0.16f * pe;
        } else {
            // 3. follow through and recover to the vertical guard
            float a = (u - kCut) / (1.f - kCut);
            float e = a * a * (3.f - 2.f * a);
            yaw = swingTo * (1.f - e) + swingTo * 0.35f * (1.f - e);
            pitch = -1.5708f * (1.f - e) + swingArc * 0.25f * std::sin(3.14159f * a);
            lift += 0.16f * (1.f - e);
        }
    }

    double timeSeconds() const { return (double)tickCount * kTickDt; }
};

// The held sword breathes on the 12 Hz grid (GameState::swordOffset). Both
// the windowed loop and headless need it or they disagree about what the
// character looks like at rest — so it is applied through one helper rather
// than copied.
LookParams swordBobbed(GameState& g, const FighterPose& pose,
                       const LookParams& look, double t) {
    g.fighter = pose; // swordOffset reads `moving` for the bob's amplitude
    LookParams out = look;
    float sy, sp, lift;
    g.swordOffset((float)(std::floor(t * 12.0) / 12.0), sy, sp, lift);
    out.sword.yaw += sy;
    out.sword.pitch += sp;
    out.sword.pos[1] += lift;
    return out;
}

FrameInfo makeFrameInfo(double t, int aa) {
    FrameInfo f;
    f.time = (float)t;
    f.poseTime = (float)(std::floor(t * 12.0) / 12.0); // stop-motion pose steps
    f.grainFrame = (float)std::floor(t * 25.0);        // film frames (PAL)
    f.aaSamples = aa;
    return f;
}

long poseTickOf(double t) { return (long)std::floor(t * 12.0 + 1e-9); }

// break-on-condition (ctl `break ledger T`): pause instead of exiting so
// the state is inspectable via stats/shot/snap. One-shot; re-arm to renew.
// Only ctl can arm it, so it goes where ctl goes.
void checkBreak(CtlServer& ctl, const Renderer& renderer, SimClock& clock) {
#if !CLAYFRAY_DEV_TOOLS
    (void)ctl;
    (void)renderer;
    (void)clock;
#else
    if (ctl.breakLedgerTol < 0.f) return;
    const SplootStats& s = renderer.sploot();
    float res = (s.carved - (s.deposited + s.inFlight + s.debt)) * 1e6f;
    if (std::fabs(res) > ctl.breakLedgerTol) {
        clock.paused = true;
        std::fprintf(stderr,
                     "[break] ledger residual %.2f ml > %.2f ml — paused "
                     "(inspect via ctl; re-arm with `break ledger`)\n",
                     res, ctl.breakLedgerTol);
        ctl.breakLedgerTol = -1.f;
    }
#endif
}

std::string gCharacterPath; // --character; empty = built-in analytic fighter

bool loadCharacterInto(Renderer& renderer) {
    std::string path = gCharacterPath;
    if (path.empty()) {
        // the authored fighter IS the default when present; the analytic
        // blob remains the fallback (delete/rename the glb to get it back)
        std::ifstream probe("assets/fighter.glb");
        if (probe.good()) path = "assets/fighter.glb";
    }
    if (path.empty()) return true;
    CharacterAsset asset;
    if (!asset.load(path)) return false;
    renderer.setCharacter(std::move(asset));
    return true;
}

struct RunOpts {
    std::string screenshotPath;
    // pinned trace resolution from --res (0 = derive from window * resScale)
    int traceW = 0, traceH = 0;
    // aa is rays-per-pixel-AXIS: the tracer loops aa*aa full marches per pixel
    // (trace.wgsl), so cost is quadratic in it. 2 -> 1 measured 3.87x faster
    // (232.5 -> 60.1 ms/frame, 960x540, RX 5700 XT) for a difference only
    // visible under magnification on the eyes — the marbles are the one hard
    // high-contrast edge in the scene; clay silhouettes and the grain hide it.
    // Raise back to 2 for beauty/reference renders.
    // DEFAULT = the shipping internal resolution. A 1280x720 window at
    // resScale 0.5 traces 640x360, so benchmarking at 1280x720 measured a
    // configuration the game never runs — and by a wide margin (57.8 ms vs
    // 21.4 ms per moving frame). Any timing run without an explicit --size now
    // measures what ships. Pass --size for beauty/reference renders.
    int width = 640, height = 360, frames = 8, aa = 1;
    double startTime = 2.0;
    // headless camera override (radians / meters); NaN = keep the default.
    // Inspecting an artifact from another angle otherwise needs serve mode.
    float camAz = NAN, camEl = NAN, camDist = NAN;
    bool carveTest = false;
    bool serve = false;          // headless, loop until ctl `quit`
    std::string replayPath;      // headless journal replay
    std::string loadName;        // snapshot to restore at launch
    int exitAfter = 0;           // windowed smoke test
};

#if CLAYFRAY_DEV_TOOLS
// Every headless mode (--screenshot, --carve-test, --serve, --replay) is a
// desktop harness: no swapchain, a fixed 1/60 s step regardless of wall clock,
// and GPU backpressure held by blocking on the queue. A browser has none of
// those levers — it hands you a canvas and a frame callback — so the web
// target builds the windowed path only.
int runHeadless(const RunOpts& o) {
    Gpu gpu;
    if (!gpu.init(nullptr)) return 1;
    Renderer renderer;
    if (!renderer.init(gpu, o.width, o.height)) return 1;
    if (!loadCharacterInto(renderer)) return 1;
    // player 2: an identical fighter standing in front, facing the hero
    renderer.addPlayer(FighterPose{{1.15f, 0.f, 0.25f}, 3.14159f, 0.f, false});

    if (o.carveTest && std::getenv("CLAYFRAY_TEST_ADDSTRESS")) {
        // far-from-body adds: pool stress + volume-boundary rejection test
        for (int i = 0; i < 60; i++) {
            BrickEdit e;
            e.mode = 2;
            float a = 0.35f + 0.09f * i; // arc marching outward, exits volume late
            e.pos[0] = 0.30f + 0.30f * std::cos(a * 2.1f);
            e.pos[1] = 0.25f + 0.012f * i;
            e.pos[2] = 0.30f + 0.30f * std::sin(a * 2.1f);
            e.radius = 0.04f;
            e.color[0] = 0.72f; e.color[1] = 0.45f; e.color[2] = 0.40f;
            renderer.queueBrickEdit(e);
        }
    } else if (o.carveTest && std::getenv("CLAYFRAY_TEST_NULLEDITS")) {
        // ops that touch nothing — but still trigger the per-edit JFA re-run
        for (int i = 0; i < 9; i++) {
            BrickEdit e;
            e.mode = 1;
            e.pos[0] = 0.f; e.pos[1] = 2.5f; e.pos[2] = 0.f; e.radius = 0.03f;
            renderer.queueBrickEdit(e);
        }
    } else if (o.carveTest) {
        // scripted edits that exercise classify/alloc/fill/free/JFA without
        // interaction: carve gouges, a slice-like drag, and an added blob.
        // Coordinates target the imported fighter (body aabb ~ +-0.64 x,
        // 0..0.69 y, +-0.17 z) — the original set was authored for the taller
        // analytic blob and carved air, which M4.6's ledger exposed.
        BrickEdit e;
        e.mode = 1;
        e.pos[0] = 0.02f; e.pos[1] = 0.42f; e.pos[2] = 0.12f; e.radius = 0.07f;
        renderer.queueBrickEdit(e); // chest crater
        e.pos[0] = -0.08f; e.pos[1] = 0.60f; e.pos[2] = 0.08f; e.radius = 0.055f;
        renderer.queueBrickEdit(e); // head gouge
        for (int i = 0; i < 6; i++) { // diagonal slice-ish drag across the arm
            e.pos[0] = 0.26f + 0.05f * i;
            e.pos[1] = 0.50f - 0.04f * i;
            e.pos[2] = 0.04f;
            e.radius = 0.035f;
            renderer.queueBrickEdit(e);
        }
        BrickEdit add;
        add.mode = 2;
        add.pos[0] = -0.15f; add.pos[1] = 0.32f; add.pos[2] = 0.16f; add.radius = 0.05f;
        add.color[0] = 0.72f; add.color[1] = 0.45f; add.color[2] = 0.40f;
        renderer.queueBrickEdit(add); // terracotta blob stuck on the belly
    }

    OrbitCamera cam;
    if (!std::isnan(o.camAz)) cam.azimuth = o.camAz;
    if (!std::isnan(o.camEl)) cam.elevation = o.camEl;
    if (!std::isnan(o.camDist)) cam.distance = o.camDist;
    LookParams look;
    look.traceW = o.traceW;
    look.traceH = o.traceH;
    if (std::getenv("CLAYFRAY_DEBUG_FLAT")) {
        look.aoStrength = 0.f;
        look.detailAmount = 0.f;
        look.shadowSoft = 32.f;
    }
    if (std::getenv("CLAYFRAY_DEBUG_NORMALS")) look.debugMode = 1.f;
    if (std::getenv("CLAYFRAY_NO_ANIM")) look.animPlay = false;
    if (std::getenv("CLAYFRAY_DEBUG_FLATALBEDO")) look.debugMode = 2.f;
    if (std::getenv("CLAYFRAY_DEBUG_GRAD")) look.debugMode = 3.f;
    if (const char* k = std::getenv("CLAYFRAY_SHADOWK")) look.shadowSoft = (float)atof(k);
    if (const char* a = std::getenv("CLAYFRAY_AO")) look.aoStrength = (float)atof(a);
    if (const char* d = std::getenv("CLAYFRAY_DETAIL")) look.detailAmount = (float)atof(d);

    double t = o.startTime;
    SimClock clock;
    bool quit = false;
    float fps = 0.f;
    FpsMeter fpsMeter;
    CtlServer ctl;
    // headless has no keyboard, so locomotion is whatever ctl/replay sets
    FighterPose fighter;
    // The idle sword bob lives in GameState::swordOffset, and until now only
    // frameOnce (windowed + web) applied it — so headless renders drew a
    // fighter holding a DEAD sword. That makes --screenshot disagree with the
    // running game, which is exactly what look-dev and the GIF read from.
    // Headless has no keyboard so nothing ever swings; this contributes the
    // bob only, driven by the ctl/replay pose.
    GameState bobState;
    CtlRefs refs;
    refs.look = &look;
    refs.cam = &cam;
    refs.renderer = &renderer;
    refs.clock = &clock;
    refs.simT = &t;
    refs.fps = &fps;
    refs.wantQuit = &quit;
    refs.fighter = &fighter;
    refs.charPath = gCharacterPath;
    ctl.init(o.serve ? "ctl" : "", refs);

    std::vector<JournalEntry> journal;
    size_t ji = 0;
    if (!o.replayPath.empty() && !loadJournal(o.replayPath, journal)) return 2;
    if (!o.loadName.empty() &&
        !renderer.loadSnapshot(snapFilePath(o.loadName), &t, gCharacterPath)) {
        return 2;
    }

    // probe/pickuv are ctl verbs, so they must work in REPLAY too, not just
    // serve — a journal that asserts what is under the cursor is the only way
    // to regression-test the pick path headlessly. One 1-workgroup dispatch.
    renderer.setAlwaysPick(true);

    if (o.serve) {
        std::printf("[ctl] serving on ctl/ at %dx%d aa=%d — drive with "
                    "tools/ctl.sh, stop with `quit`\n",
                    o.width, o.height, o.aa);
        int warm = 3; // render the bake/import + first visible frames
        int inFlight = 0;
        while (!quit) {
            long tick = poseTickOf(t);
            ctl.poll(tick);
            int n = clock.ticksToRun(kTickDt);
            if (n > 15) n = 15;
            bool active = n > 0 || ctl.activity() || warm > 0 ||
                          renderer.brick().hasPendingWork();
            if (active) {
                if (warm > 0) warm--;
                t += n * kTickDt;
                auto t0 = std::chrono::steady_clock::now();
                renderer.setFighter(fighter);
                renderer.render(cam, swordBobbed(bobState, fighter, look, t),
                                makeFrameInfo(t, o.aa), nullptr, nullptr);
                gpu.processEvents();
                if (++inFlight >= kMaxFramesInFlight) {
                    gpu.waitForGpu();
                    inFlight = 0;
                }
                double dt = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
                fps = fpsMeter.tick(dt);
            } else {
                // idle: keep async maps, the inbox, and the ledger alive
                // without burning the GPU (a paused session would otherwise
                // never absorb an in-flight measurement into stats)
                gpu.processEvents();
                renderer.pumpLedger();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ctl.finishFrame();
            checkBreak(ctl, renderer, clock);
        }
        renderer.syncMeasurements();
        return 0;
    }

    // one queued edit drains per frame; +80 covers the longest test script
    int total = o.frames + (o.carveTest ? 80 : 0);
    if (!journal.empty()) {
        // enough frames to reach one pose tick past the last journal entry
        double tEnd = (double)(journal.back().tick + 1) / 12.0;
        total += std::max(0, (int)std::ceil((tEnd - o.startTime) * kTickRate) + 1);
    }
    int inFlight = 0;
    for (int i = 0; i < total; i++) {
        long tick = poseTickOf(t);
        while (ji < journal.size() && journal[ji].tick <= tick) {
            std::string resp;
            if (!ctl.execute(journal[ji].cmd, tick, resp)) {
                std::fprintf(stderr, "[replay] entry %zu failed: %s", ji + 1,
                             resp.c_str());
            }
            ji++;
        }
        renderer.setFighter(fighter);
        renderer.render(cam, swordBobbed(bobState, fighter, look, t),
                        makeFrameInfo(t, o.aa), nullptr, nullptr);
        // headless still needs the event pump or async readbacks (volume
        // ledger, capacity poll) starve until exit
        gpu.processEvents();
        if (++inFlight >= kMaxFramesInFlight) {
            gpu.waitForGpu();
            inFlight = 0;
        }
        // replay is the regression harness: pin measurement arrival to the
        // frame after its edit so reruns are bit-for-bit repeatable
        if (!journal.empty()) renderer.syncMeasurements();
        ctl.finishFrame(); // flush journal `shot` lines
        t += kTickDt;
    }
    if (ji < journal.size()) {
        std::fprintf(stderr, "[replay] %zu entr(y/ies) past end of run\n",
                     journal.size() - ji);
    }
    if (std::getenv("CLAYFRAY_DEBUG_STATS")) {
        renderer.brick().debugStats("post-render");
        renderer.brick().debugScanField();
    }
    const SplootStats& s = renderer.sploot();
    if (std::getenv("CLAYFRAY_DEBUG_LEDGER")) {
        std::printf("[sploot] final: carved %.1f ml, landed %.1f ml, "
                    "in flight %.1f ml (%d gobs), owed %.1f ml\n",
                    s.carved * 1e6f, s.deposited * 1e6f, s.inFlight * 1e6f, s.gobs,
                    s.debt * 1e6f);
    }
    std::printf("[reuse] traced %llu of %llu frames (%.1f%% skipped)\n",
                (unsigned long long)renderer.framesTraced(),
                (unsigned long long)renderer.framesPresented(),
                renderer.framesPresented()
                    ? 100.0 * (1.0 - (double)renderer.framesTraced() /
                                         (double)renderer.framesPresented())
                    : 0.0);
    if (renderer.traceMs() > 0.f) {
        std::printf("[gpu] trace %.2f ms  post %.2f ms (smoothed, %dx%d aa=%d)\n",
                    renderer.traceMs(), renderer.postMs(), o.width, o.height, o.aa);
    }
    if (!o.screenshotPath.empty() && !renderer.screenshot(o.screenshotPath)) return 1;

    // Machine-checkable conservation gate: at any instant, carved clay is
    // accounted for as landed + airborne + owed. A nonzero residual means a
    // measurement or a gob was dropped — a real regression. Agents/CI can
    // gate on this exit code instead of eyeballing the ledger line.
    if ((o.carveTest || !journal.empty()) && look.conserveClay) {
        float residual = s.carved - (s.deposited + s.inFlight + s.debt);
        float tol = std::max(1e-6f, s.carved * 0.01f); // 1 ml or 1% of carved
        if (std::fabs(residual) > tol) {
            std::fprintf(stderr,
                         "[sploot] CONSERVATION VIOLATION: carved %.1f ml != "
                         "landed %.1f + inflight %.1f + owed %.1f (residual "
                         "%.2f ml, tol %.2f ml)\n",
                         s.carved * 1e6f, s.deposited * 1e6f, s.inFlight * 1e6f,
                         s.debt * 1e6f, residual * 1e6f, tol * 1e6f);
            return 3;
        }
    }
    return 0;
}
#endif // CLAYFRAY_DEV_TOOLS

// Everything one frame of the windowed app touches, in one object with a
// STABLE address.
//
// Native could keep these as locals in runWindowed — the `while` loop is right
// there. A browser cannot: it owns the loop, calls us back one frame at a
// time, and runWindowed has long since returned by the first frame. So the
// state is heap-allocated and both platforms drive the same frameOnce() over
// it. Stable address matters twice over: CtlRefs stores raw pointers into
// these members, and the emscripten callback holds a void* to the whole thing.
struct AppState {
    RunOpts o;
    SDL_Window* window = nullptr;
    Gpu gpu;
    Renderer renderer;
    LookParams look;
    GameState game;
    OrbitCamera cam;
    BrushState brush;
    SimClock clock;
    CtlServer ctl;

    double simT = 0.0;
    bool ctlQuit = false;
    bool running = true;
    float fps = 0.f;
    FpsMeter fpsMeter;

    int pw = 0, ph = 0;            // backing (pixel) size of the window
    long lastCamPose = -1;
    uint64_t lastTraced = 0, lastPresented = 0;
    float reuseSkipPct = 0.f;
    uint64_t prevNs = 0;
    int frameCounter = 0;
    int screenshotCounter = 0;
#ifdef __EMSCRIPTEN__
    // last canvas CSS size we resized to, so the poll below reacts to the PAGE
    // changing size and not to its own previous write (which would oscillate)
    int lastCssW = 0, lastCssH = 0;
#endif
};

#ifdef __EMSCRIPTEN__
// The canvas decides the resolution on web, not a hardcoded 1280x720: the
// element is laid out by the page's CSS and may be any size at all.
//
// DPR is REPORTED but deliberately NOT multiplied into the backing store, for
// the same reason SDL_CreateWindow does not pass SDL_WINDOW_HIGH_PIXEL_DENSITY
// on the desktop: tracing at retina density quadruples the traced pixels for
// ~4x the frame time, and the film grain hides the difference. Reporting it
// anyway is what keeps the `[res]` line honest — a 2x display genuinely is
// showing an upscaled image, and that must be visible in the log rather than
// inferred.
bool webCanvasCssSize(int& w, int& h, double& dpr) {
    double cw = 0, ch = 0;
    dpr = emscripten_get_device_pixel_ratio();
    if (emscripten_get_element_css_size("#canvas", &cw, &ch) != EMSCRIPTEN_RESULT_SUCCESS)
        return false;
    if (cw < 1 || ch < 1) return false;
    w = (int)cw;
    h = (int)ch;
    return true;
}
#endif

// Post-GPU startup: everything that needs a device. Split out of runWindowed
// because on web it runs from the device callback, a browser task or two after
// runWindowed returned (see Gpu::initAsync).
bool appStartAfterGpu(AppState& s) {
    const RunOpts& o = s.o;
    SDL_GetWindowSizeInPixels(s.window, &s.pw, &s.ph);
    s.gpu.configureSurface(s.pw, s.ph);

    LookParams& look = s.look;
    look.traceW = o.traceW;
    look.traceH = o.traceH;
    // Trace size is decided ONCE here and reported, rather than drifting in
    // from the every-10-frames resize below: the first frames would otherwise
    // trace at full window size and a --res run would not honour the flag
    // until frame 10.
    const int traceW0 = look.traceW > 0 ? look.traceW
                                        : std::max(160, (int)(s.pw * look.resScale));
    const int traceH0 = look.traceH > 0 ? look.traceH
                                        : std::max(90, (int)(s.ph * look.resScale));
    {
        // ALWAYS report what is actually traced. Frame cost is per traced
        // pixel, and the window size does not tell you that: SDL reports
        // BACKING pixels, which differ from the logical size on a scaled
        // display, and resScale then divides them. Two machines "both at
        // 1280x720" can be tracing 4x different pixel counts, which silently
        // invalidates every timing comparison between them.
        //
        // The browser adds a third way to be wrong — the canvas is sized by
        // the page's CSS and the display has its own pixel ratio — so web
        // appends the DPR. Without it, "960x540 backing" on a 2x laptop reads
        // as if it were 1920x1080 of real pixels when it is an upscale.
        int lw = 0, lh = 0;
        SDL_GetWindowSize(s.window, &lw, &lh);
        char note[64] = "";
#ifdef __EMSCRIPTEN__
        std::snprintf(note, sizeof(note), ", canvas dpr %.2f (not applied)",
                      emscripten_get_device_pixel_ratio());
#endif
        std::printf("[res] window %dx%d logical, %dx%d backing%s -> TRACING %dx%d%s\n",
                    lw, lh, s.pw, s.ph, note, traceW0, traceH0,
                    look.traceW > 0 ? " (pinned by --res)" : " (resScale)");
        std::fflush(stdout);
    }

    if (!s.renderer.init(s.gpu, traceW0, traceH0)) return false;
    if (!loadCharacterInto(s.renderer)) return false;
    // player 2: an identical fighter standing in front, facing the hero
    s.renderer.addPlayer(FighterPose{{1.15f, 0.f, 0.25f}, 3.14159f, 0.f, false});
    if (!uiInit(s.window, s.gpu)) return false;

    CtlRefs refs;
    refs.look = &s.look;
    refs.cam = &s.cam;
    refs.brush = &s.brush;
    refs.fighter = &s.game.fighter;
    refs.renderer = &s.renderer;
    refs.clock = &s.clock;
    refs.simT = &s.simT;
    refs.fps = &s.fps;
    refs.wantQuit = &s.ctlQuit;
    refs.charPath = gCharacterPath;
    s.ctl.init(CLAYFRAY_DEV_TOOLS ? "ctl" : "", refs);

#if CLAYFRAY_DEV_TOOLS
    if (!o.loadName.empty()) {
        if (!s.renderer.loadSnapshot(snapFilePath(o.loadName), &s.simT, gCharacterPath)) {
            return false;
        }
        s.game.tickCount = (uint64_t)std::llround(s.simT * kTickRate);
    }
#endif

    // A pipeline that failed to create leaves an invalid object that every
    // later pass silently no-ops on, so say so ONCE here rather than let the
    // user stare at a black canvas. The scope callbacks are async, so this
    // catches whatever has been delivered by now; the per-pipeline message
    // from GpuPipelineScope is the authoritative one either way.
    if (gpuAnyPipelineFailed()) {
        std::fprintf(stderr,
                     "[startup] at least one pipeline failed to create — the "
                     "render will be black or partial. See the [wgpu] lines "
                     "above.\n");
    }

    s.prevNs = SDL_GetTicksNS();
    return true;
}

// ONE frame. Native calls this from its `while`; the browser calls it from
// requestAnimationFrame. Nothing in here may block — see platform.h note 1.
void frameOnce(AppState& s) {
    // Local aliases keep the body below textually identical to the loop this
    // was hoisted out of, which is what makes the restructure reviewable.
    SDL_Window* window = s.window;
    Gpu& gpu = s.gpu;
    Renderer& renderer = s.renderer;
    LookParams& look = s.look;
    GameState& game = s.game;
    OrbitCamera& cam = s.cam;
    BrushState& brush = s.brush;
    SimClock& clock = s.clock;
    CtlServer& ctl = s.ctl;
    const RunOpts& o = s.o;
    bool& running = s.running;
    bool& ctlQuit = s.ctlQuit;
    int& pw = s.pw;
    int& ph = s.ph;
    double& simT = s.simT;
    float& fps = s.fps;
    FpsMeter& fpsMeter = s.fpsMeter;
    long& lastCamPose = s.lastCamPose;
    uint64_t& lastTraced = s.lastTraced;
    uint64_t& lastPresented = s.lastPresented;
    float& reuseSkipPct = s.reuseSkipPct;
    uint64_t& prevNs = s.prevNs;
    int& frameCounter = s.frameCounter;
    int& screenshotCounter = s.screenshotCounter;
    (void)ctlQuit;
    (void)screenshotCounter;

    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            uiProcessEvent(&ev);
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                SDL_GetWindowSizeInPixels(window, &pw, &ph);
                gpu.configureSurface(pw, ph);
                renderer.resize(pw, ph);
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                SDL_Gamepad* pad = SDL_OpenGamepad(ev.gdevice.which);
                SDL_Log("gamepad added: %s",
                        pad ? SDL_GetGamepadName(pad) : "(open failed)");
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED:
                SDL_Log("gamepad removed: id %u", (unsigned)ev.gdevice.which);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_1) brush.mode = 0;
                if (ev.key.key == SDLK_2) brush.mode = 1;
                if (ev.key.key == SDLK_3) brush.mode = 2;
                // SPACE swings. Edge-triggered (not repeat) so holding it
                // doesn't retrigger every frame mid-arc.
                if (ev.key.key == SDLK_SPACE && !ev.key.repeat)
                    game.swingRequested = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (!uiWantsMouse() && brush.mode == 0 &&
                    (ev.motion.state & SDL_BUTTON_LMASK)) {
                    cam.azimuth -= ev.motion.xrel * 0.005f;
                    cam.elevation += ev.motion.yrel * 0.005f;
                    if (cam.elevation < -0.35f) cam.elevation = -0.35f;
                    if (cam.elevation > 1.2f) cam.elevation = 1.2f;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (!uiWantsMouse()) {
                    cam.distance *= std::exp(-ev.wheel.y * 0.1f);
                    if (cam.distance < 0.8f) cam.distance = 0.8f;
                    if (cam.distance > 8.f) cam.distance = 8.f;
                }
                break;
            default:
                break;
            }
        }

        long tickNow = poseTickOf(simT);
        ctl.poll(tickNow); // before sim/render: same-frame set -> shot

        // WASD walks, camera-relative: forward is away from the camera, so
        // the fighter always runs "into the screen" whatever the orbit is.
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            float fx = -std::sin(cam.azimuth), fz = -std::cos(cam.azimuth);
            float rx = std::cos(cam.azimuth), rz = -std::sin(cam.azimuth);
            float mx = 0.f, mz = 0.f;
            if (keys[SDL_SCANCODE_W]) { mx += fx; mz += fz; }
            if (keys[SDL_SCANCODE_S]) { mx -= fx; mz -= fz; }
            if (keys[SDL_SCANCODE_D]) { mx += rx; mz += rz; }
            if (keys[SDL_SCANCODE_A]) { mx -= rx; mz -= rz; }
            game.moveX = mx;
            game.moveZ = mz;
        }

        uint64_t nowNs = SDL_GetTicksNS();
        double frameDt = (double)(nowNs - prevNs) * 1e-9;
        prevNs = nowNs;
        if (frameDt > 0.25) frameDt = 0.25; // debugger/stall clamp
        int ticks = clock.ticksToRun(frameDt);
        if (ticks > 15) ticks = 15;
        for (int k = 0; k < ticks; k++) game.tick();
        renderer.setFighter(game.fighter);
        simT += ticks * kTickDt;
        // Keep the walker framed — but on the SAME clock the body is drawn on.
        // Chasing the 60 Hz sim position while the body steps at 12 Hz slides
        // the camera against a stepping subject, which reads as jitter (worse
        // than either being stepped or both smooth), and it changed a traced
        // input every frame, which defeated frame reuse in exactly the case
        // root quantisation exists to fix. Runs after simT advances so it sees
        // the pose tick the renderer will use this frame.
        {
            const long camPose = poseTickOf(simT);
            const bool step = look.motion.stepRoot;
            if (!step || camPose != lastCamPose) {
                lastCamPose = camPose;
                // same easing, re-expressed per pose step when stepping
                const float k = step ? 0.29f : std::min(1.f, 4.f * (float)frameDt);
                cam.target.x += (game.fighter.pos[0] - cam.target.x) * k;
                cam.target.y += (game.fighter.pos[1] + 0.45f - cam.target.y) * k;
                cam.target.z += (game.fighter.pos[2] - cam.target.z) * k;
            }
        }
        fps = fpsMeter.tick(frameDt);

        frameCounter++;
#if CLAYFRAY_DEV_TOOLS
        // shader hot reload: a stat() sweep of the source tree's shaders/,
        // which on web would be a sweep of read-only preloaded MEMFS
        if (frameCounter % 30 == 0) renderer.reloadShadersIfChanged();
#endif
        if (o.exitAfter > 0 && frameCounter >= o.exitAfter) running = false;

        // internal resolution scale (throttled so slider drags don't thrash
        // target recreation). CONSTANT by design — a dynamic drop while the
        // view moved was tried and removed: resScale is already 0.5, so it
        // took the window to 0.31 of native for a frame rate the constant 0.5
        // already reaches, and the resolution switch itself was visible.
        if (frameCounter % 10 == 0) {
#ifdef __EMSCRIPTEN__
            // The page can resize the canvas at any time (window resize, a
            // responsive layout, devtools opening) and that arrives as a CSS
            // change, not as an SDL event we can rely on. Poll it on the same
            // throttle as the trace-size check.
            //
            // Compared against the last size WE acted on, never against the
            // current window size: SDL_SetWindowSize writes the canvas back,
            // so comparing the two would let a rounding difference oscillate
            // forever, resizing render targets every 10 frames.
            {
                int cw = 0, ch = 0;
                double dpr = 1.0;
                if (webCanvasCssSize(cw, ch, dpr) &&
                    (cw != s.lastCssW || ch != s.lastCssH)) {
                    s.lastCssW = cw;
                    s.lastCssH = ch;
                    SDL_SetWindowSize(window, cw, ch);
                    SDL_GetWindowSizeInPixels(window, &pw, &ph);
                    gpu.configureSurface(pw, ph);
                }
            }
#endif
            // A pinned size wins over resScale: frame cost is per traced
            // pixel, so this is what makes two machines comparable.
            int tw = look.traceW > 0 ? look.traceW
                                     : std::max(160, (int)(pw * look.resScale));
            int th = look.traceH > 0 ? look.traceH
                                     : std::max(90, (int)(ph * look.resScale));
            if (tw != renderer.width() || th != renderer.height()) renderer.resize(tw, th);
        }

        // sculpt: pick under the cursor every frame; apply while LMB held
        float mx = 0.f, my = 0.f;
        SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
        int ww = 0, wh = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        if (ww > 0 && wh > 0) renderer.setPickUV(mx / (float)ww, my / (float)wh);
        if (brush.mode != 0 && (buttons & SDL_BUTTON_LMASK) && !uiWantsMouse() &&
            renderer.pickValid()) {
            BrickEdit e;
            e.mode = brush.mode;
            // REST space, not world: the brick volume is authored in rest
            // space, so carving a fighter that has walked or posed away from
            // the origin needs the hit mapped back through the articulation.
            const float* p = renderer.pickRest();
            e.pos[0] = p[0];
            e.pos[1] = p[1];
            e.pos[2] = p[2];
            e.radius = brush.radius;
            e.color[0] = brush.color[0];
            e.color[1] = brush.color[1];
            e.color[2] = brush.color[2];
            // pick-resolved edit recorded so replay doesn't need a cursor
            BrickEdit resolved = renderer.queueBrickEdit(e);
            if (ctl.recording()) ctl.recordEdit(resolved, tickNow);
        }

        bool wantScreenshot = false;
        // reuse rate over a short window, so the panel reacts to what you are
        // doing right now rather than showing a lifetime average
        {
            uint64_t tr = renderer.framesTraced(), pr = renderer.framesPresented();
            uint64_t dT = tr - lastTraced, dP = pr - lastPresented;
            if (dP >= 30) {
                reuseSkipPct = 100.f * (1.f - (float)dT / (float)dP);
                lastTraced = tr;
                lastPresented = pr;
            }
        }
        uiNewFrame(look, cam, brush, fps, renderer.traceMs(), renderer.postMs(),
                   renderer.sploot(), reuseSkipPct, wantScreenshot);

        wgpu::SurfaceTexture surfaceTex;
        gpu.surface.GetCurrentTexture(&surfaceTex);
        if (surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
            surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
            wgpu::TextureView view = surfaceTex.texture.CreateView();
            // the swing rides ON TOP of the panel's hold pose, so the sliders
            // stay authoritative for the guard and the arc is a transient
            LookParams frameLook = look;
            float sy, sp, lift;
            game.swordOffset((float)(std::floor(simT * 12.0) / 12.0), sy, sp, lift);
            frameLook.sword.yaw += sy;
            frameLook.sword.pitch += sp;
            frameLook.sword.pos[1] += lift;
            renderer.render(cam, frameLook, makeFrameInfo(simT, 1), view,
                            [](wgpu::RenderPassEncoder& pass) { uiRender(pass); });
#ifndef __EMSCRIPTEN__
            // The browser presents the canvas itself when the rAF callback
            // returns, so there is nothing to call — and emdawnwebgpu makes
            // that explicit: wgpuSurfacePresent is a hard abort() there ("use
            // requestAnimationFrame via html5.h instead"), not a no-op. Same
            // shape as trap 9: browser-illegal calls kill the module rather
            // than failing softly.
            gpu.surface.Present();
#endif
        } else {
            // Skipped frame (e.g. mid-resize); ImGui frame must still be closed out.
            ImGui::Render();
        }

#if CLAYFRAY_DEV_TOOLS
        if (wantScreenshot) {
            char path[64];
            std::snprintf(path, sizeof(path), "lookdev/capture_%03d.png",
                          screenshotCounter++);
            renderer.screenshot(path);
        }
#else
        (void)wantScreenshot;
        (void)screenshotCounter;
#endif
        gpu.processEvents();
        ctl.finishFrame();
        checkBreak(ctl, renderer, clock);
    }
}

void appShutdown(AppState& s) {
    uiShutdown();
    SDL_DestroyWindow(s.window);
    SDL_Quit();
}

#ifdef __EMSCRIPTEN__
// The browser's frame callback. emscripten_set_main_loop_arg with fps 0 means
// requestAnimationFrame, i.e. the display's own cadence — which is also the
// backpressure that Gpu::waitForGpu provides for the headless desktop loops.
void webFrame(void* userData) {
    AppState& s = *static_cast<AppState*>(userData);
    frameOnce(s);
    if (!s.running || s.ctlQuit) {
        emscripten_cancel_main_loop();
        appShutdown(s);
    }
}
#endif

int runWindowed(const RunOpts& o) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Desktop opens a 1280x720 window; the browser has no such freedom — the
    // canvas is however big the page's CSS made it, and 1280x720 would either
    // overflow the layout or be silently stretched. Ask the element.
    int winW = 1280, winH = 720;
#ifdef __EMSCRIPTEN__
    {
        int cw = 0, ch = 0;
        double dpr = 1.0;
        if (webCanvasCssSize(cw, ch, dpr)) {
            winW = cw;
            winH = ch;
        } else {
            std::fprintf(stderr,
                         "[web] could not read #canvas CSS size; falling back "
                         "to %dx%d\n",
                         winW, winH);
        }
    }
#endif
    // no HIGH_PIXEL_DENSITY: retina-native quadruples the traced pixels for
    // ~4x the frame time, and the film look hides the difference
    SDL_Window* window = SDL_CreateWindow("clayfray", winW, winH, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    // Heap, not stack: on web this outlives runWindowed by the whole session
    // (see AppState). Native deletes it at the bottom; web never does, because
    // the tab closing is the only end of life there.
    AppState* s = new AppState();
    s->o = o;
    s->window = window;
#ifdef __EMSCRIPTEN__
    s->lastCssW = winW;
    s->lastCssH = winH;
#endif

#if CLAYFRAY_HAS_BLOCKING_GPU_WAIT
    // Native: initAsync completes synchronously, so this reads exactly like
    // the straight-line startup it replaced.
    bool gpuOk = false;
    s->gpu.initAsync(window, [&gpuOk](bool ok) { gpuOk = ok; });
    if (!gpuOk || !appStartAfterGpu(*s)) {
        delete s;
        return 1;
    }
    while (s->running && !s->ctlQuit) frameOnce(*s);
    appShutdown(*s);
    delete s;
    return 0;
#else
    // Browser: RETURN, don't loop. Everything downstream of the device runs
    // from the callback, and the frame loop is handed to the browser. Falling
    // off the end of main() here is correct and expected — the module stays
    // alive because emscripten_set_main_loop_arg registered a callback.
    s->gpu.initAsync(window, [s](bool ok) {
        if (!ok) {
            std::fprintf(stderr, "[web] GPU init failed; nothing will render\n");
            return;
        }
        if (!appStartAfterGpu(*s)) {
            std::fprintf(stderr, "[web] renderer init failed; nothing will render\n");
            return;
        }
        // fps 0 = requestAnimationFrame; simulate_infinite_loop false = let
        // runWindowed's caller return normally rather than throwing the
        // unwind exception, which would be caught as an error by the shell.
        emscripten_set_main_loop_arg(webFrame, s, 0, /*simulate_infinite_loop=*/false);
    });
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    // mintty/pipes fully buffer stdout, hiding startup progress for minutes
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    RunOpts o;
    bool sizeSet = false, aaSet = false;

    for (int i = 1; i < argc; i++) {
        auto arg = std::string(argv[i]);
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--screenshot") {
            o.screenshotPath = next("--screenshot");
        } else if (arg == "--size") {
            if (std::sscanf(next("--size"), "%dx%d", &o.width, &o.height) != 2) {
                std::fprintf(stderr, "--size expects WxH\n");
                return 2;
            }
            sizeSet = true;
        } else if (arg == "--frames") {
            o.frames = std::atoi(next("--frames"));
        } else if (arg == "--time") {
            o.startTime = std::atof(next("--time"));
        } else if (arg == "--cam") {
            // --cam AZ,EL,DIST (radians, radians, meters): inspect an artifact
            // from any angle headlessly instead of driving serve mode
            if (std::sscanf(next("--cam"), "%f,%f,%f", &o.camAz, &o.camEl,
                            &o.camDist) != 3) {
                std::fprintf(stderr, "--cam expects AZ,EL,DIST\n");
                return 2;
            }
        } else if (arg == "--res") {
            // Pin the TRACED resolution, ignoring window size and resScale.
            // The point is cross-machine comparability: `--res 640x360` costs
            // the same work everywhere, so a frame time from one box means
            // something on another.
            if (std::sscanf(next("--res"), "%dx%d", &o.traceW, &o.traceH) != 2) {
                std::fprintf(stderr, "--res expects WxH\n");
                return 2;
            }
        } else if (arg == "--aa") {
            o.aa = std::atoi(next("--aa"));
            aaSet = true;
        } else if (arg == "--exit-after") {
            o.exitAfter = std::atoi(next("--exit-after"));
        } else if (arg == "--carve-test") {
            o.carveTest = true;
        } else if (arg == "--character") {
            gCharacterPath = next("--character");
        } else if (arg == "--serve") {
            o.serve = true;
        } else if (arg == "--replay") {
            o.replayPath = next("--replay");
        } else if (arg == "--load") {
            o.loadName = next("--load");
        } else {
            std::fprintf(stderr,
                         "usage: clayfray [--screenshot out.png] [--size WxH] "
                         "[--frames N] [--time T] [--aa N] [--character f.glb]\n"
                         "  [--serve]        headless ctl session (tools/ctl.sh)\n"
                         "  [--replay f]     headless journal replay (deterministic)\n"
                         "  [--load name]    restore a snapshot at launch\n"
                         "  [--carve-test] [--exit-after N]\n");
            return 2;
        }
    }
    if (o.serve) {
        // serve is an interactive sim console: default to a fast trace
        if (!sizeSet) { o.width = 960; o.height = 540; }
        if (!aaSet) o.aa = 1;
    }

#if CLAYFRAY_DEV_TOOLS
    if (!o.screenshotPath.empty() || o.serve || !o.replayPath.empty()) {
        return runHeadless(o);
    }
#else
    // Flag PARSING stays identical on web so the two builds cannot drift over
    // what a flag means; only the modes that need a real OS are unreachable.
    if (!o.screenshotPath.empty() || o.serve || !o.replayPath.empty() ||
        !o.loadName.empty()) {
        std::fprintf(stderr,
                     "[web] headless modes and snapshots are desktop-only; "
                     "running windowed\n");
    }
#endif
    return runWindowed(o);
}
