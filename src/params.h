#pragma once

// M5 locomotion: where the fighter stands and which way it is going. The
// whole skeleton is premultiplied by this, so pose clips stay authored in
// character space and the sword/hands follow for free.
struct FighterPose {
    float pos[3] = {0.f, 0.f, 0.f}; // world, FEET, on the arena floor
    float yaw = 0.f;                // facing, radians about +Y
    float lean = 0.f;               // radians tilted INTO the direction of travel
    bool moving = false;            // steering DEMAND, not actual travel
    // ---- M-SPRING: the hopper's contact coordinate, metres ----
    //
    // Below zero the body is compressed by -hopU against the floor; at or above
    // zero its feet are off the floor by hopU. Written by the 60 Hz sim
    // (Body::stepHop in main.cpp) and read by the renderer, which derives both
    // the squish and the lift from it.
    //
    // It is on the POSE rather than private to the sim because it stopped being
    // decoration: a fighter only travels while this is positive, so it is the
    // single fact both halves of the game have to agree on.
    float hopU = 0.f;
    // ---- M-FIST: what the mitts are doing ----
    // `guard` raises them into the fighting pose (closed fists, up in front of
    // the face); otherwise they hang loose in the idle pose. `punch` drives
    // the lead fist out along the fighter's own forward — 0 = at guard, 1 =
    // fully extended — and `punchSide` picks the mitt (0 = the authored side).
    //
    // This is SIM state, so it is not quantised here: the driver advances the
    // timer on the fixed 60 Hz tick and reads the curve off the 12 Hz pose
    // clock at the point of use, exactly as the sword's swing does (trap 4).
    // A fighter holding the sword ignores all three — the grip wins.
    bool guard = false;
    float punch = 0.f;
    int punchSide = 0;
};

// M4.7 sword prop + hand IK. The hilt transform is driven by the harness's
// guard-and-swing pose (main.cpp GameState::swordOffset), NOT by hand any
// more — these are the rest values it offsets from. Dragging pos/yaw/pitch
// live fights the animation, which is why they left the panel for ctl.
struct SwordParams {
    bool enabled = true;             // hold the sword + run hand IK
    // carried: `pos`/`yaw` are read in CHARACTER space and ride the fighter's
    // root, so walking carries the sword and the hands keep their grip. Off =
    // the old debug behaviour, hilt placed directly in world space.
    bool carry = true;
    float pos[3] = {0.16f, 0.26f, 0.30f}; // hilt, in front of and beside the chest
    float yaw = 0.f;                 // radians about world Y
    // guard pose: blade STRAIGHT UP (pitch = pi/2 makes dir = +Y). The swing
    // drops it to horizontal and sweeps; at rest it stands vertical.
    float pitch = 1.5708f;
    float length = 0.85f;            // hilt -> tip, meters
    float radius = 0.018f;           // blade thickness
    // grip spacing must clear the mitt's own thickness along the handle
    // (~0.10 m on this rig) or the two hands interpenetrate
    float grip0 = 0.05f;             // near-hilt grip (one hand), m along blade
    float grip1 = 0.19f;             // stacked grip (other hand), m along blade
    float color[3] = {0.35f, 0.85f, 1.0f}; // debug lightsaber cyan (emissive)

    // ---- slice sploot (M4.6 ledger, sword side) ----
    // A SLICE ejects ONE gob carrying the whole volume the cut removed,
    // launched as the blade leaves the body — not the ledger's default
    // dribble of 2..35 ml pellets, which reads as a sneeze rather than a
    // wound. The brush keeps the dribble; this is sword-only.
    bool sliceGob = true;    // off = blade carves dribble like the brush
    float sliceSpeed = 1.5f; // launch speed, m/s (the blade's shove)
    float sliceLift = 0.8f;  // extra +Y m/s so the blob arcs instead of skidding
    // 0 = fly purely along the blade's sweep, 1 = purely along the wound
    // normal (straight out of the cut). Between the two reads as clay thrown
    // off the edge of a moving blade.
    float sliceOut = 0.45f;
};

// M4.7 floating hands. The rig has no arms — the mitts are detached, and
// the ONLY thing holding them to the fighter is `reach`, a max distance from
// the blob body's centre of mass. Push the sword past that and the hands
// stop following, which reads as the grip slipping rather than the arms
// stretching (there are no arms to stretch).
struct HandParams {
    bool ik = true;          // hands follow the sword grips
    float reach = 0.f;       // max m from body COM; 0 = auto from the rest rig
    float reachScale = 1.5f; // auto reach = rest COM->wrist distance * this
    float palmFrac = 0.6f;   // where along wrist->fingertip the grip sits
    bool orient = true;      // wrap the mitts around the handle
    // Which mitt axis threads the blade (0=x, 1=y, 2=z) and how far the mitt
    // is spun about it. Authoring-dependent, so these are dialled by eye in
    // the running app (ctl: hands.gripAxis / hands.gripRoll) rather than
    // derived — reasoning from the grab morph's delta axes got it wrong.
    int gripAxis = 2;
    float gripRoll = -1.5f;  // radians about the handle: where fingers point
    // Lateral offset per hand, in sword radii. ZERO now, and the reason it
    // used to be 2.5 is worth keeping: under the old bone rig the mitts sat on
    // OPPOSITE FACES of the blade, pushed half a blade-width to each side,
    // because nothing threaded the handle. The brush rig puts the blade
    // THROUGH the gap between the fingers (gripAxis), so both hands belong on
    // the blade's own axis and any offset just slides them off it.
    float gripSpread = 0.f;
    // A `gripCurl` sat here, rotating the thumb/finger subtrees about the
    // handle so the digits wrapped onto the blade. It needed an armature to
    // rotate, and the grab MORPH replaced the whole idea — it was a panel
    // slider and a ctl name driving nothing at all.
};

