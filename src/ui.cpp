#include "ui.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_wgpu.h>

bool uiInit(SDL_Window* window, Gpu& gpu) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForOther(window)) return false;
    ImGui_ImplWGPU_InitInfo info{};
    info.Device = gpu.device.Get();
    info.NumFramesInFlight = 3;
    info.RenderTargetFormat = (WGPUTextureFormat)gpu.surfaceFormat;
    info.DepthStencilFormat = WGPUTextureFormat_Undefined;
    return ImGui_ImplWGPU_Init(&info);
}

void uiProcessEvent(const SDL_Event* event) {
    ImGui_ImplSDL3_ProcessEvent(event);
}

bool uiWantsMouse() {
    return ImGui::GetIO().WantCaptureMouse;
}

void uiNewFrame(LookParams& look, BrushState& brush, float fps,
                float gpuTraceMs, float gpuPostMs, const SplootStats& sploot,
                float reuseSkipPct, bool& wantScreenshot) {
    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
    // WHAT BELONGS HERE: knobs whose right value is still a judgement call you
    // make by looking. Everything settled has been cut — the rig's grip and
    // alignment, the sword's placement, the clip controls for clips this asset
    // does not have. None of it is LOST: ctl still exposes every LookParams
    // field by struct path (`tools/ctl.sh "set hands.gripRoll 0.4"`), which is
    // the better home for a value you set once and never touch again.
    ImGui::Begin("look-dev");
    ImGui::Text("%.1f fps", fps);
    // 12 Hz frame reuse: how many of the last frames skipped the trace. Watch
    // it fall to 0 while orbiting (new pixels, nothing to reuse) and climb
    // back when the camera settles.
    ImGui::SameLine();
    ImGui::TextDisabled("| %.0f%% reused", reuseSkipPct);
    if (ImGui::Button("capture -> lookdev/")) wantScreenshot = true;

    ImGui::Checkbox("12 Hz root (stop-motion walk)", &look.motion.stepRoot);
    if (ImGui::CollapsingHeader("sculpt", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::RadioButton("orbit (1)", &brush.mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("carve (2)", &brush.mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("add (3)", &brush.mode, 2);
        ImGui::SliderFloat("brush radius", &brush.radius, 0.01f, 0.15f);
        ImGui::ColorEdit3("clay color", brush.color);
    }

    if (ImGui::CollapsingHeader("animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        // OFF draws the rest volume unposed — all three brushes side by side
        // where they were authored. That is the "is the rig wrong or is the
        // VOLUME wrong?" A/B, not a rig you would ship. (The clip play/speed
        // sliders went with it: this asset has no clips, so they were two
        // controls for nothing. `look.animPlay` survives in ctl for a
        // hypothetical rigged asset.)
        ImGui::Checkbox("affine rig (off = unposed rest volume)", &look.affineRig);
        if (look.affineRig) {
            RigParams& r = look.rig;
            ImGui::SliderFloat("squish stiffness", &r.squishK, 10.f, 200.f);
            ImGui::SliderFloat("squish damping", &r.squishDamp, 0.5f, 20.f);
            ImGui::SliderFloat("squish kick", &r.squishKick, 0.f, 4.f);
            ImGui::SliderFloat("gait Hz (walk)", &r.gaitHz, 0.2f, 6.f);
            ImGui::SliderFloat("idle Hz", &r.idleHz, 0.05f, 2.f);
            ImGui::SliderFloat("idle scale", &r.idleScale, 0.f, 1.f);
            ImGui::SliderFloat("hop (m per stretch)", &r.hop, 0.f, 2.f);
            ImGui::SliderFloat("widen", &r.widen, 0.f, 1.5f);
        }
    }

    if (ImGui::CollapsingHeader("sword / eyes", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Two toggles, because only two of these change what the app DOES.
        // The sword's placement (hilt pos/yaw/pitch/length/radius/glow) is
        // driven by the harness's guard-and-swing pose, so dragging it here
        // fought the animation; and the mitt alignment (reach, palm, orient,
        // grip axis/roll/spread) was tuned once against the authored hand and
        // has not moved since. All still live in ctl by struct path.
        ImGui::Checkbox("hold sword (mitts take the grab brush)", &look.sword.enabled);
        ImGui::Checkbox("floating hands follow grips", &look.hands.ik);
        ImGui::Checkbox("eyes track camera", &look.gaze.track);
    }

    if (ImGui::CollapsingHeader("sploot", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("conserve carved clay", &look.conserveClay);
        ImGui::Text("carved %.0f ml   landed %.0f ml", sploot.carved * 1e6f,
                    sploot.deposited * 1e6f);
        ImGui::Text("in flight %d gob(s), %.1f ml   owed %.1f ml", sploot.gobs,
                    sploot.inFlight * 1e6f, sploot.debt * 1e6f);
    }

    if (ImGui::CollapsingHeader("key light", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("position", look.keyPos, 0.05f, -6.f, 6.f);
        ImGui::SliderFloat("intensity", &look.keyIntensity, 0.f, 20.f);
        ImGui::ColorEdit3("color", look.keyColor);
        ImGui::SliderFloat("falloff", &look.keyFalloff, 0.f, 1.f);
    }
    if (ImGui::CollapsingHeader("rim / ambient")) {
        // Rim is OFF by default now. Its direction and colour only matter once
        // the intensity is non-zero, so they went to ctl rather than sit here
        // doing nothing.
        ImGui::SliderFloat("rim intensity", &look.rimIntensity, 0.f, 2.f);
        ImGui::ColorEdit3("ambient", look.ambient);
        ImGui::SliderFloat("ao strength", &look.aoStrength, 0.f, 2.f);
    }
    if (ImGui::CollapsingHeader("clay", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("detail", &look.detailAmount, 0.f, 2.f);
        ImGui::SliderFloat("boil", &look.boilAmount, 0.f, 2.f);
        ImGui::SliderFloat("shadow soft k", &look.shadowSoft, 2.f, 32.f);
        ImGui::SliderFloat("sheen", &look.sheenAmount, 0.f, 0.06f, "%.3f");
    }
    if (ImGui::CollapsingHeader("render", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("resolution scale", &look.resScale, 0.2f, 1.0f);
        if (gpuTraceMs > 0.f) {
            ImGui::Text("trace %.2f ms   post %.2f ms", gpuTraceMs, gpuPostMs);
        }
    }
    if (ImGui::CollapsingHeader("film", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("exposure", &look.exposure, 0.1f, 4.f);
        ImGui::SliderFloat("grain", &look.grainAmount, 0.f, 0.5f);
        ImGui::SliderFloat("vignette inner", &look.vignetteInner, 0.f, 1.5f);
        ImGui::SliderFloat("vignette outer", &look.vignetteOuter, 0.5f, 2.5f);
        ImGui::SliderFloat("gate weave", &look.weaveAmount, 0.f, 4.f);
        ImGui::SliderFloat("bloom", &look.bloomAmount, 0.f, 1.f);
        ImGui::SliderFloat("bloom threshold", &look.bloomThreshold, 0.f, 2.f);
    }
    // The camera sliders (azimuth/elevation/distance/fov) are gone: drag-orbit
    // and the wheel do the first three better, and `--cam AZ,EL,DIST` plus ctl
    // `cam.*` cover the scripted case.
    ImGui::End();
}

void uiRender(wgpu::RenderPassEncoder& pass) {
    ImGui::Render();
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
}

void uiShutdown() {
    ImGui_ImplWGPU_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}
