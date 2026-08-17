#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "brick.h"
#include "camera.h"
#include "params.h"

class Renderer;

// Sim time control. The sim runs on 60 Hz ticks; 5 ticks = one 12 Hz pose
// step, so `step` advances exactly one stop-motion frame per rendered frame.
struct SimClock {
    bool paused = false;
    float timescale = 1.f;
    int pendingPoseSteps = 0; // queued while paused; consumed one per frame

    // Wall dt in, whole 60 Hz ticks out. Caller clamps runaway counts.
    int ticksToRun(double frameDt) {
        if (paused) {
            if (pendingPoseSteps <= 0) return 0;
            pendingPoseSteps--;
            return 5;
        }
        acc_ += frameDt * (double)timescale;
        int n = (int)(acc_ * 60.0);
        acc_ -= n / 60.0;
        return n;
    }

  private:
    double acc_ = 0.0;
};

// One journal line: a ctl command scheduled for a 12 Hz pose tick.
struct JournalEntry {
    long tick = 0;
    std::string cmd;
};
bool loadJournal(const std::string& path, std::vector<JournalEntry>& out);

// `snapshots/<name>.snap` unless the name already looks like a path.
std::string snapFilePath(const std::string& name);

// Everything a ctl command may touch. Pointers must outlive the server
// (they point into the run loop's locals).
struct CtlRefs {
    LookParams* look = nullptr;
    OrbitCamera* cam = nullptr;
    BrushState* brush = nullptr;    // null headless
    FighterPose* fighter = nullptr; // locomotion state, drivable from replay
    // Opponent behaviour. A journal that places opponents by hand disables it
    // first (`set ai.enabled 0`), or the AI walks them off the marks it set.
    AiParams* ai = nullptr;
    // M-PHYS: knockback, resistance and body collision. Gameplay, like
    // ai above — LookParams stays about how the scene LOOKS.
    PhysicsParams* phys = nullptr;
    Renderer* renderer = nullptr;
    SimClock* clock = nullptr;
    double* simT = nullptr;
    float* fps = nullptr; // null headless
    bool* wantQuit = nullptr;
    std::string charPath;
};

// File-based command port: clients drop a text file of commands (one per
// line) into <dir>/in/, the frame loop executes it and writes the response
// to <dir>/out/<same name>. tools/ctl.sh wraps the round trip. The same
// execute() path runs journal replay lines, so live driving and replay
// can't drift apart. Not a network service — local FS only, by design.
class CtlServer {
  public:
    // Empty dir disables the inbox; execute()/finishFrame() still work
    // (replay uses them directly).
    void init(const std::string& dir, const CtlRefs& refs);

    // Scan the inbox and execute. Call once per frame BEFORE render so
    // set/edit/snap effects land in the frame a following `shot` captures.
    void poll(long poseTick);
    // Deferred work that needs the rendered frame (shots), then write the
    // staged responses. Call AFTER render + processEvents.
    void finishFrame();
    bool activity() const { return activity_; } // commands seen this poll

    // Execute one command line; appends response line(s) to `out`.
    bool execute(const std::string& line, long poseTick, std::string& out);

    // Journal recording. Mutating commands (set/edit/bake) are recorded by
    // execute(); brush strokes are recorded by the windowed loop with the
    // pick-resolved edit so replay doesn't depend on a live pick.
    bool startRecord(const std::string& path);
    void stopRecord();
    void recordEdit(const BrickEdit& e, long poseTick);
    bool recording() const { return rec_ != nullptr; }

    // break-on-condition: pause when |ledger residual| exceeds tol (ml).
    // One-shot: disarms after tripping. <0 = off. Checked by the run loops.
    float breakLedgerTol = -1.f;

    ~CtlServer() { stopRecord(); }

    // Owns the journal FILE* that stopRecord() closes, and hands out raw
    // pointers into a registry built from another object's members. A copy
    // double-closes the first and outlives the second.
    CtlServer(const CtlServer&) = delete;
    CtlServer& operator=(const CtlServer&) = delete;
    CtlServer() = default;

  private:
    struct Param {
        std::string name;
        float* f = nullptr;
        bool* b = nullptr;
        int* i = nullptr;
        int n = 1;
    };
    void addF(const char* name, float* p, int n = 1) {
        params_.push_back({name, p, nullptr, nullptr, n});
    }
    void addB(const char* name, bool* p) {
        params_.push_back({name, nullptr, p, nullptr, 1});
    }
    void addI(const char* name, int* p) {
        params_.push_back({name, nullptr, nullptr, p, 1});
    }
    const Param* find(const std::string& name) const;
    void buildRegistry();
    std::string statsJson(long poseTick) const;
    void record(long tick, const std::string& line);

    struct PendingOut {
        std::string path;                // response file ("" = direct execute)
        std::string text;                // response lines so far
        std::vector<std::string> shots;  // screenshots owed after render
    };
    std::vector<PendingOut> pending_;
    PendingOut* cur_ = nullptr; // entry being built during poll()

    CtlRefs refs_;
    std::vector<Param> params_;
    std::string inDir_, outDir_;
    bool enabled_ = false;
    bool activity_ = false;
    FILE* rec_ = nullptr;
    std::string recPath_;
};