// M-FIST: where the mitts sit when they are NOT on a hilt, and how they move.
//
// The sword case needs none of this. Holding the sword, the hands are placed
// BY the blade and they inherit the hilt's breathing for free
// (GameState::swordOffset), which is why the bob has lived in the sword until
// now. Every other pose has to place them itself AND bob them itself, or an
// unarmed fighter's hands are the one part of it that is dead still.
//
// Placement is in CHARACTER space — +x the authored side (the right mitt is
// the mirror), +y up, +z the way the fighter faces — and names where the PALM
// lands, so the rotations below spin the mitt in place instead of swinging it
// around the body. Every value here is a judgement call made by looking, so
// they live in ctl rather than the panel:
//   tools/ctl.sh "set handpose.fistPos 0.15 0.55 0.22" "set handpose.fistYaw -0.9"
struct HandPoseParams {
    // idle: hanging loose beside the body, fingers angled down. The idle shape
    // key supplies the hand's own droop; this supplies the arm that is not
    // there, which is where "lower and closer to the body" actually comes
    // from — the authored mitt sits way out at x 0.40..0.66.
    //
    // THE BODY IS x +-0.21, z +-0.21, y 0..0.69, and the palm is the mitt's
    // CENTRE, so a placement much inside those bounds buries the hand in the
    // torso. These sit the mitt just proud of the flank with a little overlap,
    // which is what makes a detached blob read as attached rather than as
    // floating nearby.
    float idlePos[3] = {0.28f, 0.29f, 0.04f};
    float idlePitch = 1.0472f; // 60 deg, fingers rotated DOWN about forward
    float idleYaw = 0.20f;     // ... and swung slightly back, as an arm hangs

    // guard: closed fists up in front of the face, where a boxer holds them.
    // The head tops out at y 0.69, so 0.545 is chin height; z clears the
    // chest's +0.21 front face so the fists are genuinely in front of it.
    float fistPos[3] = {0.135f, 0.545f, 0.275f};
    float fistPitch = 0.25f;
    float fistYaw = -0.95f; // knuckles swung round to face forward

    // ---- the bob ----
    // The SAME motion the held sword breathes with (GameState::kBobAmp /
    // kBobTilt, same rates, same 12 Hz pose clock), because "the hands bob"
    // has to mean one motion whatever they are holding — otherwise putting the
    // sword down changes how the fighter idles.
    float bobAmp = 0.012f;    // m of lift at rest
    float bobTilt = 0.05f;    // rad of roll at rest
    float bobRate = 3.0f;     // rad/s at rest
    float bobRateMove = 7.5f; // ... while walking
    float bobAmpMove = 1.6f;  // amplitude multiplier while walking

    // ---- the punch ----
    // A jab from the guard along the fighter's forward. `reach` is metres past
    // the guard position, but the RESULT is clamped by hands.reach the same
    // way the sword's grips are — the mitts are held to the body by a reach
    // ball and nothing else (there are no arms), so a punch cannot outrun it.
    float punchReach = 0.34f;
    float punchDur = 0.30f;   // s: out and back
    float fistRadius = 0.05f; // the carve a connecting punch leaves
    // A punch only cuts while the fist is genuinely TRAVELLING, for the same
    // reason a resting blade does not cut (updateBladeCut): a fist held
    // against a body would otherwise bore through it one edit per frame.
    float cutSpeed = 0.9f; // m/s the fist must be moving to carve
};

// M-PHYS: what it feels like to hit something, and to be hit.
//
// Hand-rolled and deliberately small. There is no rigid-body solver here and
// nothing wants one: a fighter is a point with a velocity on a plane, so all of
// this is one of three things — a force, a position constraint, or a rate
// multiplier on a strike's own clock. Three mechanics, one struct.
//
// EVERY INPUT IS CPU-SIDE, AND THAT IS THE LOAD-BEARING DECISION. Resistance
// scales with how much weapon is inside a body, and the tempting source for
// that number is the measured carved volume — it is right there in the ledger
// and it is the physically honest quantity. It must not be used. That volume is
// a GPU readback landing one or two frames late, whose arrival frame is pinned
// only under `--replay`'s syncMeasurements, and blocking for it is illegal on
// web (trap 9). Feeding it into the sim would make how a swing FEELS depend on
// GPU scheduling, and make a replay disagree with the run it recorded.
//
// The capsule hit test both cut paths already run is exact, free, and one frame
// earlier. `bite` below is metres of weapon inside the target's proxy, straight
// out of that test. See Renderer::StrikeContact.
struct PhysicsParams {
    bool enabled = true;

    // ---- bodies do not interpenetrate ----
    // Half the body brush's x/z extent, so two fighters touch 0.42 m apart.
    // Resolved as a position constraint in xz only: nothing here leaves the
    // floor, and a fighter is a circle from above.
    float bodyRadius = 0.21f;
    // Fraction of the overlap resolved per tick. 1 = separated immediately,
    // which is correct and stiff; lower reads as squashy contact.
    float pushOut = 1.0f;

