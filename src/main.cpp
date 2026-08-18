#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <string>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "asset.h"
#include "camera.h"
#include "ctl.h"
#include "gpu.h"
#include "params.h"
#include "renderer.h"
#include "touch.h"
#include "ui.h"

namespace {

constexpr double kTickRate = 60.0;
constexpr double kTickDt = 1.0 / kTickRate;

// GPU backpressure for the headless loops (--serve and --replay/--screenshot).
// Both advance a FIXED 1/60 s per iteration on purpose — that determinism is
// what makes replays exact — so neither is paced by wall clock, and with no
// swapchain there is no Fifo Present to block on either. Unthrottled, the CPU
// encodes ~1400 frames/s while the GPU retires far fewer, Dawn's pending-
// submission memory grows without bound, and vkQueueSubmit dies with
// VK_ERROR_OUT_OF_DEVICE_MEMORY within seconds. Every MapAsync after that
// fails "[Device] is lost", which is what made serve-mode `shot` look like a
// readback bug. Capping the queue depth costs nothing and fixes it.
constexpr int kMaxFramesInFlight = 2;

// Frames per second over a wall-clock window.
//
// NOT an exponential average of 1/dt, which is what this used to be and which
// reads HIGH whenever frame times vary: averaging reciprocals weights cheap
// frames far more than expensive ones. Frame reuse makes dt strongly bimodal
// here — roughly two in three frames re-present the previous trace for
// almost nothing while the third pays for everything — so the cheap frames
// dominated and the HUD claimed 85 fps on a loop measured at 52. It looked
// right on Windows only because vsync pinned nearly every frame to 16.7 ms,
// which flattens the distribution and hides the bias.
//
// Counting frames over a fixed window is the statistic the benchmarking notes
// in CLAUDE.md ask for: wall clock per PRESENTED frame.
struct FpsMeter {
    double window = 0.0;
    int frames = 0;
    float value = 0.f;

    float tick(double dt) {
        window += dt;
        frames++;
        if (window >= 0.5) { // ~2 updates/s: steady enough to read
            value = (float)(frames / window);
            window = 0.0;
            frames = 0;
        }
        return value;
    }
};

// ---- M-PHYS: one fighter's physical state ----
//
// Locomotion velocity and knockback velocity are kept SEPARATE and summed only
// at integration, and that single decision is what makes the rest of this
// simple. Folding a shove into `vel` puts it directly in the path of the
// accel-toward-desired model that drives walking: at kAccel 7 m/s^2 a 2 m/s
// knock is cancelled by the victim's own steering in under a third of a second,
// so a solid hit reads as nothing happening. Kept apart, the shove decays on
// its own schedule and a fighter can walk while it is still sliding.
// M-SPRING puts a THIRD velocity here, and the split is the whole mechanic:
// `vel` is what the fighter WANTS (steering, updated every tick, planted or
// not), `air` is what its last push-off actually GAVE it, and only `air` and
// `knock` ever move it. A planted foot does not slide.
struct Body {
    float vel[3] = {0.f, 0.f, 0.f};   // locomotion DEMAND: input or AI
    float air[3] = {0.f, 0.f, 0.f};   // the launch we are riding; 0 on the floor
    float knock[3] = {0.f, 0.f, 0.f}; // knockback: contact only
    float force[3] = {0.f, 0.f, 0.f}; // THIS FRAME's contact force, m/s^2
    float stagger = 0.f;              // s of no steering left
    float rate = 1.f;                 // strike-clock multiplier, minRate..1
    // M-MASS: 1 / (mass fraction), refreshed once a frame from the renderer's
    // carved ledger (PhysicsParams::massKnock). It multiplies knockback and
    // nothing else a body does for itself — losing clay makes you easier to
    // throw, not quicker on your feet.
    float invMass = 1.f;
    // ---- the hopper (M-SPRING; the model is documented on RigParams) ----
    // u < 0 compressed against the floor, u >= 0 feet off it. It lives HERE
    // rather than in the renderer because travel depends on it: see the
    // "TRAVEL IS BALLISTIC" note in params.h for why 12 Hz was not good enough
    // once distance-per-hop became locomotion.
    float u = 0.f, vu = 0.f;
    float gait = 0.f;                 // idle breath phase
    // The TRAVEL lean, before the arc lean is added. It needs its own slot
    // because it is the state of a first-order filter: writing the sum back
    // into FighterPose::lean and filtering that would feed the arc into its own
    // input and smear it across the next several ticks.
    float leanBase = 0.f;
    bool airborne() const { return u > 0.f; }

    // Pitch forward on the way up, back on the way down (RigParams::hopLean*).
    // `speedFrac` is how fast the fighter is travelling against its own top
    // speed, which fades the whole effect out as it slows and keeps a standing
    // body from rocking on its breath.
    float hopLean(const RigParams& r, float speedFrac) const {
        const float g = vu >= 0.f ? r.hopLeanFwd : r.hopLeanBack;
        const float s = std::min(std::max(speedFrac, 0.f), 1.f);
        return std::min(std::max(vu * g, -0.6f), 0.6f) * s;
    }

    // One tick of the leg spring, and the two events that matter hang off it:
    // the push-off at maximum compression (which is where the forward launch
    // comes from) and the landing (which absorbs it again).
    //
    // Substepped FOUR ways against the 60 Hz tick, giving the same ~4.2 ms step
    // the defaults were measured at — a bare 16.7 ms step costs ~7% of the hop
    // and would make every number on RigParams a lie. Fixed count, fixed dt, no
    // RNG, no wall clock, so --replay reproduces a whole brawl exactly.
    void stepHop(const RigParams& r, bool moving, float dt) {
        if (!moving) {   // breathing is still a metronome; walking pumps instead
            const float next = gait + dt * r.idleHz;
            if (next >= 1.f) vu -= r.idleKick;
            gait = next - std::floor(next);
        }
        const int kSub = 4;
        const float h = dt / (float)kSub;
        for (int i = 0; i < kSub; i++) {
            const float was = vu;
            const bool wasAir = u > 0.f;
            vu -= r.gravity * h;
            if (u < 0.f) {
                vu += (-r.legK * u - r.legDamp * vu) * h;
                if (moving && was < 0.f && vu >= 0.f) {
                    // THE PUSH-OFF. Up, and forward along whatever we are
                    // steering right now — this is the only moment a heading is
                    // chosen, which is what makes a hop a commitment.
                    vu += r.hopThrust;
                    air[0] = vel[0] * r.hopLaunch;
                    air[2] = vel[2] * r.hopLaunch;
                }
            }
            u += vu * h;
            if (wasAir && u <= 0.f) {
                // Landing. The clay takes the horizontal with the vertical —
                // it is the same compression absorbing both — so the next hop
                // starts from nothing and has to be pushed for.
                air[0] = 0.f;
                air[2] = 0.f;
            }
            // Mid-flight steering, off by default: a body in the air is a
            // projectile. See RigParams::airControl.
            if (r.airControl > 0.f && u > 0.f) {
                const float k = std::min(1.f, r.airControl * h);
                air[0] += (vel[0] * r.hopLaunch - air[0]) * k;
                air[2] += (vel[2] * r.hopLaunch - air[2]) * k;
            }
        }
        // A floor on the COMPRESSION, not on the lift: clay squashed this far
        // has left the model behind (a linear spring stops being one), but a
        // body flying high is just a body flying high.
        if (u < -0.45f) {
            u = -0.45f;
            vu = std::max(vu, 0.f);
        }
    }

    // Every velocity, one tick. Still xz for travel — the hopper owns the one
    // vertical degree of freedom and writes it to the pose, and nothing else
    // here reads a height (knockback, the arena wall and body collision are all
    // ground-plane relations).
    //
    // KNOCKBACK IS NOT GATED ON CONTACT, and that is deliberate: it is someone
    // else's force rather than your legs, so it moves a planted body. A punch
    // that could not shove a standing fighter would be a worse bug than a
    // slide. Only `vel` — your own locomotion — waits for the air.
    void integrate(FighterPose& f, const PhysicsParams& p, const RigParams& r,
                   float dt) {
        stepHop(r, f.moving, dt);
        f.hopU = u;
        // M-MASS: the SAME force throws a lighter body further, and the
        // ceiling rises with it — capping every fighter at one knockMax would
        // put the whole mechanic back behind a constant the moment a hit was
        // hard enough to saturate, which at 0.8 m/s is most of them.
        for (int a = 0; a < 3; a += 2) knock[a] += force[a] * invMass * dt;
        const float kmax = p.knockMax * invMass;
        float ks = std::sqrt(knock[0] * knock[0] + knock[2] * knock[2]);
        if (ks > kmax && ks > 1e-6f) {
            const float s = kmax / ks;
            knock[0] *= s;
            knock[2] *= s;
            ks = kmax;
        }
        // ONLY WHILE AIRBORNE. `air` is set at the push-off, which happens at
        // maximum compression — mid-stance, with the foot still planted — so
        // translating on it the moment it exists would have the body slide
        // forward through the back half of every stance. Measured: that is 26%
        // of the cycle and it put travel at 1.59 m/s against the 1.10 the
        // launch was calibrated for. The launch is EARNED at the push-off and
        // SPENT in the air.
        const float carry = airborne() ? 1.f : 0.f;
        f.pos[0] += (air[0] * carry + knock[0]) * dt;
        f.pos[2] += (air[2] * carry + knock[2]) * dt;
        // LINEAR bleed-off, not exponential. An exponential shove asymptotes
        // and never quite stops, and "still drifting a minute later" is how a
        // fighter ends up slowly leaving the arena with nothing touching it.
        const float drop = p.knockDamp * dt;
        if (ks <= drop) {
            knock[0] = 0.f;
            knock[2] = 0.f;
        } else if (ks > 1e-6f) {
            const float s = (ks - drop) / ks;
            knock[0] *= s;
            knock[2] *= s;
        }
        if (stagger > 0.f) stagger = std::max(0.f, stagger - dt);
    }

