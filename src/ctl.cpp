#include "ctl.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "renderer.h"

#if !CLAYFRAY_DEV_TOOLS
// The ctl port is the desktop agent loop: it scans a directory EVERY FRAME for
// command files and writes responses back. Under Emscripten that directory is
// MEMFS, so nothing can ever appear in it and nothing can read what we write —
// the scan would be a pure per-frame cost for a channel with no other end.
// Journal replay goes with it (same execute() path, and its input is a file).
//
// The class survives as no-ops so both run loops keep their unconditional
// ctl.poll()/finishFrame() calls; SimClock, which is header-only and is real
// gameplay state, is untouched.
bool loadJournal(const std::string&, std::vector<JournalEntry>&) { return false; }
std::string snapFilePath(const std::string& name) { return name; }
void CtlServer::init(const std::string&, const CtlRefs& refs) { refs_ = refs; }
void CtlServer::poll(long) { activity_ = false; }
void CtlServer::finishFrame() {}
bool CtlServer::execute(const std::string&, long, std::string& out) {
    out += "err ctl is not built into the web target\n";
    return false;
}
bool CtlServer::startRecord(const std::string&) { return false; }
void CtlServer::stopRecord() {}
void CtlServer::recordEdit(const BrickEdit&, long) {}

#else

namespace fs = std::filesystem;

namespace {

std::vector<std::string> tokenize(const std::string& line) {
    std::istringstream ss(line);
    std::vector<std::string> t;
    std::string w;
    while (ss >> w) t.push_back(w);
    return t;
}

bool parseFloats(const std::vector<std::string>& tok, size_t first, size_t count,
                 float* out) {
    if (tok.size() < first + count) return false;
    for (size_t i = 0; i < count; i++) {
        char* end = nullptr;
        out[i] = std::strtof(tok[first + i].c_str(), &end);
        if (end == tok[first + i].c_str()) return false;
    }
    return true;
}

std::string fmtFloats(const float* v, int n) {
    char buf[96];
    int len = 0;
    for (int i = 0; i < n; i++) {
        len += std::snprintf(buf + len, sizeof(buf) - len, i ? " %.6g" : "%.6g", v[i]);
    }
    return std::string(buf, len);
}

} // namespace

bool loadJournal(const std::string& path, std::vector<JournalEntry>& out) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[journal] cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        lineNo++;
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line[p] == '#') continue;
        char* end = nullptr;
        long tick = std::strtol(line.c_str() + p, &end, 10);
        if (end == line.c_str() + p) {
            std::fprintf(stderr, "[journal] %s:%d: expected '<tick> <cmd>'\n",
                         path.c_str(), lineNo);
            return false;
        }
        while (*end == ' ' || *end == '\t') end++;
        if (!*end) continue;
        out.push_back({tick, std::string(end)});
    }
    return true;
}

std::string snapFilePath(const std::string& name) {
    if (name.find('/') != std::string::npos) return name;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".snap") == 0) return name;
    return "snapshots/" + name + ".snap";
}

void CtlServer::init(const std::string& dir, const CtlRefs& refs) {
    refs_ = refs;
    buildRegistry();
    if (dir.empty()) return;
    inDir_ = dir + "/in";
    outDir_ = dir + "/out";
    std::error_code ec;
    fs::create_directories(inDir_, ec);
    fs::create_directories(outDir_, ec);
    enabled_ = !ec;
    if (!enabled_) {
        std::fprintf(stderr, "[ctl] cannot create %s: %s\n", dir.c_str(),
                     ec.message().c_str());
    }
}

