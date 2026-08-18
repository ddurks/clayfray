#pragma once

#include <cstdint>

union SDL_Event;

// On-screen controls for the mobile web build: a left-thumb stick that walks
// the fighter and one right-thumb button that swings. Exactly the two inputs
// the M5 harness has (WASD + SPACE) and nothing more — a phone has no keyboard
// and no scroll wheel, so without these the web build is a screensaver.
//
// Three decisions worth knowing before you change anything here:
//
// 1. **The screen is split in half: BOTTOM walks, TOP looks.**
//    The stick and the swing button live in the bottom half; a drag anywhere
//    in the top half orbits the camera. Both are driven by REAL finger events.
//
//    That last part is a change, and it bought something specific. The camera
//    used to ride the synthetic mouse SDL generates from the primary finger
//    (SDL_HINT_TOUCH_MOUSE_EVENTS), which was free but carried a hard limit:
//    SDL synthesises from the PRIMARY finger only, so a second finger produced
//    no mouse motion and you could not orbit while walking. Claiming the look
//    zone as real touch removes that — the stick and the camera are now two
//    independent fingers, which is what a twin-stick layout has to be.
//
//    `engaged()` still tells the caller to ignore the synthetic mouse for the
//    duration, and it now covers the look finger too: without that, a look drag
//    would rotate the camera twice — once here and once through the mouse SDL
//    is still synthesising from the same finger.
//
// 2. **Layout is in ImGui screen coordinates, not normalized and not pixels.**
//    Finger events arrive normalized 0..1 over the window; ImGui draws in
//    logical window points. Everything below is converted to points at the
//    event boundary so hit-testing and drawing cannot disagree — a thumb that
//    lands visually on the button must hit it on a 3x-DPR phone too.
//
// 3. **The look-dev panel wins where it overlaps.** On a phone the panel is
//    most of the screen when open, so the stick zone yields to it (see
//    uiPanelRect). uiSetCompact() then starts it collapsed on touch devices so
//    that yielding almost never has to happen.
//
// Nothing here is web-only: it compiles and runs on the desktop unchanged, and
// a touchscreen laptop gets the same controls. It is simply inert until a
// finger shows up or CLAYFRAY_TOUCH=1 forces it on for testing with Chrome's
// device emulation.
struct TouchControls {
    // Drawn and consuming touches. Set at startup from a coarse-pointer probe
    // on web (so a phone sees the controls BEFORE it touches anything), and
    // latched on the first finger event anywhere else.
    bool active = false;

    // Feed every SDL event here. `winW`/`winH` are the LOGICAL window size
    // (SDL_GetWindowSize), which is the space ImGui draws in. Returns true if
    // the event belonged to a control, i.e. the caller should not also treat it
    // as a camera or sculpt input.
    bool handleEvent(const SDL_Event& ev, float winW, float winH);

    // True while a thumb is on the stick, the button or the look zone. The
    // caller suppresses synthetic-mouse orbit and sculpt-drag while this holds
    // — see note 1. The look finger MUST be in here: it is usually the primary
    // finger, so SDL is synthesising mouse motion from it, and letting that
    // through would apply every look drag twice.
    bool engaged() const { return stickHeld_ || btnHeld_ || lookHeld_; }

    // Camera orbit accumulated since the last call, in ImGui points, cleared
    // on read. Returns false when there is nothing pending, so the caller can
    // leave the camera completely alone on a frame with no look drag.
    bool takeLook(float& dx, float& dy);

    // Adds the stick's contribution to a camera-relative move vector, using the
    // same basis as WASD: screen-up is away from the camera whatever the orbit
    // is. Additive so a keyboard and a thumb can coexist without a mode.
    void addMovement(float camAzimuth, float& moveX, float& moveZ) const;

    // Edge-triggered: true exactly once per press of the action button, then
    // cleared. Matches SPACE's `!ev.key.repeat` so a held thumb does not
    // retrigger the swing mid-arc.
    bool takeSwing();

    // Draws the stick and the button into ImGui's foreground draw list. Must be
    // called between ImGui::NewFrame() and ImGui::Render() — i.e. after
    // uiNewFrame(). Foreground, not background: these are the game's controls
    // and a dev panel must never bury them.
    void draw() const;

  private:
    bool stickHeld_ = false;
    uint64_t stickFinger_ = 0;
    float stickOx_ = 0.f, stickOy_ = 0.f; // ring centre, points
    float stickX_ = 0.f, stickY_ = 0.f;   // thumb now, points

    // Ring radius latched at grab time, so addMovement() can normalize the
    // throw without asking ImGui for a display size mid-sim — and so a rotation
    // that resizes the window cannot rescale a stick already under a thumb.
    float stickR_ = 1.f;

    bool btnHeld_ = false;
    uint64_t btnFinger_ = 0;
    bool swingEdge_ = false;

    // Look drag. `lookAccum*` is a DELTA the caller drains, not a position:
    // the camera integrates it, so a frame that never reads it must not lose
    // the motion and a frame that reads it twice must not apply it twice.
    bool lookHeld_ = false;
    uint64_t lookFinger_ = 0;
    float lookX_ = 0.f, lookY_ = 0.f;
    float lookAccumX_ = 0.f, lookAccumY_ = 0.f;
};