    // ---- resistance ----
    // A strike's own clock runs at 1 / (1 + drag * bite), so a blade buried
    // 0.2 m at bladeDrag 6 advances at 0.45x. Clay does not STOP a blade, it
    // slows one — the swing always completes, it just has to fight, and the
    // fight is visible because the whole arc is driven off that clock.
    float bladeDrag = 6.0f;
    float fistDrag = 9.0f;
    // Floor on that multiplier. Two reasons, and the second is the sharp one:
    // a strike could otherwise stall forever inside a big enough body, AND
    // updateBladeCut stops cutting below a minimum sweep — so a swing dragged
    // far enough would stop cutting, lose its contact, speed back up, cut
    // again, and buzz. At 0.25 the cut beat still sweeps ~0.023 m/frame,
    // comfortably over that 0.012 m gate.
    float minRate = 0.25f;

    // ---- knockback ----
    // A FORCE, not an impulse. Contact persists across frames while a weapon
    // ploughs through, so this integrates over the strike; firing an impulse
    // per frame instead would scale knockback with the frame rate.
    //
    // CUT TO A NUDGE. It was 26 m/s^2 / 2.2 m/s, i.e. the hit was ENTIRELY a
    // displacement — the whole read was a rigid body sliding away, and clay
    // does not do that. What deforms now is the CLAY ITSELF, at the point of
    // impact (the R20 dent, edit.wgsl mode 3), and the shove is what stops a
    // struck body reading as bolted to the floor: enough to rock the stance,
    // not enough to move the fight. `set phys.knockForce 0` gives the pure bag;
    // 26 / 2.2 restores the old slide.
    float knockForce = 7.0f;  // m/s^2 per metre of bite
    float knockMax = 0.8f;    // m/s ceiling
    float knockDamp = 10.0f;  // m/s^2 linear bleed-off; 0.8 m/s stops in 0.08 s
    float recoil = 0.35f;     // the attacker takes this fraction, reversed
    // Hitstun, and IT DOES NOT TAKE THE CONTROLS AWAY. It used to zero the
    // victim's steering outright, and that read as the game freezing rather than
    // as a hit: contact persists for every frame a weapon ploughs through, so
    // the timer was re-armed the whole way, and an opponent jabbing every
    // punchCooldown could hold you there indefinitely.
    //
    // The shove is what sells the weight anyway — it lives in Body::knock, which
    // steering cannot cancel because it is integrated separately and decays on
    // its own schedule. So hitstun only has to stop the victim from walking
    // cleanly out of a shove it is still riding: `staggerControl` is the
    // fraction of steering authority it keeps, and it is never zero. At 0.45 the
    // hero pushes back at ~0.5 m/s — enough to steer, not enough to walk the
    // hit off before it has landed.
    float stagger = 0.20f;        // s of reduced steering on a full-strength hit
    float staggerBite = 0.05f;    // m of bite that earns all of it
    float staggerControl = 0.45f; // authority kept while staggered; never 0
};

// R20/R21: what a landing strike does to the CLAY, as opposed to what it does
// to the body's motion.
//
// It sits in LookParams rather than next to PhysicsParams above, and the line
// between them is ownership: everything in PhysicsParams is read by the 60 Hz
// sim in main.cpp (forces, constraints, a rate multiplier), and every field
// here is read by the renderer, which is what resolves where a weapon is and
// what shape it cuts. Pushing these through the sim would mean pushing a copy
// into the renderer every frame to get them back where they are used.
struct ImpactParams {
    // ---- the dent ----
    // A fist mostly DISPLACES. edit.wgsl mode 3 is volume-conserving by
    // construction, so most of a punch costs the ledger nothing at all — no
    // gob, no debt, no dribble — and what is left is a much smaller rupture
    // that does eject a chunk.
    //
    // The split matters for more than looks: damage in M-DEATH is the carved
    // ledger and nothing else, so a fist that ONLY dented could never kill.
    // punchCarve is the dial between "reads as a thud" and "reads as a wound",
    // and it moves the punches-to-a-kill count by roughly its inverse square:
    //   1.0 -> ~20 punches (the old, pellety behaviour)
    //   0.7 -> ~40         (default: a dent you can see, a fight you can win)
    //   0.4 -> ~125
    //
    // dentDepth is metres of inward push at the core. Two ceilings sit above
    // it, neither of which corrupts anything if you cross them: the allocated
    // narrow band (a dent never allocates, so one deeper than the shell clips),
    // and edit.wgsl's DENT_MAX, which caps the shift at ~19 mm to keep the
    // field monotonic along the normal.
    //
    // dentLength is the region the dent occupies along its own axis, and it is
    // 1.2 rather than something tighter because a SHORT region clips the rim of
    // the profile off a tilted contact patch — and the rim is the half that
    // puts the displaced clay back. Measured residual with these values: <=1.1%
    // of the displaced volume across +-30 deg of tilt and +-20 mm of offset,
    // ~3% against a body of this curvature, positive in every case (see
    // edit.wgsl dentShift for what "positive" buys).
    float dentDepth = 0.010f;  // 0 disables the dent entirely
    float dentRadius = 1.7f;   // x the fist radius, across the impact
    float dentLength = 1.2f;   // x the dent radius, along the punch
    float punchCarve = 0.70f;  // x the fist radius, for the rupture that ejects