    // Remove the component of every velocity heading along -`out`, i.e. INTO
    // whatever just stopped us. Only the approaching part: zeroing the whole
    // vector freezes a fighter sliding ALONG a surface, which is how bodies get
    // stuck on each other and on the arena edge.
    //
    // `air` is in here too, and has to be: it is the one that actually carries
    // the body now, so leaving it out would let a fighter hop straight into the
    // arena wall and keep its launch, grinding against it for the rest of the
    // flight. Killing it mid-air is also the right read — that is what hitting
    // something while airborne does.
    void killApproach(float outX, float outZ) {
        float* vs[3] = {vel, air, knock};
        for (float* v : vs) {
            const float into = -(v[0] * outX + v[2] * outZ);
            if (into > 0.f) {
                v[0] += outX * into;
                v[2] += outZ * into;
            }
        }
    }
};

// The invisible wall (MotionParams::arenaRadius). Clamps a fighter to the lit
// disc and kills the outward part of its velocity, so it SLIDES along the
// boundary instead of grinding into it — walking straight at the edge and
// stopping dead reads as a bug, sliding reads as a wall you cannot see.
//
// Applied to the hero and to every opponent through the same function, because
// a boundary that only some bodies obey is worse than none.
void confineToArena(FighterPose& f, Body& b, float radius) {
    if (radius <= 0.f) return;
    const float r = std::sqrt(f.pos[0] * f.pos[0] + f.pos[2] * f.pos[2]);
    if (r <= radius || r < 1e-5f) return;
    const float nx = f.pos[0] / r, nz = f.pos[2] / r;
    f.pos[0] = nx * radius;
    f.pos[2] = nz * radius;
    b.killApproach(-nx, -nz); // inward is the free direction
}

// The punch's extension curve: 0 at the guard, 1 fully extended. Out fast,
// back slower — the speed is in the strike, and the slower recover is what lets
// the fist leave the wound behind it instead of dragging it back.
//
// Advanced at the 60 Hz SIM rate, deliberately NOT quantised to the pose grid.
// The sword's swing arc is the same (only its idle bob steps at 12 Hz) and for
// the same reason: a strike is carved as a swept capsule between consecutive
// frames, so a fist that jumped 12 times a second would cut in stripes.
float punchExtension(float u) {
    u = std::min(std::max(u, 0.f), 1.f);
    const float kOut = 0.40f;
    if (u < kOut) {
        const float a = u / kOut;
        return a * a * (3.f - 2.f * a);
    }
    const float a = (u - kOut) / (1.f - kOut);
    return 1.f - a * a * (3.f - 2.f * a);
}

// ---- M-FIST: one opponent's behaviour ----
//
// Wander -> lock on -> close -> jab, and back out again when the hero leaves.
// Deterministic by the house rules (fixed dt, seeded RNG, no wall clock), which
// is why it can run in the headless paths too: a `--replay` of a brawl
// reproduces it exactly. Journals that place opponents by hand turn it off with
// `set ai.enabled 0` first.
//
// It writes a FighterPose and nothing else — no renderer state, no edits. The
// punch's damage is a consequence of where the fist ends up, resolved by
// Renderer::updatePunchCut against whatever it passes through, so this has no
// idea whether a swing connected and does not need one.
struct OpponentAi {
    uint32_t rng = 0;
    bool locked = false;
    // Has anything hit us yet? Set from the strike-contact report in
    // GameState::applyContacts and never cleared — a respawn throws the whole
    // OpponentAi away, so forgiveness is something you have to kill it for.
    bool provoked = false;
    float wanderT = 0.f;      // s until a new heading is chosen
    float heading = 0.f;      // where we are wandering, radians
    float cooldown = 0.f;     // s until the next punch is allowed
    float punchT = -1.f;      // s into the current punch, <0 = not punching
    int side = 0;             // which mitt is throwing it

    void seed(int index) {
        // distinct per opponent, and distinct from the gob stream, so two
        // bodies do not wander in lockstep
        rng = 0x9E3779B9u ^ (0x85EBCA6Bu * (uint32_t)(index + 1));
        heading = 0.7f * (float)index;
    }
    float rand01() {
        rng = rng * 1664525u + 1013904223u;
        return (float)((rng >> 8) & 0xFFFFFFu) / 16777216.f;
    }