void CtlServer::buildRegistry() {
    LookParams* l = refs_.look;
    addF("look.keyPos", l->keyPos, 3);
    addF("look.keyIntensity", &l->keyIntensity);
    addF("look.keyColor", l->keyColor, 3);
    addF("look.keyFalloff", &l->keyFalloff);
    addF("look.rimDir", l->rimDir, 3);
    addF("look.rimIntensity", &l->rimIntensity);
    addF("look.rimColor", l->rimColor, 3);
    addF("look.ambient", l->ambient, 3);
    addF("look.aoStrength", &l->aoStrength);
    addF("look.detailAmount", &l->detailAmount);
    addF("look.boilAmount", &l->boilAmount);
    addF("look.shadowSoft", &l->shadowSoft);
    addF("look.sheenAmount", &l->sheenAmount);
    addF("look.grainAmount", &l->grainAmount);
    addF("look.vignetteInner", &l->vignetteInner);
    addF("look.vignetteOuter", &l->vignetteOuter);
    addF("look.weaveAmount", &l->weaveAmount);
    addF("look.exposure", &l->exposure);
    addF("look.bloomAmount", &l->bloomAmount);
    addF("look.bloomThreshold", &l->bloomThreshold);
    addF("look.debugMode", &l->debugMode);
    addF("look.resScale", &l->resScale);
    addB("look.animPlay", &l->animPlay);
    addF("look.animSpeed", &l->animSpeed);
    addB("look.conserveClay", &l->conserveClay);
    addB("sword.enabled", &l->sword.enabled);
    // Missing until now, which self-diagnosed exactly as CLAUDE.md says an
    // unregistered tunable does: "err unknown param sword.carry". Without it a
    // scenario could not put the hilt in WORLD space, so every scripted sword
    // test was stuck with the carried offset riding the fighter's root.
    addB("sword.carry", &l->sword.carry);
    addF("sword.pos", l->sword.pos, 3);
    addF("sword.yaw", &l->sword.yaw);
    addF("sword.pitch", &l->sword.pitch);
    addF("sword.length", &l->sword.length);
    addF("sword.radius", &l->sword.radius);
    addF("sword.grip0", &l->sword.grip0);
    addF("sword.grip1", &l->sword.grip1);
    addF("sword.color", l->sword.color, 3);
    addB("sword.sliceGob", &l->sword.sliceGob);
    addF("sword.sliceSpeed", &l->sword.sliceSpeed);
    addF("sword.sliceLift", &l->sword.sliceLift);
    addF("sword.sliceOut", &l->sword.sliceOut);
    addB("motion.stepRoot", &l->motion.stepRoot);
    // The invisible wall. 0 = no boundary, walk off into the dark.
    addF("motion.arenaRadius", &l->motion.arenaRadius);
    // M-PERF: affine body + two rigid mitts vs the 13-piece inverse-LBS warp.
    // CLAYFRAY_NO_AFFINE=1 pins it off for a benchmark run.
    addB("look.affineRig", &l->affineRig);
    addF("rig.squishK", &l->rig.squishK);
    addF("rig.squishDamp", &l->rig.squishDamp);
    addF("rig.squishKick", &l->rig.squishKick);
    addF("rig.gaitHz", &l->rig.gaitHz);
    addF("rig.idleHz", &l->rig.idleHz);
    addF("rig.idleScale", &l->rig.idleScale);
    addF("rig.hop", &l->rig.hop);
    addF("rig.widen", &l->rig.widen);
    addF("rig.impactGain", &l->rig.impactGain);
    addF("rig.impactMax", &l->rig.impactMax);
    addF("rig.impactK", &l->rig.impactK);
    addF("rig.impactDamp", &l->rig.impactDamp);
    addF("rig.impactWiden", &l->rig.impactWiden);
    addF("rig.impactLift", &l->rig.impactLift);
    addB("hands.ik", &l->hands.ik);
    addF("hands.reach", &l->hands.reach);
    addF("hands.reachScale", &l->hands.reachScale);
    addF("hands.palmFrac", &l->hands.palmFrac);
    addB("hands.orient", &l->hands.orient);
    addI("hands.gripAxis", &l->hands.gripAxis);
        addF("hands.gripRoll", &l->hands.gripRoll);
    addF("hands.gripSpread", &l->hands.gripSpread);
    // M-FIST: where the mitts go when they are NOT on a hilt. Every one of
    // these is dialled by eye in the running app — that is what ctl is for.
    addF("handpose.idlePos", l->handPose.idlePos, 3);
    addF("handpose.idlePitch", &l->handPose.idlePitch);
    addF("handpose.idleYaw", &l->handPose.idleYaw);
    addF("handpose.fistPos", l->handPose.fistPos, 3);
    addF("handpose.fistPitch", &l->handPose.fistPitch);
    addF("handpose.fistYaw", &l->handPose.fistYaw);
    addF("handpose.bobAmp", &l->handPose.bobAmp);
    addF("handpose.bobTilt", &l->handPose.bobTilt);
    addF("handpose.bobRate", &l->handPose.bobRate);
    addF("handpose.bobRateMove", &l->handPose.bobRateMove);
    addF("handpose.bobAmpMove", &l->handPose.bobAmpMove);
    addF("handpose.punchReach", &l->handPose.punchReach);
    addF("handpose.punchDur", &l->handPose.punchDur);
    addF("handpose.fistRadius", &l->handPose.fistRadius);
    addF("handpose.cutSpeed", &l->handPose.cutSpeed);
    if (refs_.ai) {
        AiParams* a = refs_.ai;
        addB("ai.enabled", &a->enabled);
        addB("ai.retaliatory", &a->retaliatory);
        addF("ai.wanderSpeed", &a->wanderSpeed);
        addF("ai.chaseSpeed", &a->chaseSpeed);
        addF("ai.lockRange", &a->lockRange);
        addF("ai.breakRange", &a->breakRange);
        addF("ai.standoff", &a->standoff);
        addF("ai.strikeRange", &a->strikeRange);
        addF("ai.punchCooldown", &a->punchCooldown);
        addF("ai.wanderRadius", &a->wanderRadius);
        addF("ai.wanderHold", &a->wanderHold);
        addF("ai.turnRate", &a->turnRate);
        addF("ai.accel", &a->accel);
    }
    if (refs_.phys) {
        PhysicsParams* ph = refs_.phys;
        addB("phys.enabled", &ph->enabled);
        addF("phys.bodyRadius", &ph->bodyRadius);
        addF("phys.pushOut", &ph->pushOut);
        addF("phys.bladeDrag", &ph->bladeDrag);
        addF("phys.fistDrag", &ph->fistDrag);
        addF("phys.minRate", &ph->minRate);
        addF("phys.knockForce", &ph->knockForce);
        addF("phys.knockMax", &ph->knockMax);
        addF("phys.knockDamp", &ph->knockDamp);
        addF("phys.recoil", &ph->recoil);
        addF("phys.stagger", &ph->stagger);
        addF("phys.staggerBite", &ph->staggerBite);
        addF("phys.staggerControl", &ph->staggerControl);
    }
    // M-DEATH
    addB("death.enabled", &l->death.enabled);
    addF("death.threshold", &l->death.threshold);
    addF("death.respawn", &l->death.respawn);
    addF("death.eyeSpeed", &l->death.eyeSpeed);
    addF("death.eyeLift", &l->death.eyeLift);
    addF("death.eyeBounce", &l->death.eyeBounce);
    addF("death.eyeRoll", &l->death.eyeRoll);
    addF("death.spawnRadius", &l->death.spawnRadius);
    addI("death.burstGobs", &l->death.burstGobs);
    addF("death.burstFrac", &l->death.burstFrac);
    addF("death.burstSpeed", &l->death.burstSpeed);
    addF("death.burstLift", &l->death.burstLift);
    addF("death.splatMax", &l->death.splatMax);
    addF("death.splatSpread", &l->death.splatSpread);
    addB("gaze.track", &l->gaze.track);
    addF("gaze.maxAngle", &l->gaze.maxAngle);
    OrbitCamera* c = refs_.cam;
    addF("cam.azimuth", &c->azimuth);
    addF("cam.elevation", &c->elevation);
    addF("cam.distance", &c->distance);
    addF("cam.target", &c->target.x, 3);
    addF("cam.fovY", &c->fovY);
    if (refs_.renderer) {
        // One set per opponent: p1.*, p2.*, ... (player 0 is `fighter.*`
        // below, since it is the one the harness drives directly).
        for (int i = 1; i < Renderer::kMaxPlayers; i++) {
            char name[24];
            std::snprintf(name, sizeof(name), "p%d.enabled", i);
            addB(name, refs_.renderer->playerEnabledPtr(i));
            std::snprintf(name, sizeof(name), "p%d.pos", i);
            addF(name, refs_.renderer->playerPosePtr(i)->pos, 3);
            std::snprintf(name, sizeof(name), "p%d.yaw", i);
            addF(name, &refs_.renderer->playerPosePtr(i)->yaw);
            std::snprintf(name, sizeof(name), "p%d.lean", i);
            addF(name, &refs_.renderer->playerPosePtr(i)->lean);
            std::snprintf(name, sizeof(name), "p%d.moving", i);
            addB(name, &refs_.renderer->playerPosePtr(i)->moving);
            // M-FIST. Settable so a journal can pose a fist without the AI:
            // `set ai.enabled 0` then drive p1.guard / p1.punch by hand, which
            // is how a punch cut gets a repeatable regression scenario.
            std::snprintf(name, sizeof(name), "p%d.guard", i);
            addB(name, &refs_.renderer->playerPosePtr(i)->guard);
            std::snprintf(name, sizeof(name), "p%d.punch", i);
            addF(name, &refs_.renderer->playerPosePtr(i)->punch);
            std::snprintf(name, sizeof(name), "p%d.punchSide", i);
            addI(name, &refs_.renderer->playerPosePtr(i)->punchSide);
        }
        // `foe.*` is player 1 under its old name. Journals are the durable
        // form of a scenario (CLAUDE.md), and scenarios/ already contains
        // recorded `set foe.pos` lines — dropping the name would break replay
        // determinism checks for no gain.
        addB("foe.enabled", refs_.renderer->playerEnabledPtr(1));
        addF("foe.pos", refs_.renderer->playerPosePtr(1)->pos, 3);
        addF("foe.yaw", &refs_.renderer->playerPosePtr(1)->yaw);
    }
    if (refs_.fighter) {
        addF("fighter.pos", refs_.fighter->pos, 3);
        addF("fighter.yaw", &refs_.fighter->yaw);
        addF("fighter.lean", &refs_.fighter->lean);
        addB("fighter.moving", &refs_.fighter->moving);
        addB("fighter.guard", &refs_.fighter->guard);
        addF("fighter.punch", &refs_.fighter->punch);
        addI("fighter.punchSide", &refs_.fighter->punchSide);
    }
    if (refs_.brush) {
        addI("brush.mode", &refs_.brush->mode);
        addI("look.traceW", &l->traceW);
        addI("look.traceH", &l->traceH);
        addF("brush.radius", &refs_.brush->radius);
        addF("brush.color", refs_.brush->color, 3);
    }
}