    // ---- the marks ----
    // Albedo-only edits (mode 4): no allocation, no JFA, no redistance, no
    // ledger. They cost a fraction of a carve, which is what makes it
    // affordable to stamp one on every landing hit.
    //
    // One colour, two strengths, on purpose: a punch's bruise and a blade's
    // smear are the same damage stain at different opacities, and splitting the
    // colour would mean tuning the same judgement call twice. Feeding the
    // smear the wound's OWN colour instead is a one-line change
    // (BrickEdit::srcColor) — it just reads as nothing on a uniformly coloured
    // body, which is what this asset is.
    float bruise = 0.5f;       // 0..1 opacity at the impact core; 0 = off
    float bruiseRadius = 1.5f; // x the weapon radius
    // Linear RGB, dark and slightly warm against the body's cyan.
    float bruiseColor[3] = {0.030f, 0.012f, 0.020f};
    float smear = 0.35f;       // 0..1 opacity of a blade's contamination trail
    float smearRadius = 1.6f;  // x the cut radius: the slot, plus its lips
};

// M-FIST: the opponent's behaviour. Deterministic by the house rules — fixed
// 60 Hz tick, seeded RNG, no wall clock — so `--replay` reproduces a brawl
// exactly, and it therefore runs in the headless paths too rather than being
// a windowed-only toy (the sword bob was headless-blind for exactly one
// release and it made every screenshot disagree with the game).
//
// A journal that wants to place opponents by hand turns it off first:
//   tools/ctl.sh "set ai.enabled 0" "set p1.pos -0.95 0 0.35"
struct AiParams {
    bool enabled = true;
    // PASSIVE UNTIL PROVOKED. Proximity alone used to be enough to lock on, so
    // an opponent walked out of the dark and started punching before the player
    // had touched a key — nobody ever saw it wander, and the first fight was
    // never the player's idea. Now it minds its own business until something
    // HITS it; after that, distance decides when it engages exactly as before.
    // The grudge lives in OpponentAi, which a respawn resets, so a body that
    // comes back comes back calm.
    //
    // Journals that want it swinging from tick zero: `set ai.retaliatory 0`
    // (scenarios/carve-duel.journal does, to keep exercising the punch path).
    bool retaliatory = true;
    float wanderSpeed = 0.50f; // m/s while nobody is worth fighting
    // SLOWER THAN THE HERO ON PURPOSE. GameState::kMaxSpeed is 1.1 m/s, so at
    // 0.92 the player can always break away by running — an opponent matching
    // the hero's top speed is glued to their back and there is no way out of a
    // fight, which reads as being chased by a magnet rather than fought.
    // `accel` and `turnRate` are cut for the same reason: it corners wider than
    // the player, so a hard turn buys distance.
    float chaseSpeed = 0.92f;
    // Hysteresis, deliberately wide: one radius would make an opponent hovering
    // at the boundary flip between wander and chase every few ticks, which
    // reads as indecision rather than as a fighter.
    float lockRange = 1.50f;   // lock on inside this
    float breakRange = 2.40f;  // ... and lose interest outside this
    // Stop closing HERE, not at contact. At 0.70 m between centres the target's
    // near face is ~0.49 m away and the extended fist reaches ~0.62 m, so a jab
    // lands ~0.13 m in: a dent, not a skewer. Closing further makes every punch
    // a tunnel through the middle of the body.
    float standoff = 0.70f;
    float strikeRange = 0.80f; // throw a punch inside this
    float punchCooldown = 0.75f;
    float wanderRadius = 2.20f; // keep wandering inside this of the arena centre
    float wanderHold = 1.60f;   // s before picking a new heading
    float turnRate = 4.0f;      // rad/s — the hero turns at 7.0
    float accel = 4.5f;         // m/s^2 — the hero accelerates at 7.0
};

// Whether the ROOT translation steps on the 12 Hz pose grid like the rest of
// the stop-motion, or slides at 60 Hz.
//
// DEFAULT OFF, and the reason is worth keeping: trap 4 covers the POSE, and
// extending it to translation does not work at this frame rate. Real
// stop-motion animates on 2s at 24 fps, so a held position spans 2 frames.
// We present at 60, so it spans 5 — and a ~9 cm translation jump (1.1 m/s
// over 1/12 s) is nowhere near as forgiving as a held pose or a boil reseed.
// It reads as jumpy whichever way the camera is handled: chase the sim
// position and the subject jumps inside the frame, chase the stepped position
// and the whole world jumps. There is no framing that hides it.
//
// The cost of leaving it off is that walking cannot reuse frames (the root is
// a traced input that changes every frame), so motion runs at the raw frame
// cost. That is a rendering problem to solve in the renderer, not by making
// the movement look worse.
//
// It was flipped ON once, for that reuse cost, and flipped back after playing
// it. DON'T FLIP IT AGAIN ON A REUSE MEASUREMENT — reuse % is the wrong
// scoreboard here, and it is convincing:
//
//   Frame reuse does not produce images, it only makes DUPLICATE frames
//   cheap. Sliding, every frame is unique and costs the full trace: ~17
//   unique images/s. Stepped, the trace happens 12x/s and ~48 duplicates
//   ride along: the fps counter reads ~60 while the eye gets 12. Stepping
//   spends motion fidelity (17 -> 12 unique images/s) to buy a bigger
//   number. The reuse rate went 0% -> 73% and the game looked worse.
//
// Reuse is still worth having for what it was built for — a still camera,
// where the duplicates are duplicates of a frame nothing was changing anyway.
// It is not a lever to make motion cheaper. Motion gets cheaper by making the
// TRACE cheaper.
struct MotionParams {
    bool stepRoot = false;