    void tick(FighterPose& me, const FighterPose& hero, const AiParams& ai,
              Body& body, const PhysicsParams& phys, const RigParams& rig,
              float punchDur, float dt) {
        // ---- lock on, with hysteresis, once we have a reason to ----
        //
        // The reason is `provoked` (AiParams::retaliatory): being near the hero
        // is not a reason to fight, having been hit by one is. Gating the LOCK
        // rather than the chase speed is deliberate — an unprovoked opponent is
        // not "chasing at zero", it is genuinely elsewhere: no guard up, no
        // standoff, no jab, just wandering.
        const float dx = hero.pos[0] - me.pos[0], dz = hero.pos[2] - me.pos[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        const bool willFight = provoked || !ai.retaliatory;
        if (!locked && willFight && dist < ai.lockRange) locked = true;
        if (locked && (!willFight || dist > ai.breakRange)) locked = false;

        float want[3] = {0.f, 0.f, 0.f};
        float face = me.yaw;
        if (locked) {
            // close to punching distance and hold there. Walking THROUGH the
            // hero would look like a bug even though the bodies do not collide,
            // and standing off is what gives the jab something to reach for.
            face = std::atan2(dx, dz);
            if (dist > ai.standoff) {
                const float inv = dist > 1e-4f ? 1.f / dist : 0.f;
                want[0] = dx * inv * ai.chaseSpeed;
                want[2] = dz * inv * ai.chaseSpeed;
            }
        } else {
            // wander: hold a heading for a while, then pick another. Steered
            // back toward the middle when it strays, so an opponent left alone
            // does not walk off into the dark and never come back.
            wanderT -= dt;
            const float r = std::sqrt(me.pos[0] * me.pos[0] + me.pos[2] * me.pos[2]);
            if (wanderT <= 0.f) {
                wanderT = ai.wanderHold * (0.6f + rand01() * 0.8f);
                heading = rand01() * 6.28318531f;
            }
            if (r > ai.wanderRadius) {
                // turn back toward the centre rather than snapping to it
                heading = std::atan2(-me.pos[0], -me.pos[2]);
                wanderT = ai.wanderHold;
            }
            want[0] = std::sin(heading) * ai.wanderSpeed;
            want[2] = std::cos(heading) * ai.wanderSpeed;
            face = std::atan2(want[0], want[2]);
        }

        // Hitstun: a staggered fighter keeps steering, at reduced AUTHORITY —
        // it stumbles, it does not switch off. It still faces `face` too, at
        // full turn rate: a body that cannot even look at you while being
        // knocked about reads as broken rather than as staggered.
        const bool stunned = body.stagger > 0.f;
        if (stunned) {
            want[0] *= phys.staggerControl;
            want[2] *= phys.staggerControl;
        }

        for (int a = 0; a < 3; a += 2) {
            const float d = want[a] - body.vel[a];
            const float m = ai.accel * dt;
            body.vel[a] += std::max(-m, std::min(d, m));
        }
        body.integrate(me, phys, rig, dt);
        // LOCOMOTION speed only, deliberately: a fighter sliding backwards off
        // a punch is not walking, and driving the hopper off the shove would
        // have it break into a bound while being knocked over. `moving` is the
        // only thing gating the push-off, so this flag decides whether a body
        // bounds or slides.
        const float sp = std::sqrt(body.vel[0] * body.vel[0] +
                                   body.vel[2] * body.vel[2]);
        me.moving = sp > 0.10f;

        // shortest-arc turn toward `face`
        {
            float d = face - me.yaw;
            while (d > 3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            const float m = ai.turnRate * dt;
            me.yaw += std::max(-m, std::min(d, m));
        }
        // lean into the run, eased off while turning hard (the hero's rule)
        {
            float d = face - me.yaw;
            while (d > 3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            const float top = std::max(ai.chaseSpeed, 1e-3f);
            const float aim = 0.30f * (sp / top) *
                              (1.f - std::min(std::fabs(d), 1.f));
            body.leanBase += (aim - body.leanBase) * std::min(1.f, 6.f * dt);
            // The arc rides ON the travel lean, unfiltered — see Body::hopLean.
            me.lean = body.leanBase + body.hopLean(rig, sp / top);
        }

        // ---- fists ----
        me.guard = locked;
        if (cooldown > 0.f) cooldown -= dt;
        const float dur = std::max(punchDur, 1e-3f);
        if (punchT >= 0.f) {
            // M-PHYS: the punch's own clock, slowed by whatever it is buried
            // in. The whole extension curve reads off punchT, so resistance is
            // visible as the fist ploughing rather than as a number somewhere.
            punchT += dt * body.rate;
            if (punchT >= dur) {
                punchT = -1.f;
                cooldown = ai.punchCooldown;
            }
        } else if (!stunned && locked && dist < ai.strikeRange && cooldown <= 0.f) {
            punchT = 0.f;
            side = rand01() < 0.5f ? 0 : 1; // not predictably alternating
        }
        me.punchSide = side;
        me.punch = punchT < 0.f ? 0.f : punchExtension(punchT / dur);
    }
};

// Deterministic gameplay core. M5 test harness: one fighter walks the arena
// under WASD and swings its sword on a keypress. Deterministic by the house
// rules — fixed 60 Hz tick, seeded RNG, no wall clock in here — so a journal
// replay reproduces a run exactly. (After a snapshot load the tick count is
// resynced to the restored sim time.)
struct GameState {
    GameState() {
        for (int i = 0; i < Renderer::kMaxPlayers; i++) {
            foes[i].seed(i);
            // Stagger the hopper so identical bodies don't breathe — or bound —
            // in lockstep, which reads as one puppet duplicated rather than as
            // N actors. Both halves matter: `gait` desyncs the idle breath, and
            // the nudge to `vu` desyncs the WALK, because a limit cycle keeps
            // the phase it starts with and two bodies that begin walking from
            // identical states hop in step forever.
            bodies[i].gait = 0.37f * (float)i;
            bodies[i].vu = -0.21f * (float)i;
        }
    }

    uint64_t tickCount = 0;

    // ---- hitstop (PhysicsParams::hitstop) ----
    // Seconds the whole world is held. stepWorld eats ticks against it without
    // stepping anything and reports back how many it really ran, so the render
    // clock holds with the sim rather than sliding underneath a frozen picture.
    float hold = 0.f;
    // Seconds until ANY weapon may freeze the world again, and it is global on
    // purpose. Two reasons, both measured:
    //   - a plain "was it live last frame" edge does not work at all. Contact
    //     is reported every frame a weapon ploughs through AND it flickers off
    //     whenever a fist drops below `cutSpeed` at the apex of its arc, so an
    //     edge test re-arms several times per punch.
    //   - keyed per (attacker, target, weapon) it still fired twice per punch,
    //     because the IDLE mitt is also inside the target at this range and
    //     moves fast enough to report its own contact. That put 20 freezes in
    //     5 seconds, which reads as the game chugging rather than as impact.
    // Hitstop is a property of the MOMENT, not of the weapon, so one timer is
    // also the more honest model. Drained on the sim tick, hold or no hold.
    float hitCool = 0.f;

    FighterPose fighter;

    // M-FIST: the opponents, and the knobs they share. One AI per player slot;
    // slot 0 is the hero's and never ticks.
    AiParams ai;
    OpponentAi foes[Renderer::kMaxPlayers];
    // M-PHYS: EVERY fighter's physical state in one array, hero at 0.
    //
    // It used to live in two places — a `vel` on GameState for the hero and one
    // inside each OpponentAi — which was fine while nothing needed to look at
    // two bodies at once. Body-body collision does: it is a constraint over
    // PAIRS, and it cannot be written at all while each fighter's velocity is
    // private to whatever happens to drive it.
    PhysicsParams phys;
    Body bodies[Renderer::kMaxPlayers];
    // Set from look.sword.enabled before each tick. With the sword away the
    // hero keeps its fists up and SPACE throws a jab instead of a swing —
    // the punch is a generic mechanic, the opponent is just its first user.
    bool unarmed = false;
    float punchT = -1.f;
    int punchSide = 0;

    // desired travel direction in WORLD space, latched from the keyboard each
    // frame (camera-relative, resolved by the caller). Zero = stand still.
    float moveX = 0.f, moveZ = 0.f;
    bool swingRequested = false;

    // swing: a random arc swept across the front over kSwingDur seconds
    bool swinging = false;
    float swingT = 0.f, swingFrom = 0.f, swingTo = 0.f, swingArc = 0.f;
    uint32_t rng = 0x9E3779B9u;

    static constexpr float kMaxSpeed = 1.1f;   // m/s
    static constexpr float kAccel = 7.0f;      // m/s^2
    static constexpr float kTurnRate = 7.0f;   // rad/s
    static constexpr float kMaxLean = 0.30f;   // rad at full speed
    // A flourished swing is SLOW enough to read as claymation and slow enough
    // that the cut substeps cover its sweep without a bigger op budget.
    static constexpr float kSwingDur = 0.80f;  // s
    static constexpr float kBobAmp = 0.012f;   // m, held-sword breathing
    static constexpr float kBobTilt = 0.05f;   // rad

    float rand01() {
        rng = rng * 1664525u + 1013904223u;
        return (float)((rng >> 8) & 0xFFFFFFu) / 16777216.f;
    }

    void tick(const RigParams& rig, float punchDur = 0.30f) {
        tickCount++;
        const float dt = (float)kTickDt;

        float want[3] = {0.f, 0.f, 0.f};
        float dl = std::sqrt(moveX * moveX + moveZ * moveZ);
        if (dl > 1e-3f) {
            want[0] = moveX / dl * kMaxSpeed;
            want[2] = moveZ / dl * kMaxSpeed;
        }
        // Hitstun costs the player steering AUTHORITY, never the controls. It
        // used to zero the input outright and that read as the game freezing:
        // contact is reported for every frame a weapon ploughs through, so the
        // timer kept re-arming, and a jab every punchCooldown could pin the hero
        // in place with nothing to do about it. Scaled instead, WASD still
        // answers on the frame it is pressed — you simply cannot walk a 2.2 m/s
        // shove off at 0.45 authority, which is the part that should be true.
        Body& body = bodies[0];
        if (body.stagger > 0.f) {
            want[0] *= phys.staggerControl;
            want[2] *= phys.staggerControl;
        }
        for (int a = 0; a < 3; a += 2) {
            float d = want[a] - body.vel[a];
            float m = kAccel * dt;
            body.vel[a] += std::max(-m, std::min(d, m));
        }
        body.integrate(fighter, phys, rig, dt);

        // LOCOMOTION speed only — see the same note in OpponentAi::tick.
        float sp = std::sqrt(body.vel[0] * body.vel[0] + body.vel[2] * body.vel[2]);
        fighter.moving = sp > 0.10f;
        if (fighter.moving) {
            // face the way we are going, then tip into it: shortest-arc turn
            float target = std::atan2(body.vel[0], body.vel[2]);
            float d = target - fighter.yaw;
            while (d > 3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            float m = kTurnRate * dt;
            fighter.yaw += std::max(-m, std::min(d, m));
            // lean scales with speed, and eases off while still turning hard
            // so a pivot doesn't throw the body sideways
            float aim = kMaxLean * (sp / kMaxSpeed) * (1.f - std::min(std::fabs(d), 1.f));
            body.leanBase += (aim - body.leanBase) * std::min(1.f, 6.f * dt);
        } else {
            body.leanBase += (0.f - body.leanBase) * std::min(1.f, 6.f * dt);
        }
        // The arc lean rides ON the travel lean and is NOT filtered: `vu` is
        // already smooth, and a 6/s filter has a 0.17 s time constant against a
        // 0.22 s flight, which would damp the pose to nothing. Applied outside
        // the moving/standing branch so that a fighter that stops mid-hop
        // finishes its arc instead of snapping upright in the air.
        fighter.lean = body.leanBase + body.hopLean(rig, sp / kMaxSpeed);

        // ---- unarmed: SPACE is a jab, not a swing ----
        // Same request flag, so the key binding does not have to know which it
        // is; the sword's own state decides. Guard stays up whenever the sword
        // is away, which is what puts the fist brush on the hero's mitts.
        fighter.guard = unarmed;
        const float pdur = std::max(punchDur, 1e-3f);
        if (unarmed) {
            if (punchT >= 0.f) {
                punchT += dt * body.rate; // M-PHYS: clay slows the jab
                if (punchT >= pdur) punchT = -1.f;
            } else if (swingRequested) {
                punchT = 0.f;
                punchSide = (punchSide + 1) & 1; // alternate hands
            }
            fighter.punchSide = punchSide;
            fighter.punch = punchT < 0.f ? 0.f : punchExtension(punchT / pdur);
        } else {
            punchT = -1.f;
            fighter.punch = 0.f;
        }

        if (!unarmed && swingRequested && !swinging) {
            swinging = true;
            swingT = 0.f;
            // a random arc ACROSS the front: start on one side, finish on the
            // other, with a randomized rise so repeats don't read identically
            float side = rand01() < 0.5f ? 1.f : -1.f;
            swingFrom = side * (1.05f + rand01() * 0.25f);
            swingTo = -side * (1.15f + rand01() * 0.30f);
            swingArc = 0.25f + rand01() * 0.35f;
        }
        swingRequested = false;
        if (swinging) {
            // M-PHYS: the swing's own clock. Every beat of the flourish reads
            // off swingT, so a blade buried in a body visibly labours through
            // the cut and then snaps back up to speed as it exits — no separate
            // animation, no state, just a slower clock.
            swingT += dt * body.rate;
            if (swingT >= kSwingDur) swinging = false;
        }
    }

    // Sword offsets (character space) on top of the vertical guard pose.
    // Idle/moving: a slight bob so the held blade breathes. Swinging: a
    // three-beat flourish — wind up past the start, sweep flat and fast
    // across the front, then follow through and recover to vertical.
    // `poseT` is the 12 Hz quantized clock, so the bob steps with the
    // stop-motion instead of sliding at frame rate (CLAUDE.md trap 4).
    void swordOffset(float poseT, float& yaw, float& pitch, float& lift) const {
        // bob rides whether we are moving or not, a touch stronger on the move
        float amp = kBobAmp * (fighter.moving ? 1.6f : 1.0f);
        float rate = fighter.moving ? 7.5f : 3.0f;
        lift = amp * std::sin(poseT * rate);
        yaw = 0.f;
        pitch = kBobTilt * std::sin(poseT * rate * 0.5f);
        if (!swinging) return;

        float u = std::min(1.f, swingT / kSwingDur);
        const float kWind = 0.28f, kCut = 0.62f; // beat boundaries
        if (u < kWind) {
            // 1. wind up: rotate BACK past the start, blade still high
            float a = u / kWind;
            float e = a * a * (3.f - 2.f * a);
            yaw = swingFrom * e;
            pitch = -0.35f * e; // tip back over the shoulder
        } else if (u < kCut) {
            // 2. the cut: flat and fast across the front
            float a = (u - kWind) / (kCut - kWind);
            float e = a * a * (3.f - 2.f * a);
            yaw = swingFrom + (swingTo - swingFrom) * e;
            // Flatten to HORIZONTAL early in the beat and stay there, so the
            // blade sweeps flat across the whole strike instead of only
            // arriving horizontal at the end of it.
            float pe = std::min(1.f, a / 0.30f);
            pe = pe * pe * (3.f - 2.f * pe);
            pitch = -0.35f + (-1.5708f + 0.35f) * pe;
            // ride the flat sweep up to chest height, where an opponent's
            // torso actually is — at the hilt's resting height it scythes
            // past their shins
            lift += 0.16f * pe;
        } else {
            // 3. follow through and recover to the vertical guard
            float a = (u - kCut) / (1.f - kCut);
            float e = a * a * (3.f - 2.f * a);
            yaw = swingTo * (1.f - e) + swingTo * 0.35f * (1.f - e);
            pitch = -1.5708f * (1.f - e) + swingArc * 0.25f * std::sin(3.14159f * a);
            lift += 0.16f * (1.f - e);
        }
    }

    // Where fighter `i` actually lives. The hero's pose is GameState's; every
    // other one belongs to the renderer, because opponents are placed through
    // it (ctl `p1.pos`, addPlayer). The constraint solver needs them addressed
    // uniformly and does not care which is which.
    FighterPose* posePtr(Renderer& r, int i) { return i == 0 ? &fighter : &r.player(i); }
    // ALIVE, not merely enabled: a collapsed fighter has no body, so it
    // neither collides nor gets confined — it is not there to be pushed.
    bool liveBody(Renderer& r, int i) const { return r.playerAlive(i); }

    // Fold the renderer's per-frame contact report into per-fighter physics.
    //
    // Runs ONCE per frame, before the tick loop, and the split is intentional:
    // `force` and `rate` are held constant across that frame's ticks so
    // knockback integrates over real elapsed time rather than being re-applied
    // per tick (which would scale the shove with the tick count), while
    // `stagger` is a timer those ticks drain.
    void applyContacts(const std::vector<Renderer::StrikeContact>& cs) {
        for (Body& b : bodies) {
            b.force[0] = b.force[1] = b.force[2] = 0.f;
            b.rate = 1.f;
        }
        if (!phys.enabled) return;
        for (const Renderer::StrikeContact& c : cs) {
            const int t = c.target, a = c.attacker;
            if (t < 0 || t >= Renderer::kMaxPlayers) continue;
            if (a < 0 || a >= Renderer::kMaxPlayers) continue;
            // Shove along the weapon's TRAVEL, flattened to the floor. The
            // wound normal is the other candidate and it is worse: it points
            // out of the surface, so a blade entering at a glancing angle would
            // shunt the target sideways out of a cut it is still making.
            float d[3] = {c.dir[0], 0.f, c.dir[2]};
            const float dl = std::sqrt(d[0] * d[0] + d[2] * d[2]);
            if (dl < 1e-4f) continue; // a purely vertical strike shoves nobody
            d[0] /= dl;
            d[2] /= dl;

            // NO SQUARENESS FACTOR HERE, and that was a mistake worth leaving
            // a note about. Scaling the shove by -dot(travel, normal) is right
            // for a rigid impulse and WRONG for anything measured a frame at a
            // time: a punch reciprocates, so the instant it is deepest its
            // radial velocity is ~0 and the frame delta is whatever the bodies
            // happened to be doing sideways. Measured over a real AI brawl it
            // printed 0.88, 0.00, 0.69, 0.00, 0.29 on consecutive contacts —
            // half of every punch graded as a graze, which cut the shove, the
            // stagger AND the hitstop to a quarter on alternating frames.
            //
            // `bite` already carries what squareness was meant to add. It is
            // penetration past the surface, and a weapon skidding along the
            // skin does not penetrate — so the two multiplied together were
            // double-counting a quantity that was only reliable once. The
            // normal is still carried on the contact and still used, but only
            // where it is read at the DEEPEST sample and is therefore stable:
            // the glance push-out and the dent axis, both renderer-side.
            const float f = phys.knockForce * c.bite;
            bodies[t].force[0] += d[0] * f;
            bodies[t].force[2] += d[2] * f;
            // Newton's third, scaled down. An even exchange would shove the
            // attacker as far as the victim, which reads as both of them having
            // been hit; the wielder is braced and the clay is not.
            bodies[a].force[0] -= d[0] * f * phys.recoil;
            bodies[a].force[2] -= d[2] * f * phys.recoil;

            const float hit =
                std::min(1.f, c.bite / std::max(phys.staggerBite, 1e-4f));
            // M-MASS: a lighter body is rocked for longer by the same hit. This
            // is the mass term you can actually SEE — the shove it also scales
            // was cut to a nudge on purpose, so multiplying it stays subtle.
            bodies[t].stagger =
                std::max(bodies[t].stagger, phys.stagger * hit * bodies[t].invMass);

            // ---- hitstop, on the RISING EDGE of this contact ----
            // The animator's held pose: the world stops dead for one 12 Hz step
            // at the moment of impact. Keyed on (attacker, target, weapon)
            // rather than on "any contact", so a second fist landing while the
            // first is still buried is its own hit and holds again.
            {
                {
                    // A HOLD IS BINARY. It was `hitstop * hit`, which sounds
                    // reasonable and is not: a stop-motion animator holds a
                    // frame or does not, and a proportional hold turns every
                    // real hit into 1-3 ticks of freeze that nobody can see.
                    // Measured on an AI brawl it never once reached half its
                    // nominal length. So any hit past `hitstopBite` holds for
                    // the FULL step and anything under it holds not at all.
                    //
                    // The refractory is what makes that safe. Contact flickers
                    // frame to frame — updatePunchCut drops a fist below
                    // `cutSpeed`, so a punch reports EDGE on alternating frames
                    // as it slows at the apex — and a full-length hold re-armed
                    // on every one of those would lock the game solid. The
                    // rising edge alone is not enough; it needs a floor on how
                    // often one weapon may freeze the world.
                    const bool edge = hitCool <= 0.f;
                    const bool solid = c.bite >= std::max(phys.hitstopBite, 1e-4f);
                    if (edge && solid) {
                        hold = std::max(hold, phys.hitstop);
                        hitCool = phys.hitstopCool;
                    }
                    if (std::getenv("CLAYFRAY_DEBUG_IMPACT")) {
                        std::printf("[impact] p%d->p%d side %d bite=%.4f hit=%.2f "
                                    "%s%s hold=%.3f invM=%.2f\n",
                                    a, t, c.side, c.bite, hit,
                                    solid ? "SOLID" : "light",
                                    (edge && solid) ? " FREEZE" : "",
                                    hold, bodies[t].invMass);
                        std::fflush(stdout);
                    }
                }
            }

            // M-FIST: being hit is what starts a fight (AiParams::retaliatory).
            // Any strike provokes, whoever threw it — a body does not check who
            // hurt it before deciding to care. This is the only writer, so an
            // opponent nobody touches never locks on and the player gets to
            // choose when the brawl begins.
            if (t >= 1) foes[t].provoked = true;

            // Resistance belongs to the ATTACKER — it is their strike that has
            // to fight through. Minimum across contacts, so cutting two bodies
            // at once is as slow as the worse of them rather than the average.
            const float drag = (c.side < 0) ? phys.bladeDrag : phys.fistDrag;
            const float rate = 1.f / (1.f + drag * c.bite);
            bodies[a].rate = std::min(bodies[a].rate, std::max(phys.minRate, rate));
        }
    }

    // Bodies do not interpenetrate: a pairwise position constraint in xz, run
    // after every fighter has integrated. Integrate, then solve — nothing ends
    // a tick overlapping.
    //
    // Symmetric by construction (each body takes half the correction), which
    // matters more than it looks. Pushing only the "second" body would make
    // collision depend on player index: the hero would bulldoze opponents and
    // they could not return the favour.
    void resolveBodies(Renderer& r) {
        if (!phys.enabled || phys.bodyRadius <= 0.f) return;
        const float minD = phys.bodyRadius * 2.f;
        for (int i = 0; i < Renderer::kMaxPlayers; i++) {
            if (!liveBody(r, i)) continue;
            for (int j = i + 1; j < Renderer::kMaxPlayers; j++) {
                if (!liveBody(r, j)) continue;
                FighterPose& a = *posePtr(r, i);
                FighterPose& b = *posePtr(r, j);
                float dx = b.pos[0] - a.pos[0], dz = b.pos[2] - a.pos[2];
                const float d2 = dx * dx + dz * dz;
                if (d2 >= minD * minD) continue;
                float d = std::sqrt(d2);
                if (d < 1e-5f) {
                    // Exactly coincident: there is no separating axis to
                    // compute, so take an arbitrary one. Fixed, not random —
                    // an RNG draw here would make a wall-clock-free sim depend
                    // on how many times something else happened to roll.
                    dx = 1.f;
                    dz = 0.f;
                    d = 1.f;
                }
                const float nx = dx / d, nz = dz / d; // points i -> j
                const float push = (minD - d) * phys.pushOut;
                // M-MASS: split by weight rather than evenly. This keeps the
                // property that mattered — the rule does not depend on player
                // INDEX, so the hero cannot bulldoze by virtue of being first —
                // while letting a body that has lost half its clay be the one
                // that gives ground. Total separation is unchanged.
                const float wi = bodies[i].invMass, wj = bodies[j].invMass;
                const float tot = wi + wj;
                const float fi = tot > 1e-6f ? wi / tot : 0.5f;
                a.pos[0] -= nx * push * fi;
                a.pos[2] -= nz * push * fi;
                b.pos[0] += nx * push * (1.f - fi);
                b.pos[2] += nz * push * (1.f - fi);
                bodies[i].killApproach(-nx, -nz);
                bodies[j].killApproach(nx, nz);
            }
        }
    }

    // ONE tick of the whole world, in the order a constraint solver wants:
    // every body decides and integrates, THEN the constraints run over all of
    // them at once.
    //
    // Bodies used to integrate and confine themselves inside their own tick(),
    // which cannot express a pairwise rule at all — collision is a relation,
    // not a property, and in the old order there was no moment when every
    // fighter had moved but none had yet been corrected.
    //
    // `driveHero` false = the hero's pose comes from ctl/replay (headless), so
    // it does not THINK — but it is still a body: it integrates, so knockback
    // moves it, and the constraints still run over it. An opponent able to
    // stand inside a journal-placed hero would be a strange thing to ship.
    // M-DEATH: adopt anything the renderer put back on the field. A respawn is
    // a teleport decided over there (it owns the carved ledger that triggers
    // it), and every fighter's DRIVER lives here — so the sim has to be told,
    // or it would keep steering off the momentum and the intent it had when it
    // died. The hero needs its pose copied back as well, because GameState's
    // copy is the authoritative one and setFighter overwrites the renderer's
    // every frame.
    void adoptRespawns(Renderer& r) {
        for (int i = 0; i < Renderer::kMaxPlayers; i++) {
            if (!r.takeRespawn(i)) continue;
            bodies[i] = Body{};
            // Re-stagger, or a body that comes back bounds in lockstep with
            // whoever is already out there — the constructor's reasoning, and
            // a wholesale Body{} throws it away. Starting the hopper at its
            // UNLOADED length (u = 0, not the sag) is deliberate too: the first
            // thing a new body does is settle onto its own weight, ~4 cm over a
            // fifth of a second, which reads as arriving rather than as popping
            // into existence already standing.
            bodies[i].gait = 0.37f * (float)i;
            bodies[i].vu = -0.21f * (float)i;
            if (i == 0) {
                fighter = r.player(0);
                swinging = false;
                punchT = -1.f;
            } else {
                foes[i] = OpponentAi{};
                foes[i].seed(i);
            }
        }
    }

    // Returns the number of ticks it ACTUALLY stepped, which is `ticks` minus
    // whatever hitstop ate. Every caller advances its render clock by the
    // return value rather than by the budget it handed in — that is what keeps
    // the picture, the sim and the journal's pose ticks on one clock through a
    // freeze, instead of letting the world slide underneath a held frame.
    int stepWorld(Renderer& r, const LookParams& look, int ticks, bool driveHero) {
        const float dt = (float)kTickDt;
        adoptRespawns(r);
        // M-MASS: re-weigh every body once per FRAME, not per tick. The number
        // behind it is a GPU readback that lands at most once a frame, so a
        // per-tick refresh would buy nothing but divisions.
        for (int i = 0; i < Renderer::kMaxPlayers; i++) {
            const float m = std::max(r.massFrac(i), std::max(phys.massMin, 0.05f));
            const float knob = std::min(std::max(phys.massKnock, 0.f), 1.f);
            bodies[i].invMass = 1.f + knob * (1.f / m - 1.f);
        }
        int stepped = 0;
        for (int k = 0; k < ticks; k++) {
            // HITSTOP: the world holds. Nothing thinks, nothing integrates,
            // nothing is constrained — and the tick is still CONSUMED, so the
            // freeze lasts real seconds instead of being outrun by whatever
            // frame rate the machine happens to be managing.
            if (hitCool > 0.f) hitCool -= dt;
            if (hold > 0.f) {
                hold -= dt;
                continue;
            }
            // A collapsed hero has nothing to drive: no body to move, no
            // weapon to swing. It still holds its pose so the camera has
            // something to frame while it waits to come back.
            const bool heroAlive = r.playerAlive(0);
            if (driveHero && heroAlive) {
                tick(look.rig, look.handPose.punchDur);
            } else if (heroAlive) {
                bodies[0].integrate(fighter, phys, look.rig, dt);
            }
            if (ai.enabled) {
                for (int i = 1; i < Renderer::kMaxPlayers; i++) {
                    if (!r.playerAlive(i)) continue;
                    // A dead hero is not a target. Steering at a corpse would
                    // park every opponent on the respawn point, waiting.
                    foes[i].tick(r.player(i), fighter, ai, bodies[i], phys,
                                 look.rig, look.handPose.punchDur, dt);
                    if (!heroAlive) foes[i].locked = false;
                }
            }
            resolveBodies(r);
            for (int i = 0; i < Renderer::kMaxPlayers; i++) {
                if (!liveBody(r, i)) continue;
                confineToArena(*posePtr(r, i), bodies[i], look.motion.arenaRadius);
            }
            stepped++;
        }
        return stepped;
    }

    double timeSeconds() const { return (double)tickCount * kTickDt; }
};

// The held sword breathes on the 12 Hz grid (GameState::swordOffset). Both
// the windowed loop and headless need it or they disagree about what the
// character looks like at rest — so it is applied through one helper rather
// than copied.
LookParams swordBobbed(GameState& g, const FighterPose& pose,
                       const LookParams& look, double t) {
    g.fighter = pose; // swordOffset reads `moving` for the bob's amplitude
    LookParams out = look;
    float sy, sp, lift;
    g.swordOffset((float)(std::floor(t * 12.0) / 12.0), sy, sp, lift);
    out.sword.yaw += sy;
    out.sword.pitch += sp;
    out.sword.pos[1] += lift;
    return out;
}

FrameInfo makeFrameInfo(double t, int aa) {
    FrameInfo f;
    f.time = (float)t;
    f.poseTime = (float)(std::floor(t * 12.0) / 12.0); // stop-motion pose steps
    f.grainFrame = (float)std::floor(t * 25.0);        // film frames (PAL)
    f.aaSamples = aa;
    return f;
}

long poseTickOf(double t) { return (long)std::floor(t * 12.0 + 1e-9); }

// break-on-condition (ctl `break ledger T`): pause instead of exiting so
// the state is inspectable via stats/shot/snap. One-shot; re-arm to renew.
// Only ctl can arm it, so it goes where ctl goes.
void checkBreak(CtlServer& ctl, const Renderer& renderer, SimClock& clock) {
#if !CLAYFRAY_DEV_TOOLS
    (void)ctl;
    (void)renderer;
    (void)clock;
#else
    if (ctl.breakLedgerTol < 0.f) return;
    const SplootStats& s = renderer.sploot();
    float res = (s.carved - (s.deposited + s.inFlight + s.debt)) * 1e6f;
    if (std::fabs(res) > ctl.breakLedgerTol) {
        clock.paused = true;
        std::fprintf(stderr,
                     "[break] ledger residual %.2f ml > %.2f ml — paused "
                     "(inspect via ctl; re-arm with `break ledger`)\n",
                     res, ctl.breakLedgerTol);
        ctl.breakLedgerTol = -1.f;
    }
#endif
}

// Debug/ablation overrides from the environment, applied to BOTH the windowed
// and headless paths.
//
// These used to live inline in runHeadless, which made them a trap rather than
// a tool: the windowed loop is the one that ships, and `--exit-after` (what
// tools/fairbench.sh drives) never saw them — so an ablation would silently
// measure nothing and read as "this term is free".
//
// NOTE WHAT THEY DO AND DO NOT ABLATE. `aoStrength` and `detailAmount` scale
// the RESULT, not the work: calcAO still runs all five mapLoose taps and the
// grain still evaluates its noise gradients, they just get multiplied by zero.
// Use them to see a look, not to price one. The ones that genuinely remove
// GPU work are `debugMode` (1 skips shade() entirely — no AO, no soft shadow,
// no albedo, no lighting) and a large `shadowSoft`, which makes softShadow's
// 22-step loop hit its early-out sooner.
// How many fighters to put on the field, for cost attribution.
// CLAYFRAY_PLAYERS=1 leaves the hero alone; the default is 2.
//
// This exists because "what does one more body cost in the march?" is the
// question that decides whether the player cap is a rendering problem or a
// memory one, and there was no way to ask it: every path hardcoded one
// addPlayer() call. Clamped to kMaxPlayers.
int wantedPlayers() {
    const char* n = std::getenv("CLAYFRAY_PLAYERS");
    if (!n) return 2;
    int v = std::atoi(n);
    if (v < 1) v = 1;
    if (v > Renderer::kMaxPlayers) v = Renderer::kMaxPlayers;
    return v;
}

// Spreads `count - 1` opponents around the hero so none of them is hidden
// behind another — an occluded body still costs march steps, but it would
// make the number depend on where they happened to stand.
//
// EVERY SPOT IS OUTSIDE AiParams::lockRange (1.50 m) and inside
// MotionParams::arenaRadius (2.60 m). Spawning inside the lock radius meant an
// opponent was already locked on at frame zero: it walked straight at the hero
// out of the dark and started throwing punches before the player had touched a
// key, so nobody ever saw it wander, and the fight had no beginning. Starting
// them out at ~2.3 m means they idle and roam until the player comes looking —
// which is also the only way to see the idle hand pose at all.
void addOpponents(Renderer& renderer, int count) {
    static const FighterPose kSpots[3] = {
        {{1.95f, 0.f, -1.20f}, 3.14159f, 0.f, false},
        {{-2.05f, 0.f, 0.90f}, 1.40f, 0.f, false},
        {{0.20f, 0.f, 2.25f}, 0.20f, 0.f, false},
    };
    for (int i = 1; i < count; i++) renderer.addPlayer(kSpots[(i - 1) % 3]);
}

void applyLookEnv(LookParams& look) {
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
}

std::string gCharacterPath; // --character; empty = built-in analytic fighter

bool loadCharacterInto(Renderer& renderer, const LookParams& look) {
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
    // BEFORE setCharacter: the voxelizer bakes the skin's colour once, so a
    // fighter imported with the default and recoloured afterwards would be
    // two-tone — old skin, new core.
    renderer.applyBodyColors(look);
    renderer.setCharacter(std::move(asset));
    return true;
}

struct RunOpts {
    std::string screenshotPath;
    // pinned trace resolution from --res (0 = derive from window * resScale)
    int traceW = 0, traceH = 0;
    // aa is rays-per-pixel-AXIS: the tracer loops aa*aa full marches per pixel
    // (trace.wgsl), so cost is quadratic in it. 2 -> 1 measured 3.87x faster
    // (232.5 -> 60.1 ms/frame, 960x540, RX 5700 XT) for a difference only
    // visible under magnification on the eyes — the marbles are the one hard
    // high-contrast edge in the scene; clay silhouettes and the grain hide it.
    // Raise back to 2 for beauty/reference renders.
    // DEFAULT = the shipping internal resolution. A 1280x720 window at
    // resScale 0.5 traces 640x360, so benchmarking at 1280x720 measured a
    // configuration the game never runs — and by a wide margin (57.8 ms vs
    // 21.4 ms per moving frame). Any timing run without an explicit --size now
    // measures what ships. Pass --size for beauty/reference renders.
    int width = 640, height = 360, frames = 8, aa = 1;
    double startTime = 2.0;
    // headless camera override (radians / meters); NaN = keep the default.
    // Inspecting an artifact from another angle otherwise needs serve mode.
    float camAz = NAN, camEl = NAN, camDist = NAN;
    bool carveTest = false;
    bool serve = false;          // headless, loop until ctl `quit`
    std::string replayPath;      // headless journal replay
    std::string loadName;        // snapshot to restore at launch
    int exitAfter = 0;           // windowed smoke test
};

#if CLAYFRAY_DEV_TOOLS
// The GPU-health gate every headless mode returns through: 4 if a pipeline was
// rejected or the device reported an uncaptured error, 0 otherwise.
//
// This exists because the two failures most likely to be catastrophic were the
// only ones with NO exit code. A rejected pipeline is an invalid object every
// later pass silently no-ops on (trap 8), and a bind-group layout mismatch
// (trap 2) renders black while balancing the ledger perfectly at zero. Both
// were diagnosed by asking a human to run `2>&1 | grep -c 'wgpu error'` — a
// check CLAUDE.md documents and the same file's iteration rules tell agents not
// to bother running. `--carve-test` grew a `carved <= 0` guard after trap 2
// bit, but `--replay` and `--screenshot` still exited 0 straight through a
// black screen. Now nothing does.
//
// 4 is deliberately distinct from 3 (conservation violation) and 1 (I/O).
int gpuHealthExit() {
    const int errs = gpuUncapturedErrorCount();
    if (!gpuAnyPipelineFailed() && errs == 0) return 0;
    std::fprintf(stderr,
                 "[gpu] UNHEALTHY: %s, %d uncaptured error(s). The render is not "
                 "trustworthy — see the [wgpu error] lines above.\n",
                 gpuAnyPipelineFailed() ? "a pipeline failed to create"
                                        : "pipelines ok",
                 errs);
    return 4;
}

// Every headless mode (--screenshot, --carve-test, --serve, --replay) is a
// desktop harness: no swapchain, a fixed 1/60 s step regardless of wall clock,
// and GPU backpressure held by blocking on the queue. A browser has none of
// those levers — it hands you a canvas and a frame callback — so the web
// target builds the windowed path only.
int runHeadless(const RunOpts& o) {
    Gpu gpu;
    if (!gpu.init(nullptr)) return 1;
    Renderer renderer;
    if (!renderer.init(gpu, o.width, o.height)) return 1;
    // Declared HERE rather than beside the camera below because the character
    // import reads look.bodyColor and the voxelizer bakes the skin's colour
    // exactly once — a LookParams built after this point could only ever
    // recolour the core.
    LookParams look;
    look.traceW = o.traceW;
    look.traceH = o.traceH;
    applyLookEnv(look);
    if (!loadCharacterInto(renderer, look)) return 1;
    // player 2: an identical fighter standing in front, facing the hero
    addOpponents(renderer, wantedPlayers());

    if (o.carveTest && std::getenv("CLAYFRAY_TEST_ADDSTRESS")) {
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
    } else if (o.carveTest && std::getenv("CLAYFRAY_TEST_NULLEDITS")) {
        // ops that touch nothing — but still trigger the per-edit JFA re-run
        for (int i = 0; i < 9; i++) {
            BrickEdit e;
            e.mode = 1;
            e.pos[0] = 0.f; e.pos[1] = 2.5f; e.pos[2] = 0.f; e.radius = 0.03f;
            renderer.queueBrickEdit(e);
        }
    } else if (o.carveTest) {
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
    cam.distance = look.cam.distance; // params own the default framing
    if (!std::isnan(o.camAz)) cam.azimuth = o.camAz;
    if (!std::isnan(o.camEl)) cam.elevation = o.camEl;
    if (!std::isnan(o.camDist)) cam.distance = o.camDist;

    double t = o.startTime;
    SimClock clock;
    bool quit = false;
    float fps = 0.f;
    FpsMeter fpsMeter;
    CtlServer ctl;
    // headless has no keyboard, so locomotion is whatever ctl/replay sets
    FighterPose fighter;
    // The idle sword bob lives in GameState::swordOffset, and until now only
    // frameOnce (windowed + web) applied it — so headless renders drew a
    // fighter holding a DEAD sword. That makes --screenshot disagree with the
    // running game, which is exactly what look-dev and the GIF read from.
    // Headless has no keyboard so nothing ever swings; this contributes the
    // bob only, driven by the ctl/replay pose.
    GameState bobState;
    CtlRefs refs;
    refs.look = &look;
    refs.cam = &cam;
    refs.renderer = &renderer;
    refs.clock = &clock;
    refs.simT = &t;
    refs.fps = &fps;
    refs.wantQuit = &quit;
    refs.fighter = &fighter;
    refs.ai = &bobState.ai;
    refs.phys = &bobState.phys;
    refs.charPath = gCharacterPath;
    ctl.init(o.serve ? "ctl" : "", refs);

    std::vector<JournalEntry> journal;
    size_t ji = 0;
    if (!o.replayPath.empty() && !loadJournal(o.replayPath, journal)) return 2;
    if (!o.loadName.empty() &&
        !renderer.loadSnapshot(snapFilePath(o.loadName), &t, gCharacterPath)) {
        return 2;
    }

    // probe/pickuv are ctl verbs, so they must work in REPLAY too, not just
    // serve — a journal that asserts what is under the cursor is the only way
    // to regression-test the pick path headlessly. One 1-workgroup dispatch.
    renderer.setAlwaysPick(true);

    if (o.serve) {
        std::printf("[ctl] serving on ctl/ at %dx%d aa=%d — drive with "
                    "tools/ctl.sh, stop with `quit`\n",
                    o.width, o.height, o.aa);
        int warm = 3; // render the bake/import + first visible frames
        int inFlight = 0;
        while (!quit) {
            long tick = poseTickOf(t);
            ctl.poll(tick);
            int n = clock.ticksToRun(kTickDt);
            if (n > 15) n = 15;
            bool active = n > 0 || ctl.activity() || warm > 0 ||
                          renderer.brick().hasPendingWork();
            if (active) {
                if (warm > 0) warm--;
                auto t0 = std::chrono::steady_clock::now();
                // Opponents think here too. The AI is deterministic (fixed dt,
                // seeded RNG) so this costs replay nothing, and a headless
                // render that showed a STATIONARY opponent would disagree with
                // the game the same way the missing sword bob used to.
                //
                // The hero does NOT think — ctl/replay owns its pose — but it
                // is still a body, so it is handed in, stepped for knockback
                // and collision, and handed back.
                bobState.fighter = fighter;
                bobState.applyContacts(renderer.contacts());
                // The render clock advances by what the world ACTUALLY ran, not
                // by the tick budget: hitstop holds both or neither.
                t += bobState.stepWorld(renderer, look, n, /*driveHero=*/false) *
                     kTickDt;
                fighter = bobState.fighter;
                renderer.setFighter(fighter);
                renderer.render(cam, swordBobbed(bobState, fighter, look, t),
                                makeFrameInfo(t, o.aa), nullptr, nullptr);
                gpu.processEvents();
                if (++inFlight >= kMaxFramesInFlight) {
                    gpu.waitForGpu();
                    inFlight = 0;
                }
                double dt = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
                fps = fpsMeter.tick(dt);
            } else {
                // idle: keep async maps, the inbox, and the ledger alive
                // without burning the GPU (a paused session would otherwise
                // never absorb an in-flight measurement into stats)
                gpu.processEvents();
                renderer.pumpLedger();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ctl.finishFrame();
            checkBreak(ctl, renderer, clock);
        }
        renderer.syncMeasurements();
        return gpuHealthExit();
    }

    // one queued edit drains per frame; +80 covers the longest test script
    int total = o.frames + (o.carveTest ? 80 : 0);
    if (!journal.empty()) {
        // enough frames to reach one pose tick past the last journal entry
        double tEnd = (double)(journal.back().tick + 1) / 12.0;
        total += std::max(0, (int)std::ceil((tEnd - o.startTime) * kTickRate) + 1);
    }
    int inFlight = 0;
    // Hitstop holds the render clock (stepWorld returns 0 ticks while it does),
    // so a run that freezes on every landing hit reaches a given pose tick
    // LATER than `total` frames assumed. Keep going while journal entries are
    // still pending rather than dropping them: a scenario that stops firing
    // half way through is a silently weaker gate, not a failing one. The slack
    // is a runaway guard — 600 frames is ten seconds of held picture, far more
    // than any journal accumulates at ~5 held ticks per landing hit.
    const int kHoldSlack = 600;
    for (int i = 0; i < total || (ji < journal.size() && i < total + kHoldSlack);
         i++) {
        long tick = poseTickOf(t);
        while (ji < journal.size() && journal[ji].tick <= tick) {
            std::string resp;
            if (!ctl.execute(journal[ji].cmd, tick, resp)) {
                std::fprintf(stderr, "[replay] entry %zu failed: %s", ji + 1,
                             resp.c_str());
            }
            ji++;
        }
        bobState.fighter = fighter;
        bobState.applyContacts(renderer.contacts());
        const int ran = bobState.stepWorld(renderer, look, 1, /*driveHero=*/false);
        fighter = bobState.fighter;
        renderer.setFighter(fighter);
        renderer.render(cam, swordBobbed(bobState, fighter, look, t),
                        makeFrameInfo(t, o.aa), nullptr, nullptr);
        // headless still needs the event pump or async readbacks (volume
        // ledger, capacity poll) starve until exit
        gpu.processEvents();
        if (++inFlight >= kMaxFramesInFlight) {
            gpu.waitForGpu();
            inFlight = 0;
        }
        // replay is the regression harness: pin measurement arrival to the
        // frame after its edit so reruns are bit-for-bit repeatable
        if (!journal.empty()) renderer.syncMeasurements();
        ctl.finishFrame(); // flush journal `shot` lines
        t += ran * kTickDt;
    }
    if (ji < journal.size()) {
        std::fprintf(stderr, "[replay] %zu entr(y/ies) past end of run\n",
                     journal.size() - ji);
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
    std::printf("[reuse] traced %llu of %llu frames (%.1f%% skipped)\n",
                (unsigned long long)renderer.framesTraced(),
                (unsigned long long)renderer.framesPresented(),
                renderer.framesPresented()
                    ? 100.0 * (1.0 - (double)renderer.framesTraced() /
                                         (double)renderer.framesPresented())
                    : 0.0);
    if (renderer.traceMs() > 0.f) {
        std::printf("[gpu] trace %.2f ms  post %.2f ms (smoothed, %dx%d aa=%d)\n",
                    renderer.traceMs(), renderer.postMs(), o.width, o.height, o.aa);
    }
    if (!o.screenshotPath.empty() && !renderer.screenshot(o.screenshotPath)) return 1;

    // Machine-checkable conservation gate: at any instant, carved clay is
    // accounted for as landed + airborne + owed. A nonzero residual means a
    // measurement or a gob was dropped — a real regression. Agents/CI can
    // gate on this exit code instead of eyeballing the ledger line.
    if ((o.carveTest || !journal.empty()) && look.conserveClay) {
        // A balance of 0 == 0 is NOT a pass for --carve-test: it always carves,
        // so carving nothing means the run never got that far. That is exactly
        // what a bind-group validation failure looks like from out here — the
        // app boots, imports, prints a healthy banner, renders black, and
        // balances perfectly at zero. This gate reported success through it
        // (see trap 2), so make the silence itself the failure.
        // ...but only for the DEFAULT carve-test. The two env-var variants
        // carve nothing on purpose: ADDSTRESS only deposits, and NULLEDITS
        // fires ops that deliberately touch no clay.
        const bool plainCarveTest = o.carveTest &&
                                    !std::getenv("CLAYFRAY_TEST_ADDSTRESS") &&
                                    !std::getenv("CLAYFRAY_TEST_NULLEDITS");
        if (plainCarveTest && s.carved <= 0.f) {
            std::fprintf(stderr,
                         "[sploot] CARVE TEST CARVED NOTHING (0.0 ml). The "
                         "ledger 'balances' but no edit landed — check the run "
                         "for wgpu validation errors.\n");
            return 3;
        }
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
    return gpuHealthExit();
}
#endif // CLAYFRAY_DEV_TOOLS

// Everything one frame of the windowed app touches, in one object with a
// STABLE address.
//
// Native could keep these as locals in runWindowed — the `while` loop is right
// there. A browser cannot: it owns the loop, calls us back one frame at a
// time, and runWindowed has long since returned by the first frame. So the
// state is heap-allocated and both platforms drive the same frameOnce() over
// it. Stable address matters twice over: CtlRefs stores raw pointers into
// these members, and the emscripten callback holds a void* to the whole thing.
struct AppState {
    RunOpts o;
    SDL_Window* window = nullptr;
    Gpu gpu;
    Renderer renderer;
    LookParams look;
    GameState game;
    OrbitCamera cam;
    BrushState brush;
    SimClock clock;
    CtlServer ctl;
    TouchControls touch;

    double simT = 0.0;
    bool ctlQuit = false;
    bool running = true;
    float fps = 0.f;
    FpsMeter fpsMeter;

    int pw = 0, ph = 0;            // backing (pixel) size of the window
    long lastCamPose = -1;
    uint64_t lastTraced = 0, lastPresented = 0;
    float reuseSkipPct = 0.f;
    uint64_t prevNs = 0;
    int frameCounter = 0;
    int screenshotCounter = 0;
#ifdef __EMSCRIPTEN__
    // last canvas CSS size we resized to, so the poll below reacts to the PAGE
    // changing size and not to its own previous write (which would oscillate)
    int lastCssW = 0, lastCssH = 0;
#endif
};

#ifdef __EMSCRIPTEN__
// The canvas decides the resolution on web, not a hardcoded 1280x720: the
// element is laid out by the page's CSS and may be any size at all.
//
// DPR is REPORTED but deliberately NOT multiplied into the backing store, for
// the same reason SDL_CreateWindow does not pass SDL_WINDOW_HIGH_PIXEL_DENSITY
// on the desktop: tracing at retina density quadruples the traced pixels for
// ~4x the frame time, and the film grain hides the difference. Reporting it
// anyway is what keeps the `[res]` line honest — a 2x display genuinely is
// showing an upscaled image, and that must be visible in the log rather than
// inferred.
bool webCanvasCssSize(int& w, int& h, double& dpr) {
    double cw = 0, ch = 0;
    dpr = emscripten_get_device_pixel_ratio();
    if (emscripten_get_element_css_size("#canvas", &cw, &ch) != EMSCRIPTEN_RESULT_SUCCESS)
        return false;
    if (cw < 1 || ch < 1) return false;
    w = (int)cw;
    h = (int)ch;
    return true;
}
#endif

// Post-GPU startup: everything that needs a device. Split out of runWindowed
// because on web it runs from the device callback, a browser task or two after
// runWindowed returned (see Gpu::initAsync).
bool appStartAfterGpu(AppState& s) {
    const RunOpts& o = s.o;
    SDL_GetWindowSizeInPixels(s.window, &s.pw, &s.ph);
    s.gpu.configureSurface(s.pw, s.ph);

    LookParams& look = s.look;
    look.traceW = o.traceW;
    look.traceH = o.traceH;
    // Same debug/ablation overrides the headless path gets. The windowed loop
    // is the one that ships and the one tools/fairbench.sh measures, so an
    // ablation it cannot see is worse than no ablation — it reads as "free".
    applyLookEnv(look);
    // Trace size is decided ONCE here and reported, rather than drifting in
    // from the every-10-frames resize below: the first frames would otherwise
    // trace at full window size and a --res run would not honour the flag
    // until frame 10.
    const int traceW0 = look.traceW > 0 ? look.traceW
                                        : std::max(160, (int)(s.pw * look.resScale));
    const int traceH0 = look.traceH > 0 ? look.traceH
                                        : std::max(90, (int)(s.ph * look.resScale));
    {
        // ALWAYS report what is actually traced. Frame cost is per traced
        // pixel, and the window size does not tell you that: SDL reports
        // BACKING pixels, which differ from the logical size on a scaled
        // display, and resScale then divides them. Two machines "both at
        // 1280x720" can be tracing 4x different pixel counts, which silently
        // invalidates every timing comparison between them.
        //
        // The browser adds a third way to be wrong — the canvas is sized by
        // the page's CSS and the display has its own pixel ratio — so web
        // appends the DPR. Without it, "960x540 backing" on a 2x laptop reads
        // as if it were 1920x1080 of real pixels when it is an upscale.
        int lw = 0, lh = 0;
        SDL_GetWindowSize(s.window, &lw, &lh);
        char note[64] = "";
#ifdef __EMSCRIPTEN__
        std::snprintf(note, sizeof(note), ", canvas dpr %.2f (not applied)",
                      emscripten_get_device_pixel_ratio());
#endif
        std::printf("[res] window %dx%d logical, %dx%d backing%s -> TRACING %dx%d%s\n",
                    lw, lh, s.pw, s.ph, note, traceW0, traceH0,
                    look.traceW > 0 ? " (pinned by --res)" : " (resScale)");
        std::fflush(stdout);
    }

    if (!s.renderer.init(s.gpu, traceW0, traceH0)) return false;
    if (!loadCharacterInto(s.renderer, s.look)) return false;
    // player 2: an identical fighter standing in front, facing the hero
    addOpponents(s.renderer, wantedPlayers());
    if (!uiInit(s.window, s.gpu)) return false;

    // Touch controls are shown BEFORE the first touch, or a phone opens to what
    // looks like a screensaver: no keyboard, so no way in. The probe wants BOTH
    // a touch point and a coarse pointer — `maxTouchPoints > 0` alone fires on
    // every convertible laptop sitting on a desk with a mouse. Anything the
    // probe misses still latches on the first finger event (src/touch.h).
    // CLAYFRAY_TOUCH=1/0 forces it either way, which is how the overlay gets
    // laid out and eyeballed on the desktop.
    {
        int want = 0;
#ifdef __EMSCRIPTEN__
        want = EM_ASM_INT({
            return (navigator.maxTouchPoints > 0 &&
                    window.matchMedia('(pointer: coarse)').matches)
                       ? 1
                       : 0;
        });
#endif
        if (const char* e = std::getenv("CLAYFRAY_TOUCH")) want = std::atoi(e);
        s.touch.active = want > 0;
        if (s.touch.active) std::printf("[touch] on-screen controls enabled\n");
    }

    CtlRefs refs;
    refs.look = &s.look;
    refs.cam = &s.cam;
    refs.brush = &s.brush;
    refs.fighter = &s.game.fighter;
    refs.ai = &s.game.ai;
    refs.phys = &s.game.phys;
    refs.renderer = &s.renderer;
    refs.clock = &s.clock;
    refs.simT = &s.simT;
    refs.fps = &s.fps;
    refs.wantQuit = &s.ctlQuit;
    refs.charPath = gCharacterPath;
    s.ctl.init(CLAYFRAY_DEV_TOOLS ? "ctl" : "", refs);

#if CLAYFRAY_DEV_TOOLS
    if (!o.loadName.empty()) {
        if (!s.renderer.loadSnapshot(snapFilePath(o.loadName), &s.simT, gCharacterPath)) {
            return false;
        }
        s.game.tickCount = (uint64_t)std::llround(s.simT * kTickRate);
    }
#endif

    // A pipeline that failed to create leaves an invalid object that every
    // later pass silently no-ops on, so say so ONCE here rather than let the
    // user stare at a black canvas. The scope callbacks are async, so this
    // catches whatever has been delivered by now; the per-pipeline message
    // from GpuPipelineScope is the authoritative one either way.
    if (gpuAnyPipelineFailed()) {
        std::fprintf(stderr,
                     "[startup] at least one pipeline failed to create — the "
                     "render will be black or partial. See the [wgpu] lines "
                     "above.\n");
    }

    s.prevNs = SDL_GetTicksNS();
    return true;
}

// ONE frame. Native calls this from its `while`; the browser calls it from
// requestAnimationFrame. Nothing in here may block — see platform.h note 1.
void frameOnce(AppState& s) {
    // Local aliases keep the body below textually identical to the loop this
    // was hoisted out of, which is what makes the restructure reviewable.
    SDL_Window* window = s.window;
    Gpu& gpu = s.gpu;
    Renderer& renderer = s.renderer;
    LookParams& look = s.look;
    GameState& game = s.game;
    OrbitCamera& cam = s.cam;
    BrushState& brush = s.brush;
    SimClock& clock = s.clock;
    CtlServer& ctl = s.ctl;
    const RunOpts& o = s.o;
    bool& running = s.running;
    bool& ctlQuit = s.ctlQuit;
    int& pw = s.pw;
    int& ph = s.ph;
    double& simT = s.simT;
    float& fps = s.fps;
    FpsMeter& fpsMeter = s.fpsMeter;
    long& lastCamPose = s.lastCamPose;
    uint64_t& lastTraced = s.lastTraced;
    uint64_t& lastPresented = s.lastPresented;
    float& reuseSkipPct = s.reuseSkipPct;
    uint64_t& prevNs = s.prevNs;
    int& frameCounter = s.frameCounter;
    int& screenshotCounter = s.screenshotCounter;
    (void)ctlQuit;
    (void)screenshotCounter;

    {
        // Logical (not backing) size: it is the space finger events normalize
        // against and the space ImGui draws in, so the touch hit boxes and the
        // touch overlay cannot disagree about where the button is.
        int lw = 0, lh = 0;
        SDL_GetWindowSize(window, &lw, &lh);

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            uiProcessEvent(&ev);
            // Touch controls get first refusal on every finger. What they claim
            // (stick zone, action button) never reaches the camera or the
            // sculpt brush; everything else falls through to the synthetic
            // mouse, which is what still makes drag-orbit and the ImGui panel
            // work under a thumb. See src/touch.h.
            if (s.touch.handleEvent(ev, (float)lw, (float)lh)) continue;
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
                // SPACE swings. Edge-triggered (not repeat) so holding it
                // doesn't retrigger every frame mid-arc.
                if (ev.key.key == SDLK_SPACE && !ev.key.repeat)
                    game.swingRequested = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                // `!touch.engaged()`: a thumb on the stick or the button also
                // drives the synthetic mouse, so without this the fighter would
                // orbit the camera every time it walked.
                if (!uiWantsMouse() && !s.touch.engaged() && brush.mode == 0 &&
                    (ev.motion.state & SDL_BUTTON_LMASK)) {
                    cam.azimuth -= ev.motion.xrel * 0.005f;
                    if (look.cam.lockHeight) {
                        // Elevation is derived, so dragging it directly would
                        // be overwritten on the next frame and read as a dead
                        // control. Move the thing that is actually authoritative.
                        look.cam.height += ev.motion.yrel * 0.005f * cam.distance;
                    } else {
                        cam.elevation += ev.motion.yrel * 0.005f;
                        if (cam.elevation < -0.35f) cam.elevation = -0.35f;
                        if (cam.elevation > 1.2f) cam.elevation = 1.2f;
                    }
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

        long tickNow = poseTickOf(simT);
        ctl.poll(tickNow); // before sim/render: same-frame set -> shot

        // WASD walks, camera-relative: forward is away from the camera, so
        // the fighter always runs "into the screen" whatever the orbit is.
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            float fx = -std::sin(cam.azimuth), fz = -std::cos(cam.azimuth);
            float rx = std::cos(cam.azimuth), rz = -std::sin(cam.azimuth);
            float mx = 0.f, mz = 0.f;
            if (keys[SDL_SCANCODE_W]) { mx += fx; mz += fz; }
            if (keys[SDL_SCANCODE_S]) { mx -= fx; mz -= fz; }
            if (keys[SDL_SCANCODE_D]) { mx += rx; mz += rz; }
            if (keys[SDL_SCANCODE_A]) { mx -= rx; mz -= rz; }
            // The thumb stick adds into the same vector on the same basis, so
            // there is no touch "mode" to be in and no second code path for the
            // sim to disagree with.
            s.touch.addMovement(cam.azimuth, mx, mz);
        // Look drag from the top half of the screen. Same sensitivity and the
        // same sign as the mouse, and the same height-vs-elevation split — the
        // camera must not behave differently depending on which pointer moved it.
        {
            float ldx = 0.f, ldy = 0.f;
            if (s.touch.takeLook(ldx, ldy)) {
                cam.azimuth -= ldx * 0.005f;
                if (look.cam.lockHeight) {
                    look.cam.height += ldy * 0.005f * cam.distance;
                } else {
                    cam.elevation += ldy * 0.005f;
                    cam.elevation = std::min(std::max(cam.elevation, -0.35f), 1.2f);
                }
            }
        }
            game.moveX = mx;
            game.moveZ = mz;
        }
        // Edge-triggered exactly like SPACE, so a resting thumb does not
        // retrigger the swing mid-arc.
        if (s.touch.takeSwing()) game.swingRequested = true;

        uint64_t nowNs = SDL_GetTicksNS();
        double frameDt = (double)(nowNs - prevNs) * 1e-9;
        prevNs = nowNs;
        if (frameDt > 0.25) frameDt = 0.25; // debugger/stall clamp
        int ticks = clock.ticksToRun(frameDt);
        if (ticks > 15) ticks = 15;
        // The sword's own switch decides whether SPACE swings or jabs, so the
        // hero's fists come up the moment `sword.enabled` goes off from ctl or
        // the panel — no separate mode to keep in sync.
        game.unarmed = !look.sword.enabled;
        // M-PHYS: last frame's weapon contacts become this frame's forces,
        // hitstun and strike resistance. One frame of lag by construction —
        // contacts are resolved during render(), which is the only place the
        // posed weapons exist — and it is invisible at 60 Hz.
        game.applyContacts(renderer.contacts());
        simT += game.stepWorld(renderer, look, ticks, /*driveHero=*/true) * kTickDt;
        renderer.setFighter(game.fighter);
        // Keep the walker framed — but on the SAME clock the body is drawn on.
        // Chasing the 60 Hz sim position while the body steps at 12 Hz slides
        // the camera against a stepping subject, which reads as jitter (worse
        // than either being stepped or both smooth), and it changed a traced
        // input every frame, which defeated frame reuse in exactly the case
        // root quantisation exists to fix. Runs after simT advances so it sees
        // the pose tick the renderer will use this frame.
        {
            const long camPose = poseTickOf(simT);
            const bool step = look.motion.stepRoot;
            if (!step || camPose != lastCamPose) {
                lastCamPose = camPose;
                // same easing, re-expressed per pose step when stepping
                const float k = step ? 0.29f : std::min(1.f, 4.f * (float)frameDt);
                cam.target.x += (game.fighter.pos[0] - cam.target.x) * k;
                cam.target.y += (game.fighter.pos[1] + 0.45f - cam.target.y) * k;
                cam.target.z += (game.fighter.pos[2] - cam.target.z) * k;
            }
            // Height is the authority, elevation the derived quantity — every
            // frame, not just on a pose step, because `distance` can change
            // between them (scroll wheel) and the eye must not dip when it does.
            if (look.cam.lockHeight && cam.distance > 1e-3f) {
                look.cam.height =
                    std::min(std::max(look.cam.height, look.cam.minHeight),
                             cam.distance * look.cam.maxHeightFrac);
                cam.elevation = std::asin(look.cam.height / cam.distance);
            }
        }
        fps = fpsMeter.tick(frameDt);

        frameCounter++;
#if CLAYFRAY_DEV_TOOLS
        // shader hot reload: a stat() sweep of the source tree's shaders/,
        // which on web would be a sweep of read-only preloaded MEMFS
        if (frameCounter % 30 == 0) renderer.reloadShadersIfChanged();
#endif
        if (o.exitAfter > 0 && frameCounter >= o.exitAfter) running = false;

        // internal resolution scale (throttled so slider drags don't thrash
        // target recreation). CONSTANT by design — a dynamic drop while the
        // view moved was tried and removed: resScale is already 0.5, so it
        // took the window to 0.31 of native for a frame rate the constant 0.5
        // already reaches, and the resolution switch itself was visible.
        if (frameCounter % 10 == 0) {
#ifdef __EMSCRIPTEN__
            // The page can resize the canvas at any time (window resize, a
            // responsive layout, devtools opening) and that arrives as a CSS
            // change, not as an SDL event we can rely on. Poll it on the same
            // throttle as the trace-size check.
            //
            // Compared against the last size WE acted on, never against the
            // current window size: SDL_SetWindowSize writes the canvas back,
            // so comparing the two would let a rounding difference oscillate
            // forever, resizing render targets every 10 frames.
            {
                int cw = 0, ch = 0;
                double dpr = 1.0;
                if (webCanvasCssSize(cw, ch, dpr) &&
                    (cw != s.lastCssW || ch != s.lastCssH)) {
                    s.lastCssW = cw;
                    s.lastCssH = ch;
                    SDL_SetWindowSize(window, cw, ch);
                    SDL_GetWindowSizeInPixels(window, &pw, &ph);
                    gpu.configureSurface(pw, ph);
                }
            }
#endif
            // A pinned size wins over resScale: frame cost is per traced
            // pixel, so this is what makes two machines comparable.
            int tw = look.traceW > 0 ? look.traceW
                                     : std::max(160, (int)(pw * look.resScale));
            int th = look.traceH > 0 ? look.traceH
                                     : std::max(90, (int)(ph * look.resScale));
            if (tw != renderer.width() || th != renderer.height()) renderer.resize(tw, th);
        }

        // sculpt: pick under the cursor every frame; apply while LMB held
        float mx = 0.f, my = 0.f;
        SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
        int ww = 0, wh = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        if (ww > 0 && wh > 0) renderer.setPickUV(mx / (float)ww, my / (float)wh);
        if (brush.mode != 0 && (buttons & SDL_BUTTON_LMASK) && !uiWantsMouse() &&
            !s.touch.engaged() && renderer.pickValid()) {
            BrickEdit e;
            e.mode = brush.mode;
            // REST space, not world: the brick volume is authored in rest
            // space, so carving a fighter that has walked or posed away from
            // the origin needs the hit mapped back through the articulation.
            const float* p = renderer.pickRest();
            e.pos[0] = p[0];
            e.pos[1] = p[1];
            e.pos[2] = p[2];
            // The rest point belongs to the fighter the ray actually hit, so
            // the edit has to name it — otherwise clicking on an opponent
            // carves the same coordinates out of the HERO's slice, which looks
            // like the click landing on the wrong body.
            e.player = renderer.pickPlayer() > 0 ? renderer.pickPlayer() : 0;
            e.radius = brush.radius;
            e.color[0] = brush.color[0];
            e.color[1] = brush.color[1];
            e.color[2] = brush.color[2];
            // pick-resolved edit recorded so replay doesn't need a cursor
            BrickEdit resolved = renderer.queueBrickEdit(e);
            if (ctl.recording()) ctl.recordEdit(resolved, tickNow);
        }

        bool wantScreenshot = false;
        // reuse rate over a short window, so the panel reacts to what you are
        // doing right now rather than showing a lifetime average
        {
            uint64_t tr = renderer.framesTraced(), pr = renderer.framesPresented();
            uint64_t dT = tr - lastTraced, dP = pr - lastPresented;
            if (dP >= 30) {
                reuseSkipPct = 100.f * (1.f - (float)dT / (float)dP);
                lastTraced = tr;
                lastPresented = pr;
            }
        }
        uiSetCompact(s.touch.active);
        uiNewFrame(look, brush, fps, renderer.traceMs(), renderer.postMs(),
                   renderer.sploot(), reuseSkipPct, wantScreenshot);
        // After the panel, so the overlay's foreground draw list is built with
        // this frame's panel rect already recorded.
        s.touch.draw();

        wgpu::SurfaceTexture surfaceTex;
        gpu.surface.GetCurrentTexture(&surfaceTex);
        if (surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
            surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
            wgpu::TextureView view = surfaceTex.texture.CreateView();
            // the swing rides ON TOP of the panel's hold pose, so the sliders
            // stay authoritative for the guard and the arc is a transient
            LookParams frameLook = look;
            float sy, sp, lift;
            game.swordOffset((float)(std::floor(simT * 12.0) / 12.0), sy, sp, lift);
            frameLook.sword.yaw += sy;
            frameLook.sword.pitch += sp;
            frameLook.sword.pos[1] += lift;
            renderer.render(cam, frameLook, makeFrameInfo(simT, 1), view,
                            [](wgpu::RenderPassEncoder& pass) { uiRender(pass); });
#ifndef __EMSCRIPTEN__
            // The browser presents the canvas itself when the rAF callback
            // returns, so there is nothing to call — and emdawnwebgpu makes
            // that explicit: wgpuSurfacePresent is a hard abort() there ("use
            // requestAnimationFrame via html5.h instead"), not a no-op. Same
            // shape as trap 9: browser-illegal calls kill the module rather
            // than failing softly.
            gpu.surface.Present();
#endif
        } else {
            // Skipped frame (e.g. mid-resize); ImGui frame must still be closed out.
            ImGui::Render();
        }

#if CLAYFRAY_DEV_TOOLS
        if (wantScreenshot) {
            char path[64];
            std::snprintf(path, sizeof(path), "lookdev/capture_%03d.png",
                          screenshotCounter++);
            renderer.screenshot(path);
        }
#else
        (void)wantScreenshot;
        (void)screenshotCounter;
#endif
        gpu.processEvents();
        ctl.finishFrame();
        checkBreak(ctl, renderer, clock);
    }
}

void appShutdown(AppState& s) {
    uiShutdown();
    SDL_DestroyWindow(s.window);
    SDL_Quit();
}

#ifdef __EMSCRIPTEN__
// The browser's frame callback. emscripten_set_main_loop_arg with fps 0 means
// requestAnimationFrame, i.e. the display's own cadence — which is also the
// backpressure that Gpu::waitForGpu provides for the headless desktop loops.
void webFrame(void* userData) {
    AppState& s = *static_cast<AppState*>(userData);
    frameOnce(s);
    if (!s.running || s.ctlQuit) {
        emscripten_cancel_main_loop();
        appShutdown(s);
    }
}
#endif

int runWindowed(const RunOpts& o) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Desktop opens a 1280x720 window; the browser has no such freedom — the
    // canvas is however big the page's CSS made it, and 1280x720 would either
    // overflow the layout or be silently stretched. Ask the element.
    int winW = 1280, winH = 720;
#ifdef __EMSCRIPTEN__
    {
        int cw = 0, ch = 0;
        double dpr = 1.0;
        if (webCanvasCssSize(cw, ch, dpr)) {
            winW = cw;
            winH = ch;
        } else {
            std::fprintf(stderr,
                         "[web] could not read #canvas CSS size; falling back "
                         "to %dx%d\n",
                         winW, winH);
        }
    }
#endif
    // no HIGH_PIXEL_DENSITY: retina-native quadruples the traced pixels for
    // ~4x the frame time, and the film look hides the difference
    SDL_Window* window = SDL_CreateWindow("clayfray", winW, winH, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    // Heap, not stack: on web this outlives runWindowed by the whole session
    // (see AppState). Native deletes it at the bottom; web never does, because
    // the tab closing is the only end of life there.
    AppState* s = new AppState();
    s->o = o;
    s->window = window;
#ifdef __EMSCRIPTEN__
    s->lastCssW = winW;
    s->lastCssH = winH;
#endif

#if CLAYFRAY_HAS_BLOCKING_GPU_WAIT
    // Native: initAsync completes synchronously, so this reads exactly like
    // the straight-line startup it replaced.
    bool gpuOk = false;
    s->gpu.initAsync(window, [&gpuOk](bool ok) { gpuOk = ok; });
    if (!gpuOk || !appStartAfterGpu(*s)) {
        delete s;
        return 1;
    }
    while (s->running && !s->ctlQuit) frameOnce(*s);
    appShutdown(*s);
    delete s;
    return 0;
#else
    // Browser: RETURN, don't loop. Everything downstream of the device runs
    // from the callback, and the frame loop is handed to the browser. Falling
    // off the end of main() here is correct and expected — the module stays
    // alive because emscripten_set_main_loop_arg registered a callback.
    s->gpu.initAsync(window, [s](bool ok) {
        if (!ok) {
            std::fprintf(stderr, "[web] GPU init failed; nothing will render\n");
            return;
        }
        if (!appStartAfterGpu(*s)) {
            std::fprintf(stderr, "[web] renderer init failed; nothing will render\n");
            return;
        }
        // fps 0 = requestAnimationFrame; simulate_infinite_loop false = let
        // runWindowed's caller return normally rather than throwing the
        // unwind exception, which would be caught as an error by the shell.
        emscripten_set_main_loop_arg(webFrame, s, 0, /*simulate_infinite_loop=*/false);
    });
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    // mintty/pipes fully buffer stdout, hiding startup progress for minutes
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    RunOpts o;
    bool sizeSet = false, aaSet = false;

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
            o.screenshotPath = next("--screenshot");
        } else if (arg == "--size") {
            if (std::sscanf(next("--size"), "%dx%d", &o.width, &o.height) != 2) {
                std::fprintf(stderr, "--size expects WxH\n");
                return 2;
            }
            sizeSet = true;
        } else if (arg == "--frames") {
            o.frames = std::atoi(next("--frames"));
        } else if (arg == "--time") {
            o.startTime = std::atof(next("--time"));
        } else if (arg == "--cam") {
            // --cam AZ,EL,DIST (radians, radians, meters): inspect an artifact
            // from any angle headlessly instead of driving serve mode
            if (std::sscanf(next("--cam"), "%f,%f,%f", &o.camAz, &o.camEl,
                            &o.camDist) != 3) {
                std::fprintf(stderr, "--cam expects AZ,EL,DIST\n");
                return 2;
            }
        } else if (arg == "--res") {
            // Pin the TRACED resolution, ignoring window size and resScale.
            // The point is cross-machine comparability: `--res 640x360` costs
            // the same work everywhere, so a frame time from one box means
            // something on another.
            if (std::sscanf(next("--res"), "%dx%d", &o.traceW, &o.traceH) != 2) {
                std::fprintf(stderr, "--res expects WxH\n");
                return 2;
            }
        } else if (arg == "--aa") {
            o.aa = std::atoi(next("--aa"));
            aaSet = true;
        } else if (arg == "--exit-after") {
            o.exitAfter = std::atoi(next("--exit-after"));
        } else if (arg == "--carve-test") {
            o.carveTest = true;
        } else if (arg == "--character") {
            gCharacterPath = next("--character");
        } else if (arg == "--serve") {
            o.serve = true;
        } else if (arg == "--replay") {
            o.replayPath = next("--replay");
        } else if (arg == "--load") {
            o.loadName = next("--load");
        } else {
            std::fprintf(stderr,
                         "usage: clayfray [--screenshot out.png] [--size WxH] "
                         "[--frames N] [--time T] [--aa N] [--character f.glb]\n"
                         "  [--serve]        headless ctl session (tools/ctl.sh)\n"
                         "  [--replay f]     headless journal replay (deterministic)\n"
                         "  [--load name]    restore a snapshot at launch\n"
                         "  [--carve-test] [--exit-after N]\n");
            return 2;
        }
    }
    if (o.serve) {
        // serve is an interactive sim console: default to a fast trace
        if (!sizeSet) { o.width = 960; o.height = 540; }
        if (!aaSet) o.aa = 1;
    }

#if CLAYFRAY_DEV_TOOLS
    if (!o.screenshotPath.empty() || o.serve || !o.replayPath.empty()) {
        return runHeadless(o);
    }
#else
    // Flag PARSING stays identical on web so the two builds cannot drift over
    // what a flag means; only the modes that need a real OS are unreachable.
    if (!o.screenshotPath.empty() || o.serve || !o.replayPath.empty() ||
        !o.loadName.empty()) {
        std::fprintf(stderr,
                     "[web] headless modes and snapshots are desktop-only; "
                     "running windowed\n");
    }
#endif
    return runWindowed(o);
}
