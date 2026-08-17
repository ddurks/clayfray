#pragma once
#include "camera.h"
#include "gpu.h"
#include "params.h"

union SDL_Event;

bool uiInit(SDL_Window* window, Gpu& gpu);
void uiProcessEvent(const SDL_Event* event);
bool uiWantsMouse();
// Builds the look-dev panel for this frame. Sets wantScreenshot when the
// user clicks the capture button. No longer takes the camera: drag-orbit and
// the wheel beat four sliders, and ctl `cam.*` covers the scripted case.
void uiNewFrame(LookParams& look, BrushState& brush, float fps,
                float gpuTraceMs, float gpuPostMs, const SplootStats& sploot,
                float reuseSkipPct, bool& wantScreenshot);
// Compact mode: the panel starts COLLAPSED. For touch devices, where an open
// look-dev panel is most of a phone screen and would sit on top of the thumb
// stick. FirstUseEver, so opening it is sticky within a session.
void uiSetCompact(bool compact);
// The panel's screen rect (ImGui coordinates) as of the last built frame, so
// touch controls can yield the region the panel is drawn over. Empty until the
// first uiNewFrame. One frame stale by construction — events are polled before
// the panel is built — which is harmless: the panel does not move on its own.
void uiPanelRect(float& x0, float& y0, float& x1, float& y1);
void uiRender(wgpu::RenderPassEncoder& pass);
void uiShutdown();