    // ---- the invisible wall ----
    // The arena used to end at a pebble-mosaic backdrop plane (z = -2.3). That
    // is gone: the floor now just runs out of key light and fades to the near
    // black `background()`, which is a better edge than any surface — no seam,
    // no corner, nothing to recognise as a set.
    //
    // But a scene with no wall has nothing stopping a fighter walking out into
    // the dark and becoming a rumour, so the boundary survives as a radius that
    // every fighter's position is clamped to. It sits WHERE THE LIGHT HAS
    // ALREADY GONE — the key is at (-1.1, 2.1, 1.6) with a quadratic falloff,
    // so by 2.6 m from the centre a body is lit at roughly a tenth of what it
    // gets at the origin — which is what makes it invisible rather than a
    // barrier you can see yourself bump into.
    //
    // 0 disables it. Set from ctl (`set motion.arenaRadius 3.5`) if you want a
    // bigger stage; the light will not follow you out there.
    float arenaRadius = 2.6f;
};

// M4.8 gaze. There are no eye bones any more — the eyes are marble beads
// riding the body affine — so this rotates each PUPIL about its own eyeball's
// centre to face the target. Latched on the 12 Hz pose grid like everything
// else that moves, or a standing fighter would re-trace every frame.
struct GazeParams {
    bool track = true;         // eyes follow the camera as it orbits
    float maxAngle = 1.5708f;  // clamp cone off the forward direction, radians
};

