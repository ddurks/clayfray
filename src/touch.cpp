#include "touch.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <imgui.h>

#include "ui.h"

namespace {

// Everything is derived from the SHORT window edge, not from width. A phone
// held in portrait and the same phone in landscape must offer the same size of
// thumb target, and only the short edge tracks "how big is a thumb here".
// The clamps stop a tablet growing a dinner-plate stick and a small landscape
// window shrinking the button below a tappable size (~44 pt is the usual
// floor for a reliable touch target).
struct Layout {
    float stickR, knobR, btnR, margin;
    float homeX, homeY; // idle ring centre, bottom-left
    float btnX, btnY;   // action button centre, bottom-right
    float zoneX1, zoneY0;
};

Layout layoutFor(float w, float h) {
    const float u = std::min(w, h);
    Layout L{};
    L.stickR = std::clamp(0.155f * u, 46.f, 128.f);
    L.knobR = 0.44f * L.stickR;
    L.btnR = std::clamp(0.125f * u, 40.f, 104.f);
    L.margin = std::clamp(0.055f * u, 14.f, 44.f);
    L.homeX = L.margin + L.stickR;
    L.homeY = h - L.margin - L.stickR;
    L.btnX = w - L.margin - L.btnR;
    L.btnY = h - L.margin - L.btnR;
    // The stick zone is the bottom-LEFT region rather than the whole left half:
    // drag-orbit has to survive somewhere, and the top of the screen is the
    // part of the arena a thumb is not already sitting on.
    L.zoneX1 = 0.52f * w;
    L.zoneY0 = 0.42f * h;
    return L;
}

// The look-dev panel is drawn over the game, so it takes precedence over both
// controls wherever it overlaps them — a slider under your thumb should move,
// not walk the fighter. uiSetCompact() starts the panel collapsed on touch
// devices precisely so this almost never costs you the stick.
bool overPanel(float px, float py) {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    uiPanelRect(x0, y0, x1, y1);
    return px >= x0 && px <= x1 && py >= y0 && py <= y1;
}

bool hitButton(const Layout& L, float px, float py) {
    const float dx = px - L.btnX, dy = py - L.btnY;
    // 1.3x the drawn radius: the visual is the affordance, the hit box is
    // forgiving, which is how a button survives being pressed without looking.
    const float r = L.btnR * 1.3f;
    return dx * dx + dy * dy <= r * r && !overPanel(px, py);
}

bool inStickZone(const Layout& L, float px, float py) {
    return px < L.zoneX1 && py > L.zoneY0 && !overPanel(px, py);
}

constexpr ImU32 kInk = IM_COL32(238, 231, 216, 255); // bone, matches the film

ImU32 ink(int alpha) { return (kInk & 0x00FFFFFFu) | ((ImU32)alpha << 24); }

} // namespace

bool TouchControls::handleEvent(const SDL_Event& ev, float winW, float winH) {
    switch (ev.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        break;
    default:
        return false;
    }

    // A finger is proof of a touchscreen whatever the startup probe decided —
    // desktop Chrome's device emulation and convertible laptops both arrive
    // here without ever matching a coarse-pointer query.
    active = true;
    if (winW < 1.f || winH < 1.f) return false;

    const Layout L = layoutFor(winW, winH);
    const float px = ev.tfinger.x * winW;
    const float py = ev.tfinger.y * winH;
    const uint64_t id = (uint64_t)ev.tfinger.fingerID;

    if (ev.type == SDL_EVENT_FINGER_DOWN) {
        // Button first: it is the smaller target and it sits inside no other
        // zone, so testing it first costs nothing and removes an ordering
        // question if the two are ever moved closer together.
        if (!btnHeld_ && hitButton(L, px, py)) {
            btnHeld_ = true;
            btnFinger_ = id;
            swingEdge_ = true;
            return true;
        }
        if (!stickHeld_ && inStickZone(L, px, py)) {
            stickHeld_ = true;
            stickFinger_ = id;
            stickR_ = L.stickR;
            // The ring spawns exactly where the thumb landed and is NOT clamped
            // back on-screen. Clamping would offset the ring from the thumb,
            // which reads as the fighter walking the instant you touch down;
            // a ring clipped by the screen edge is the lesser evil and is what
            // every dynamic stick does. ImGui clips the draw for us.
            stickOx_ = px;
            stickOy_ = py;
            stickX_ = px;
            stickY_ = py;
            return true;
        }
        return false;
    }

    if (ev.type == SDL_EVENT_FINGER_MOTION) {
        if (stickHeld_ && id == stickFinger_) {
            stickX_ = px;
            stickY_ = py;
            return true;
        }
        // Swallowed, not ignored: the swing already fired on the press, but the
        // caller must not read this finger's drag as an orbit.
        if (btnHeld_ && id == btnFinger_) return true;
        return false;
    }

    // UP / CANCELED. A thumb that slides off the button still counts as held
    // until it lifts, so a sloppy press cannot re-arm the swing.
    if (stickHeld_ && id == stickFinger_) {
        stickHeld_ = false;
        return true;
    }
    if (btnHeld_ && id == btnFinger_) {
        btnHeld_ = false;
        return true;
    }
    return false;
}

