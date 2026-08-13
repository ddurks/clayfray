#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <string>

#include "asset.h"
#include "camera.h"
#include "gpu.h"
#include "params.h"
#include "renderer.h"
#include "ui.h"

namespace {

constexpr double kTickRate = 60.0;
constexpr double kTickDt = 1.0 / kTickRate;

// Deterministic gameplay core stub. Grows a real state machine in M5;
// for M0/M1 it only advances time.
struct GameState {
    uint64_t tickCount = 0;
    void tick() { tickCount++; }
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

int runHeadless(const std::string& outPath, int width, int height, int frames,
                double startTime, int aa, bool carveTest) {
    Gpu gpu;
    if (!gpu.init(nullptr)) return 1;
    Renderer renderer;
    if (!renderer.init(gpu, width, height)) return 1;
    if (!loadCharacterInto(renderer)) return 1;

    if (carveTest && std::getenv("CLAYFRAY_TEST_ADDSTRESS")) {
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
    } else if (carveTest && std::getenv("CLAYFRAY_TEST_NULLEDITS")) {
        // ops that touch nothing — but still trigger the per-edit JFA re-run
        for (int i = 0; i < 9; i++) {
            BrickEdit e;
            e.mode = 1;
            e.pos[0] = 0.f; e.pos[1] = 2.5f; e.pos[2] = 0.f; e.radius = 0.03f;
            renderer.queueBrickEdit(e);
        }
    } else if (carveTest) {
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
    double t = startTime;
    // one queued edit drains per frame; +80 covers the longest test script
    int total = frames + (carveTest ? 80 : 0);
    for (int i = 0; i < total; i++) {
        renderer.render(cam, look, makeFrameInfo(t, aa), nullptr, nullptr);
        // headless still needs the event pump or async readbacks (volume
        // ledger, capacity poll) starve until exit
        gpu.processEvents();
        t += kTickDt;
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
    if (renderer.traceMs() > 0.f) {
        std::printf("[gpu] trace %.2f ms  post %.2f ms (smoothed, %dx%d aa=%d)\n",
                    renderer.traceMs(), renderer.postMs(), width, height, aa);
    }
    if (!renderer.screenshot(outPath)) return 1;

    // Machine-checkable conservation gate: at any instant, carved clay is
    // accounted for as landed + airborne + owed. A nonzero residual means a
    // measurement or a gob was dropped — a real regression. Agents/CI can
    // gate on this exit code instead of eyeballing the ledger line.
    if (carveTest && look.conserveClay) {
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

int runWindowed(int exitAfterFrames) {
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
    if (!uiInit(window, gpu)) return 1;

    GameState game;
    OrbitCamera cam;
    LookParams look;
    BrushState brush;

    uint64_t prevNs = SDL_GetTicksNS();
    double accumulator = 0.0;
    float fps = 0.f;
    int frameCounter = 0;
    int screenshotCounter = 0;
    bool running = true;

    while (running) {
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

        uint64_t nowNs = SDL_GetTicksNS();
        double frameDt = (double)(nowNs - prevNs) * 1e-9;
        prevNs = nowNs;
        if (frameDt > 0.25) frameDt = 0.25; // debugger/stall clamp
        accumulator += frameDt;
        while (accumulator >= kTickDt) {
            game.tick();
            accumulator -= kTickDt;
        }
        fps = fps * 0.95f + (float)(1.0 / (frameDt > 1e-6 ? frameDt : 1e-6)) * 0.05f;

        if (++frameCounter % 30 == 0) renderer.reloadShadersIfChanged();
        if (exitAfterFrames > 0 && frameCounter >= exitAfterFrames) running = false;

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
            const float* p = renderer.pickPos();
            e.pos[0] = p[0];
            e.pos[1] = p[1];
            e.pos[2] = p[2];
            e.radius = brush.radius;
            e.color[0] = brush.color[0];
            e.color[1] = brush.color[1];
            e.color[2] = brush.color[2];
            renderer.queueBrickEdit(e);
        }

        bool wantScreenshot = false;
        uiNewFrame(look, cam, brush, fps, renderer.traceMs(), renderer.postMs(),
                   renderer.sploot(), wantScreenshot);

        wgpu::SurfaceTexture surfaceTex;
        gpu.surface.GetCurrentTexture(&surfaceTex);
        if (surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
            surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
            wgpu::TextureView view = surfaceTex.texture.CreateView();
            renderer.render(cam, look, makeFrameInfo(game.timeSeconds(), 1), view,
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
    }

    uiShutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string screenshotPath;
    int width = 1280, height = 720, frames = 8, aa = 2;
    int exitAfter = 0;
    bool carveTest = false;
    double startTime = 2.0;

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
            screenshotPath = next("--screenshot");
        } else if (arg == "--size") {
            if (std::sscanf(next("--size"), "%dx%d", &width, &height) != 2) {
                std::fprintf(stderr, "--size expects WxH\n");
                return 2;
            }
        } else if (arg == "--frames") {
            frames = std::atoi(next("--frames"));
        } else if (arg == "--time") {
            startTime = std::atof(next("--time"));
        } else if (arg == "--aa") {
            aa = std::atoi(next("--aa"));
        } else if (arg == "--exit-after") {
            exitAfter = std::atoi(next("--exit-after"));
        } else if (arg == "--carve-test") {
            carveTest = true;
        } else if (arg == "--character") {
            gCharacterPath = next("--character");
        } else {
            std::fprintf(stderr,
                         "usage: clayfray [--screenshot out.png] [--size WxH] "
                         "[--frames N] [--time T] [--aa N]\n");
            return 2;
        }
    }

    if (!screenshotPath.empty()) {
        return runHeadless(screenshotPath, width, height, frames, startTime, aa, carveTest);
    }
    return runWindowed(exitAfter);
}
