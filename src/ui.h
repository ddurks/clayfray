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
void uiRender(wgpu::RenderPassEncoder& pass);
void uiShutdown();