// M-PERF: the affine body's procedural motion.
//
// This IS the blob's shape animation — there is nothing else. The motion the
// game wants is a squish-and-spring (idle) and a squish, hop and forward lean
// (walk), which is one non-uniform scale plus a shear plus a translation:
// one matrix, nothing worth skinning per sample.
//
// It does not REPLACE skeletal clips, whatever the older note here said. The
// asset ships no clips and no armature (CLAUDE.md trap 7), so there is no A/B
// against a 13-piece path; `look.affineRig` off now draws the rest volume
// unposed, which answers a different question ("rig or volume?").
//
// DETERMINISM: the spring is integrated ONLY on 12 Hz pose steps, at a dt read
// off frame.poseTime, with a fixed substep count and no RNG and no wall clock.
// So --replay reproduces it exactly, and — the reason it is on the pose grid
// rather than the frame clock — a standing fighter's uniforms stop changing
// between pose steps, which is what lets frame reuse keep firing (trap 4).
//
// ---- M-SPRING: the bounce is a REAL HOPPER, not a bob on a metronome ----
//
// What was here before: a harmonic oscillator kicked once per `gaitHz`, and a
// hop that was `max(stretch, 0) * hop` — a lift scaled off the spring's shape
// rather than produced by it. Nothing about it was a body leaving the floor.
// It could not be, because the fighter had no vertical dynamics at all: the
// 60 Hz sim is xz-only, and this was the ONLY vertical channel there was. So
// walking read as a slide with a 2 cm bob riding on top.
//
// It is now the textbook vertical SLIP hopper (spring-loaded inverted
// pendulum) and it is ONE variable, `u`, in METRES — the contact coordinate:
//
//   u <  0   grounded, the body compressed by -u against the floor
//   u >= 0   airborne, the FEET at height u
//
// which makes the whole simulation three lines, with no landing or takeoff
// event to detect and therefore no way to miss one (Body::stepHop, main.cpp):
//
//   vu -= g*h;                                   // gravity, always
//   if (u < 0) vu += (-K*u - C*vu) * h;          // the floor, only in contact
//   u += vu*h;
//
// Everything the eye reads is a CONSEQUENCE of that, not a curve: the arc of a
// hop is ballistic because nothing else is acting; the squash on landing is
// deep exactly in proportion to how fast the body arrived; the body springs
// back up because the clay it compressed gave the energy back, minus what `C`
// took. Hop height, cadence and squash cannot be dialled independently, which
// is the point — they are the same event seen three ways.
//
// THE AUTHORED REST SHAPE IS THE *LOADED* ONE. A blob standing on the floor is
// already squashed by its own weight — `u* = -gravity/legK`, about 4.4 cm here
// — and the artist sculpted it standing, so that sag is where the squish reads
// zero. Two things fall out, both wanted. A fighter at rest looks exactly as
// authored (this change does not resculpt the idle silhouette). And a fighter
// in the AIR is off its floor, so the clay relaxes to its unloaded length and
// the body reads as STRETCHED by exactly the sag it was carrying — +6.3%, the
// squash-and-stretch of the animation books, arrived at from the weight rather
// than keyed. It is also why there is no `hop` scale any more: the lift is in
// metres because `u` is in metres.
//
// WALKING PUMPS THE SPRING, it does not schedule a hop. `hopThrust` is one
// velocity impulse at MAXIMUM COMPRESSION — the bottom of the stance, which is
// where a push-off goes and where pumping a swing goes. So the gait is a limit
// cycle: the hop grows until damping eats exactly what the push-off adds, and
// then it holds there. Consequences worth knowing before you turn the knob:
//
//   - Below a threshold thrust the body never leaves the floor at all. It
//     hunkers and pulses in place. That is not a bug and not a dead zone to
//     tune out; it is what a push too weak to lift you does.
//   - MORE thrust makes the cadence SLOWER, not faster (a bigger hop is a
//     longer flight): 1.0 measures 2.33 Hz and 1.1 measures 2.00 Hz. It also
//     makes the fighter FASTER, because more of the cycle is spent airborne
//     and airborne is the only time it travels — 0.8/1.0/1.3 give 0.83/1.10/
//     1.35 m/s. One knob, three coupled effects, which is the point.
//   - Stopping needs no code. With no push-off damping simply wins: ONE last
//     landing, then the squish rings down from 9.6% to 2.3% within two seconds
//     and 0.3% by four. Damping this light (zeta 0.067) is what makes the
//     bounce lively, and the long quiet tail is the price — it is also the
//     right read, clay absorbing the last of it rather than snapping to a stop.
//
// `legK` has a CEILING that has nothing to do with taste: this is displayed on
// a 12 Hz grid, so a spring ringing above ~3 Hz aliases into hash. 225 puts it
// at 2.39 Hz with the stance at 0.21 s, close enough to the ceiling to be
// worth re-checking if you raise it.
//
// ---- TRAVEL IS BALLISTIC: you only move while you are in the air ----
//
// The hop stopped being a decoration on a walk and became the walk. A planted
// foot does not slide, so while `hopU < 0` the fighter's own locomotion moves
// it NOWHERE; the push-off that throws it upward also throws it forward along
// whatever heading it is steering, and it coasts on that until it lands, where
// the clay absorbs the horizontal velocity along with the vertical one. Leap,
// land, leap. `Body::stepHop` in main.cpp is the whole of it.
//
// This is why the hopper moved OUT of the renderer and into the 60 Hz sim.
// While the bounce was cosmetic, the pose grid was the right home for it. Once
// distance-per-hop is locomotion, an airborne flag that only updates 12 times
// a second quantises every flight to +-83 ms of a ~220 ms arc, and hop length
// visibly lurches. The sim owns it now, substepped four ways per tick so the
// numbers above still hold; the RENDERER latches it back onto the pose grid
// for display, which is what keeps trap 4 and the idle frame reuse intact (a
// breath rings this spring for ~3 s, so a 60 Hz DISPLAY would leave a standing
// fighter's uniforms moving every frame). Height therefore steps at 12 Hz
// while xz slides at 60, which is not the jumpiness MotionParams argues
// against — the camera chases a subject horizontally, nothing chases it
// vertically.
//
// `hopLaunch` exists to keep this from silently rebalancing the fight. A
// fighter is airborne 48% of the time at the defaults, so travelling only in
// flight at the old 1.1 m/s would have halved its real speed and changed every
// distance the AI is tuned around (standoff, strikeRange, breakRange) without
// touching one of them. 2.1 is 1/0.48: the flight is that much faster than the
// demand, so the AVERAGE lands back on 1.10 m/s exactly, and the AI's own
// numbers arrive intact too — a 0.92 chase measures 0.92, a 0.45 wander 0.45.
// `kMaxSpeed` still means what it says.
//
// THE LAUNCH IS SPENT IN THE AIR, NOT EARNED THERE, and the difference is a
// bug I shipped once. The push-off fires at maximum COMPRESSION, which is
// mid-stance with the foot still planted, so translating on `air` the moment
// it is set slides the body through the back half of every stance: 26% of the
// cycle, and travel measured 1.59 m/s against the 1.10 it was calibrated for.
// Body::integrate gates the translation on `airborne()` for that reason.
//
// Tune `hopThrust` and the calibration stops being exact — a bigger push-off
// is a longer flight is more of the cycle spent travelling, so the fighter
// genuinely gets faster (0.8/1.0/1.3 give 0.83/1.10/1.35 m/s). That coupling
// is real and it is the point; only the shipped defaults are calibrated.
//
// Two things deliberately NOT gated on contact. KNOCKBACK still shoves a
// planted fighter — it is someone else's force, not your legs, and a punch
// that could not move a standing body would be a worse bug than a slide. And
// STEERING keeps updating while planted, so `moving`, the facing turn and the
// lean all behave exactly as before; what stance withholds is only the
// translation. The cost is that a change of direction waits for the next
// push-off, up to a stance (~0.21 s) away. That is what committing to a hop
// means, and `airControl` is the escape hatch if it plays badly.
//
// Two A/Bs, and they answer different questions. `hopThrust 0` is the one to
// reach for: the body still has weight, still sags onto it, still absorbs a
// landing — it just never pushes off, so it slides, which is the closest thing
// to what this replaced. `gravity 0` removes the weight itself: no sag, no
// stance, nothing for the breath to compress against while walking.
//
// Defaults measured by simulating this integrator at the 60 Hz tick and 4
// substeps it actually runs at (a 16.7 ms step without them loses ~7% of the
// hop; 4 substeps tracks a 200-substep reference to 0.3 cm). The walk lands at
//
//   5.9 cm hop, -10.1% squash, +6.3% stretch, 2.33 Hz, 48% of the time
//   airborne, 0.47 m of ground per hop, 1.10 m/s average
//
// — a half-metre bound twice a second on a 0.69 m body, at exactly the walking
// speed `kMaxSpeed` has always meant, and at a cadence within noise of the old
// `gaitHz` 2.2. The rhythm survived the mechanism changing underneath it.
//
// The idle breath sits at -6.9% with the feet DOWN (peak lift -0.43 cm, i.e.
// still compressed) and travels exactly zero; past `idleKick` ~1.0 a breath
// starts lifting them, which reads as fidgeting rather than breathing.
struct RigParams {
    // ---- M-SPRING: the leg spring ----
    float gravity = 9.81f;    // m/s^2. 0 = no weight, no sag, no bounce
    float legK = 225.f;       // stiffness, 1/s^2 per metre; omega 15.0 (2.39 Hz)
    float legDamp = 2.0f;     // contact damping, 1/s; zeta ~0.067, ~3 hops to rest
    float hopThrust = 1.f;    // m/s of push-off at max compression, walking only
    float idleHz = 0.5f;      // breaths per second at rest
    float idleKick = 0.8f;    // m/s per breath; past ~1.0 the feet leave the floor
    float widen = 0.5f;       // sideways bulge per unit of squish
    // ---- the hop CARRIES you ----
    // The same push-off that throws the body up throws it FORWARD, along
    // whatever direction the fighter is steering at that instant, and that
    // launch is the only thing that ever moves it: planted feet do not slide.
    // See the "TRAVEL IS BALLISTIC" note above for what this multiplies and
    // why 2.1 is the number that keeps the average at the old walking speed.
    float hopLaunch = 2.1f;
    // ---- the arc lean: pitch forward going up, back coming down ----
    //
    // Radians of lean per m/s of VERTICAL velocity, and the two halves are
    // separate because the pose is not symmetric: a body throws itself forward
    // off the push-off and only tips back a little as it drops. At the shipped
    // hop (takeoff ~1.08 m/s) that is about +13.6 deg at the launch easing to
    // -6.2 deg at the landing.
    //
    // There is no lerp to write. `vu` passes smoothly through zero at the apex,
    // so reading the lean off it IS the ease from forward to back, timed by the
    // arc itself and free of any curve to keep in sync with the hop. It is
    // added to the travel lean rather than replacing it, and scaled by speed so
    // that a STANDING fighter does not rock: the idle breath swings `vu` by
    // +-0.8 m/s, which ungated would tip a breathing body about 10 degrees.
    float hopLeanFwd = 0.22f;
    float hopLeanBack = 0.10f;
    // Steering authority in mid-air, per second, as a fraction of the gap
    // between the launch you got and the one you now want. 0 is the honest
    // answer and the default — a body in the air is a projectile, and the
    // moment to choose a direction is the push-off. It exists because "no air
    // control" is a feel decision, not a physics one: if committing to a
    // heading for a whole hop plays badly, this is the dial, and anything
    // above ~4 effectively restores steering mid-flight.
    float airControl = 0.f;
};