void TouchControls::addMovement(float camAzimuth, float& moveX, float& moveZ) const {
    if (!stickHeld_) return;
    float dx = stickX_ - stickOx_, dy = stickY_ - stickOy_;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    // Deadzone on the THROW, not on the raw distance, so it scales with the
    // stick — a thumb resting still must not creep the fighter.
    if (len / stickR_ < 0.18f) return;

    // Direction only. GameState normalizes the move vector to full speed
    // anyway, so an analog magnitude here would be thrown away; this keeps the
    // thumb and WASD producing literally the same input rather than two paths
    // that could drift apart. (Analog walk speed is a GameState change, not a
    // touch one.)
    dx /= len;
    dy /= len;

    // Same basis as WASD: forward is away from the camera, so the fighter runs
    // into the screen whatever the orbit is. Screen y grows DOWN, so pushing
    // the thumb up (negative dy) is forward.
    const float fx = -std::sin(camAzimuth), fz = -std::cos(camAzimuth);
    const float rx = std::cos(camAzimuth), rz = -std::sin(camAzimuth);
    const float right = dx, fwd = -dy;
    moveX += rx * right + fx * fwd;
    moveZ += rz * right + fz * fwd;
}

bool TouchControls::takeSwing() {
    const bool s = swingEdge_;
    swingEdge_ = false;
    return s;
}

void TouchControls::draw() const {
    if (!active) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w < 1.f || h < 1.f) return;
    const Layout L = layoutFor(w, h);
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // --- stick -------------------------------------------------------------
    // Idle it sits at the home anchor as a ghost, so the control is DISCOVERABLE
    // before it is touched; on contact the ring jumps to the thumb.
    const float cx = stickHeld_ ? stickOx_ : L.homeX;
    const float cy = stickHeld_ ? stickOy_ : L.homeY;
    const float ringW = std::max(2.f, L.stickR * 0.045f);
    dl->AddCircle(ImVec2(cx, cy), L.stickR, ink(stickHeld_ ? 120 : 46), 0, ringW);

    float kx = cx, ky = cy;
    if (stickHeld_) {
        float dx = stickX_ - stickOx_, dy = stickY_ - stickOy_;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > L.stickR) { // knob rides the rim, it never leaves the ring
            dx = dx / len * L.stickR;
            dy = dy / len * L.stickR;
        }
        kx = cx + dx;
        ky = cy + dy;
    }
    dl->AddCircleFilled(ImVec2(kx, ky), L.knobR, ink(stickHeld_ ? 90 : 34));
    dl->AddCircle(ImVec2(kx, ky), L.knobR, ink(stickHeld_ ? 190 : 80), 0, ringW);

    // --- action button -----------------------------------------------------
    dl->AddCircleFilled(ImVec2(L.btnX, L.btnY), L.btnR, ink(btnHeld_ ? 105 : 36));
    dl->AddCircle(ImVec2(L.btnX, L.btnY), L.btnR, ink(btnHeld_ ? 220 : 96), 0,
                  std::max(2.f, L.btnR * 0.05f));
    const char* label = "SWING";
    const float fs = std::max(11.f, L.btnR * 0.38f);
    ImFont* font = ImGui::GetFont();
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
    dl->AddText(font, fs, ImVec2(L.btnX - ts.x * 0.5f, L.btnY - ts.y * 0.5f),
                ink(btnHeld_ ? 245 : 150), label);
}