const CtlServer::Param* CtlServer::find(const std::string& name) const {
    for (const Param& p : params_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

void CtlServer::record(long tick, const std::string& line) {
    if (!rec_) return;
    std::fprintf(rec_, "%ld %s\n", tick, line.c_str());
    std::fflush(rec_);
}

bool CtlServer::startRecord(const std::string& path) {
    stopRecord();
    rec_ = std::fopen(path.c_str(), "a");
    if (rec_) {
        recPath_ = path;
        std::fprintf(rec_, "# clayfray journal\n");
    }
    return rec_ != nullptr;
}

void CtlServer::stopRecord() {
    if (rec_) std::fclose(rec_);
    rec_ = nullptr;
    recPath_.clear();
}

void CtlServer::recordEdit(const BrickEdit& e, long poseTick) {
    if (!rec_) return;
    char buf[256];
    // The player index is the last field, so a diff against a pre-slice
    // journal is one appended column rather than a reshuffle.
    std::snprintf(buf, sizeof(buf),
                  "edit %s %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g "
                  "%.6g %.6g %.6g %d",
                  e.mode == 1 ? "carve" : "add", e.pos[0], e.pos[1], e.pos[2],
                  e.radius, e.color[0], e.color[1], e.color[2], e.outDir[0],
                  e.outDir[1], e.outDir[2], e.srcColor[0], e.srcColor[1],
                  e.srcColor[2], e.player);
    record(poseTick, buf);
}

std::string CtlServer::statsJson(long poseTick) const {
    const SplootStats& s = refs_.renderer->sploot();
    char buf[512];
    std::snprintf(
        buf, sizeof(buf),
        "{\"t\":%.4f,\"poseTick\":%ld,\"paused\":%d,\"timescale\":%.3g,"
        "\"carved_ml\":%.2f,\"deposited_ml\":%.2f,\"debt_ml\":%.2f,"
        "\"inflight_ml\":%.2f,\"gobs\":%d,\"traceMs\":%.2f,\"postMs\":%.2f,"
        "\"fps\":%.1f,\"recording\":%d}",
        refs_.simT ? *refs_.simT : 0.0, poseTick,
        refs_.clock->paused ? 1 : 0, refs_.clock->timescale, s.carved * 1e6f,
        s.deposited * 1e6f, s.debt * 1e6f, s.inFlight * 1e6f, s.gobs,
        refs_.renderer->traceMs(), refs_.renderer->postMs(),
        refs_.fps ? *refs_.fps : 0.f, rec_ ? 1 : 0);
    return buf;
}

bool CtlServer::execute(const std::string& line, long poseTick, std::string& out) {
    std::vector<std::string> tok = tokenize(line);
    if (tok.empty()) return true;
    const std::string& cmd = tok[0];
    auto ok = [&](const std::string& msg) {
        out += "ok " + msg + "\n";
        return true;
    };
    auto err = [&](const std::string& msg) {
        out += "err " + msg + "\n";
        return false;
    };

    if (cmd == "get") {
        bool all = tok.size() < 2 || tok[1] == "*";
        bool found = false;
        for (const Param& p : params_) {
            if (!all && p.name != tok[1]) continue;
            found = true;
            if (p.f) {
                out += p.name + " = " + fmtFloats(p.f, p.n) + "\n";
            } else if (p.b) {
                out += p.name + " = " + (*p.b ? "1" : "0") + "\n";
            } else if (p.i) {
                out += p.name + " = " + std::to_string(*p.i) + "\n";
            }
        }
        return found || err("unknown param " + (tok.size() > 1 ? tok[1] : ""));
    }
    if (cmd == "set") {
        if (tok.size() < 3) return err("usage: set <name> <values...>");
        const Param* p = find(tok[1]);
        if (!p) return err("unknown param " + tok[1]);
        float v[4];
        if (!parseFloats(tok, 2, (size_t)p->n, v)) {
            return err(tok[1] + " needs " + std::to_string(p->n) + " value(s)");
        }
        if (p->f) {
            std::memcpy(p->f, v, p->n * sizeof(float));
        } else if (p->b) {
            *p->b = v[0] != 0.f;
        } else if (p->i) {
            *p->i = (int)v[0];
        }
        record(poseTick, line);
        return ok(tok[1] + " = " + fmtFloats(v, p->n));
    }
    if (cmd == "edit") {
        // edit carve|add x y z r [cr cg cb [dx dy dz sr sg sb [player]]]
        // The trailing player index is OPTIONAL and defaults to 0, so every
        // journal recorded before fighters became slices replays unchanged
        // against the hero — which is the body they were all authored for.
        if (tok.size() != 6 && tok.size() != 9 && tok.size() != 15 &&
            tok.size() != 16) {
            return err("usage: edit carve|add x y z r [cr cg cb "
                       "[dx dy dz sr sg sb [player]]]");
        }
        BrickEdit e;
        if (tok[1] == "carve") e.mode = 1;
        else if (tok[1] == "add") e.mode = 2;
        else return err("edit mode must be carve or add");
        float v[14];
        size_t n = tok.size() - 2;
        if (!parseFloats(tok, 2, n, v)) return err("edit: bad number");
        std::memcpy(e.pos, v, 12);
        e.radius = v[3];
        if (n >= 7) std::memcpy(e.color, v + 4, 12);
        if (n == 14) e.player = (int)v[13];
        if (e.player < 0 || e.player >= Renderer::kMaxPlayers) {
            return err("edit: player out of range");
        }
        BrickSystem& vol = refs_.renderer->playerBrick(e.player);
        bool inBounds = vol.editInBounds(e);
        BrickEdit resolved = e;
        if (n >= 13) {
            // explicit wound direction/color: bypass the live pick (replay)
            std::memcpy(e.outDir, v + 7, 12);
            std::memcpy(e.srcColor, v + 10, 12);
            resolved = e;
            vol.queueEdit(e);
        } else {
            resolved = refs_.renderer->queueBrickEdit(e);
        }
        if (rec_) recordEdit(resolved, poseTick);
        return ok(inBounds ? "edit queued" : "edit dropped (out of bounds)");
    }
    if (cmd == "bake") {
        refs_.renderer->brick().requestBake();
        record(poseTick, line);
        return ok("rebake queued");
    }
    if (cmd == "shot") {
        if (tok.size() != 2) return err("usage: shot <path.png>");
        if (cur_) {
            cur_->shots.push_back(tok[1]);
        } else {
            PendingOut po;
            po.shots.push_back(tok[1]);
            pending_.push_back(std::move(po));
        }
        return true; // result line lands after the render
    }
    if (cmd == "stats") {
        out += statsJson(poseTick) + "\n";
        return true;
    }
    if (cmd == "pause") {
        refs_.clock->paused = true;
        return ok("paused");
    }
    if (cmd == "resume") {
        refs_.clock->paused = false;
        refs_.clock->pendingPoseSteps = 0;
        return ok("resumed");
    }
    if (cmd == "step") {
        int n = tok.size() > 1 ? std::atoi(tok[1].c_str()) : 1;
        if (n < 1) n = 1;
        refs_.clock->paused = true;
        refs_.clock->pendingPoseSteps += n;
        return ok("stepping " + std::to_string(n) + " pose tick(s)");
    }
    if (cmd == "timescale") {
        float v[1];
        if (!parseFloats(tok, 1, 1, v)) return err("usage: timescale <f>");
        refs_.clock->timescale = std::min(std::max(v[0], 0.f), 8.f);
        return ok("timescale " + std::to_string(refs_.clock->timescale));
    }
    if (cmd == "snap") {
        if (tok.size() != 3 || (tok[1] != "save" && tok[1] != "load")) {
            return err("usage: snap save|load <name>");
        }
        std::string path = snapFilePath(tok[2]);
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        if (tok[1] == "save") {
            if (!refs_.renderer->saveSnapshot(path, refs_.simT ? *refs_.simT : 0.0,
                                              refs_.charPath)) {
                return err("save failed: " + path);
            }
            return ok("saved " + path);
        }
        double t = 0.0;
        if (!refs_.renderer->loadSnapshot(path, &t, refs_.charPath)) {
            return err("load failed: " + path);
        }
        if (refs_.simT) *refs_.simT = t;
        return ok("loaded " + path);
    }
    if (cmd == "record") {
        if (tok.size() != 2) return err("usage: record <path>|stop");
        if (tok[1] == "stop") {
            stopRecord();
            return ok("recording stopped");
        }
        if (!startRecord(tok[1])) return err("cannot open " + tok[1]);
        return ok("recording to " + tok[1]);
    }
    if (cmd == "break") {
        if (tok.size() == 2 && tok[1] == "off") {
            breakLedgerTol = -1.f;
            return ok("break disarmed");
        }
        float v[1];
        if (tok.size() != 3 || tok[1] != "ledger" || !parseFloats(tok, 2, 1, v)) {
            return err("usage: break ledger <tol_ml> | break off");
        }
        breakLedgerTol = v[0];
        return ok("break armed: ledger residual > " + std::to_string(v[0]) + " ml");
    }
    if (cmd == "pickuv") {
        float v[2];
        if (!parseFloats(tok, 1, 2, v)) return err("usage: pickuv <u> <v>");
        refs_.renderer->setPickUV(v[0], v[1]);
        return ok("pick uv set (probe next frame)");
    }
    if (cmd == "probe") {
        const Renderer* r = refs_.renderer;
        char buf[256];
        // `rest` is the volume-space address of the hit — what an edit here
        // would carve. It equals pos only while the fighter is unposed at the
        // origin; the gap between them IS the articulation.
        std::snprintf(buf, sizeof(buf),
                      "{\"valid\":%d,\"pos\":[%.4f,%.4f,%.4f],"
                      "\"rest\":[%.4f,%.4f,%.4f],"
                      "\"normal\":[%.3f,%.3f,%.3f],\"mat\":%.1f}",
                      r->pickValid() ? 1 : 0, r->pickPos()[0], r->pickPos()[1],
                      r->pickPos()[2], r->pickRest()[0], r->pickRest()[1],
                      r->pickRest()[2], r->pickNormal()[0], r->pickNormal()[1],
                      r->pickNormal()[2], r->pickMat());
        out += std::string(buf) + "\n";
        return true;
    }
    if (cmd == "quit") {
        if (refs_.wantQuit) *refs_.wantQuit = true;
        return ok("quitting");
    }
    return err("unknown command " + cmd +
               " (get set edit bake shot stats pause resume step timescale "
               "snap record break pickuv probe quit)");
}

void CtlServer::poll(long poseTick) {
    activity_ = false;
    if (!enabled_) return;
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(inDir_, ec)) {
        std::string n = entry.path().filename().string();
        if (!n.empty() && n[0] != '.') names.push_back(n);
    }
    if (names.empty()) return;
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        std::string path = inDir_ + "/" + n;
        std::ifstream f(path);
        if (!f) continue;
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        f.close();
        fs::remove(path, ec);
        activity_ = true;

        PendingOut po;
        po.path = outDir_ + "/" + n;
        pending_.push_back(std::move(po));
        cur_ = &pending_.back();
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            execute(line, poseTick, pending_.back().text);
        }
        cur_ = nullptr;
    }
}

void CtlServer::finishFrame() {
    if (pending_.empty()) return;
    for (PendingOut& po : pending_) {
        for (const std::string& s : po.shots) {
            bool okShot = refs_.renderer->screenshot(s);
            po.text += (okShot ? "ok shot " : "err shot failed ") + s + "\n";
        }
        if (po.path.empty()) {
            if (!po.text.empty()) std::fputs(po.text.c_str(), stdout);
            continue;
        }
        std::string tmp = po.path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            f << po.text;
        }
        std::error_code ec;
        // Windows rename won't replace an existing file (POSIX does); a
        // stale response from a dead client would otherwise wedge this name
        fs::remove(po.path, ec);
        fs::rename(tmp, po.path, ec);
    }
    pending_.clear();
}

#endif // CLAYFRAY_DEV_TOOLS
