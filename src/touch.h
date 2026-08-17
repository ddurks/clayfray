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
// 1. **The controls are driven by REAL finger events, but the camera is not.**
//    SDL synthesises mouse events from the primary finger (SDL3's
//    SDL_HINT_TOUCH_MOUSE_EVENTS defaults on), which is what already makes
//    drag-orbit and the ImGui panel work under a thumb with no code at all.
//    Reimplementing orbit on top of SDL_EVENT_FINGER_* would have thrown that
//    away. So this module claims fingers only inside its own two zones, and
//    `engaged()` tells the caller to ignore the synthetic mouse for the
//    duration — otherwise a thumb on the stick would orbit the camera while it
//    walked. Consequence, and it is deliberate: you cannot orbit WHILE walking,
//    because SDL only synthesises from the primary finger and a second finger
//    elsewhere produces no mouse motion to orbit with.
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

    // True while a thumb is on the stick or the button. The caller suppresses
    // synthetic-mouse orbit and sculpt-drag while this holds — see note 1.
    bool engaged() const { return stickHeld_ || btnHeld_; }

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
};