// M-DEATH: a fighter that has lost half its clay stops being a fighter.
//
// The threshold is measured against the CARVED LEDGER, not against a hit-point
// pool, and that is the whole point: the number that kills you is the same
// number the conservation system has been tracking since M4.6, so damage is
// literally the clay that left your body. There is no second health model to
// keep in sync with the one the renderer already maintains.
//
// The collapse spends that remaining clay through the EXISTING dribble
// spawner rather than a bespoke one: death adds the leftover mass to
// `carved` and `debt` in the same breath, so `carved == deposited + inFlight
// + debt` holds across it and --carve-test's gate stays honest. Respawn then
// re-imports the body, which does put new clay in the world — the ledger's
// invariant is that CARVED clay is conserved, not that total scene mass is.
struct DeathParams {
    bool enabled = true;
    // Fraction of a fighter's own volume that has to be gone. Measured against
    // the mesh volume of its brushes (body + two mitts), printed per brush in
    // the `asset:` banner.
    float threshold = 0.5f;
    float respawn = 2.6f;     // s face-down before coming back

    // ---- how the leftover mass leaves ----
    // A collapsing fighter sheds ~46 litres at the shipping threshold, and the
    // dribble spawner cannot carry that: it moves at most twelve 35 ml gobs per
    // pose step, so a body would leak for FIFTEEN SECONDS as a thin stream.
    // Same reasoning as the sword's slice gob one scale up — the size of the
    // thing that comes off has to match the size of what was removed.
    //
    // So the mass splits two ways: a burst of chunky gobs that fly, and the
    // rest deposited straight into the ground field as a heap where the body
    // stood. Anything that will not fit (the gob array is capped at 12) falls
    // back to being debt, which the dribble drains — slow, but never lost.
    int burstGobs = 8;
    float burstFrac = 0.20f;  // of the remaining mass, thrown as gobs
    float burstSpeed = 1.6f;  // m/s outward
    float burstLift = 1.8f;   // m/s up
    // Ground splats clamp their radius at 0.28 m, so volume past ~6 l in one
    // splat stops widening and starts stacking a spike taller than the fighter
    // was. The heap is therefore spread over as many points as it takes to keep
    // each one under this, arranged on a golden-angle spiral so they do not
    // line up into a visible lattice.
    float splatMax = 6.0e-3f; // m^3 per splat
    float splatSpread = 0.34f; // m, radius of the heap
    // The eyes outlive the body: on collapse the four beads stop riding the
    // corpse and become loose marbles that fall, bounce and roll. They are the
    // only rigid props in the scene, so this is the only place the game has a
    // reason to simulate one.
    float eyeSpeed = 1.9f;    // m/s outward kick
    float eyeLift = 1.4f;     // m/s up
    float eyeBounce = 0.42f;  // restitution on the floor
    float eyeRoll = 0.55f;    // rolling friction, m/s^2 per m/s
    // Where a respawned fighter reappears: this far from the arena centre, and
    // for an opponent, on the far side of it from the hero. Coming back inside
    // arm's reach would be a free hit on whoever killed you.
    float spawnRadius = 2.15f;
};

