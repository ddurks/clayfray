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

void uiNewFrame(LookParams& look, OrbitCamera& cam, BrushState& brush, float fps,
                float gpuTraceMs, float gpuPostMs, const SplootStats& sploot,
                bool& wantScreenshot) {
    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("look-dev");
    ImGui::Text("%.1f fps", fps);
    if (ImGui::Button("capture -> lookdev/")) wantScreenshot = true;

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
        ImGui::Checkbox("play clip (off = rest pose)", &look.animPlay);
        ImGui::SliderFloat("speed", &look.animSpeed, 0.f, 2.f);
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
        ImGui::DragFloat3("rim dir", look.rimDir, 0.02f, -1.f, 1.f);
        ImGui::SliderFloat("rim intensity", &look.rimIntensity, 0.f, 2.f);
        ImGui::ColorEdit3("rim color", look.rimColor);
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
    if (ImGui::CollapsingHeader("camera")) {
        ImGui::SliderFloat("azimuth", &cam.azimuth, -3.14f, 3.14f);
        ImGui::SliderFloat("elevation", &cam.elevation, -0.4f, 1.2f);
        ImGui::SliderFloat("distance", &cam.distance, 0.8f, 8.f);
        ImGui::SliderFloat("fov", &cam.fovY, 0.3f, 1.4f);
    }
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
