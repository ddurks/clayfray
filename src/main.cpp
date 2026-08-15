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
void checkBreak(CtlServer& ctl, const Renderer& renderer, SimClock& clock) {
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
    // aa is rays-per-pixel-AXIS: the tracer loops aa*aa full marches per pixel
    // (trace.wgsl), so cost is quadratic in it. 2 -> 1 measured 3.87x faster
    // (232.5 -> 60.1 ms/frame, 960x540, RX 5700 XT) for a difference only
    // visible under magnification on the eyes — the marbles are the one hard
    // high-contrast edge in the scene; clay silhouettes and the grain hide it.
    // Raise back to 2 for beauty/reference renders.
    int width = 1280, height = 720, frames = 8, aa = 1;
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
    CtlServer ctl;
    // headless has no keyboard, so locomotion is whatever ctl/replay sets
    FighterPose fighter;
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
                renderer.render(cam, look, makeFrameInfo(t, o.aa), nullptr, nullptr);
                gpu.processEvents();
                if (++inFlight >= kMaxFramesInFlight) {
                    gpu.waitForGpu();
                    inFlight = 0;
                }
                double dt = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
                if (dt > 1e-6) fps = fps * 0.9f + (float)(1.0 / dt) * 0.1f;
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
        renderer.render(cam, look, makeFrameInfo(t, o.aa), nullptr, nullptr);
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

int runWindowed(const RunOpts& o) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // no HIGH_PIXEL_DENSITY: retina-native quadruples the traced pixels for
    // ~4x the frame time, and the film look hides the difference
    SDL_Window* window = SDL_CreateWindow("clayfray", 1280, 720, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    Gpu gpu;
    if (!gpu.init(window)) return 1;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    gpu.configureSurface(pw, ph);

    Renderer renderer;
    if (!renderer.init(gpu, pw, ph)) return 1;
    if (!loadCharacterInto(renderer)) return 1;
    // player 2: an identical fighter standing in front, facing the hero
    renderer.addPlayer(FighterPose{{1.15f, 0.f, 0.25f}, 3.14159f, 0.f, false});
    if (!uiInit(window, gpu)) return 1;

    GameState game;
    OrbitCamera cam;
    LookParams look;
    BrushState brush;

    SimClock clock;
    double simT = 0.0;
    bool ctlQuit = false;
    float fps = 0.f;
    CtlServer ctl;
    CtlRefs refs;
    refs.look = &look;
    refs.cam = &cam;
    refs.brush = &brush;
    refs.fighter = &game.fighter;
    refs.renderer = &renderer;
    refs.clock = &clock;
    refs.simT = &simT;
    refs.fps = &fps;
    refs.wantQuit = &ctlQuit;
    refs.charPath = gCharacterPath;
    ctl.init("ctl", refs);

    if (!o.loadName.empty()) {
        if (!renderer.loadSnapshot(snapFilePath(o.loadName), &simT, gCharacterPath)) {
            return 2;
        }
        game.tickCount = (uint64_t)std::llround(simT * kTickRate);
    }

    long lastCamPose = -1;
    uint64_t lastTraced = 0, lastPresented = 0;
    float reuseSkipPct = 0.f;
    uint64_t prevNs = SDL_GetTicksNS();
    int frameCounter = 0;
    int screenshotCounter = 0;
    bool running = true;

    while (running && !ctlQuit) {
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
        fps = fps * 0.95f + (float)(1.0 / (frameDt > 1e-6 ? frameDt : 1e-6)) * 0.05f;

        if (++frameCounter % 30 == 0) renderer.reloadShadersIfChanged();
        if (o.exitAfter > 0 && frameCounter >= o.exitAfter) running = false;

        // internal resolution scale (throttled so slider drags don't thrash
        // target recreation)
        if (frameCounter % 10 == 0) {
            int tw = std::max(160, (int)(pw * look.resScale));
            int th = std::max(90, (int)(ph * look.resScale));
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
            gpu.surface.Present();
        } else {
            // Skipped frame (e.g. mid-resize); ImGui frame must still be closed out.
            ImGui::Render();
        }

        if (wantScreenshot) {
            char path[64];
            std::snprintf(path, sizeof(path), "lookdev/capture_%03d.png",
                          screenshotCounter++);
            renderer.screenshot(path);
        }
        gpu.processEvents();
        ctl.finishFrame();
        checkBreak(ctl, renderer, clock);
    }

    uiShutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
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

    if (!o.screenshotPath.empty() || o.serve || !o.replayPath.empty()) {
        return runHeadless(o);
    }
    return runWindowed(o);
}