// Look-dev parameters, exposed in the ImGui panel and packed into the
// uniform buffer by the renderer. Defaults target the Trap Door "day
// dungeon" rig: warm amber key pooling to black, faint cool rim.
struct LookParams {
    // close-in key = tight pool of light; distance ratio subject:edge is what
    // makes the darkness, not raw falloff
    float keyPos[3] = {-1.1f, 2.1f, 1.6f};
    float keyIntensity = 19.0f;
    float keyColor[3] = {1.0f, 0.70f, 0.40f};
    float keyFalloff = 1.4f; // attenuation = intensity / (1 + falloff * d^2)

    // Rim light: a cool Fresnel edge from the UNLIT side. `rim` peaks where
    // the surface turns away from the camera (pow(1 + dot(n, rd), 2.5) in
    // trace.wgsl's shade()), gated by how much that point faces rimDir and
    // multiplied by AO, so it paints a blue-ish outline on the character's
    // silhouette and separates it from the dark background. The set gets a
    // quarter of it — rim on rough stone reads as wetness.
    //
    // OFF (0) by default. It is the classic third light of a studio rig and it
    // was doing what that always does: making the clay look photographed
    // rather than lit by the one warm key the scene actually has. rimDir and
    // rimColor still exist for anyone turning it back on from ctl.
    float rimDir[3] = {-0.5f, 0.35f, -1.0f};
    float rimIntensity = 0.f;
    float rimColor[3] = {0.45f, 0.62f, 0.85f};

    float ambient[3] = {0.016f, 0.012f, 0.009f};
    float aoStrength = 1.15f;

    float detailAmount = 0.8f; // thumbprint/tool-mark normal perturbation
    float boilAmount = 0.5f;   // per-pose-step detail reseed (stop-motion boil)
    float shadowSoft = 10.0f;  // soft shadow sharpness k
    float sheenAmount = 0.008f; // dry clay barely sheens; plasticine was 0.04

    float grainAmount = 0.07f;
    float vignetteInner = 0.72f;
    float vignetteOuter = 1.60f;
    // gate weave (whole-frame drift): OFF by default — at reduced internal
    // resolution it amplifies into camera shake (user). Slider remains for
    // taste; unrelated to boil, which lives in surface detail.
    float weaveAmount = 0.0f;

    float exposure = 1.1f;
    float bloomAmount = 0.12f;
    float bloomThreshold = 0.75f;
    float debugMode = 0.f; // 1 = normals visualization (set via CLAYFRAY_DEBUG_NORMALS)
    // internal render scale (windowed): trace at a fraction of window size,
    // blit upscales. Chunky low-res + grain reads very stop-motion, and
    // traced pixels are the whole frame cost.
    float resScale = 0.5f; // user-approved default: chunky + fast
    // FIXED trace resolution, overriding resScale when both are > 0. Frame
    // cost is per TRACED PIXEL, so a resScale-derived size makes two machines
    // incomparable the moment their windows or backing scales differ — and
    // "same window size" is not the same thing as "same traced pixels" on a
    // display that reports backing pixels. Pin this and the number means the
    // same thing everywhere. `--res WxH` sets it; the startup [res] line
    // always prints what is actually being traced, so a mismatch is visible
    // instead of inferred.
    int traceW = 0, traceH = 0; // 0,0 = derive from the window via resScale

    // M4-P0 animation: play the asset's first clip, looped, sampled on the
    // 12 Hz pose grid. Off = rest pose. (Drives marbles + shadow proxy; the
    // voxel body articulates in M4-P1.)
    bool animPlay = true;
    float animSpeed = 1.0f;

    // M4.6 conservation: carved clay sploots onto the arena instead of
    // vanishing. Off = the pre-conservation vanish behavior.
    bool conserveClay = true;

    // M-PERF: collapse articulation to an affine body + two rigid mitts.
    // Off = the M4-P1 13-piece inverse-LBS warp. Both live in one binary so
    // the pair can be benchmarked without a shader edit (a shader edit forces
    // a cold pipeline compile on the next launch, which has faked wins here
    // twice — see the benchmarking notes in CLAUDE.md).
    // CLAYFRAY_NO_AFFINE=1 forces it off regardless.
    bool affineRig = true;

    // M4.7 sword prop + floating-hand IK (debug-controlled hilt transform).
    SwordParams sword;
    HandParams hands;
    HandPoseParams handPose;
    ImpactParams impact;
    GazeParams gaze;
    MotionParams motion;
    RigParams rig;
    // M-DEATH lives here rather than beside AiParams because the renderer is
    // what owns the carved ledger and the volumes, and LookParams is the only
    // channel render() gets.
    DeathParams death;
};

// Conservation ledger readout for the panel (all volumes in m^3).
struct SplootStats {
    float carved = 0.f;    // total measured off the body
    float deposited = 0.f; // total landed (splats + re-sticks)
    float debt = 0.f;      // measured but not yet spawned as gobs
    float inFlight = 0.f;  // riding gobs right now
    int gobs = 0;
};

// Sculpt-mode UI state (windowed only).
struct BrushState {
    int mode = 0; // 0 orbit, 1 carve, 2 add
    float radius = 0.045f;
    float color[3] = {0.72f, 0.45f, 0.40f}; // terracotta — contamination preview
};

struct FrameInfo {
    float time = 0.f;      // seconds, smooth
    float poseTime = 0.f;  // quantized to 12 Hz (stop-motion pose steps)
    float grainFrame = 0.f; // integer counter at 25 Hz (film frames)
    int aaSamples = 1;     // rays per pixel axis (1 = 1 ray, 2 = 4 rays)
};
