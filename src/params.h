#pragma once

// M5 locomotion: where the fighter stands and which way it is going. The
// whole skeleton is premultiplied by this, so pose clips stay authored in
// character space and the sword/hands follow for free.
struct FighterPose {
    float pos[3] = {0.f, 0.f, 0.f}; // world, feet on the arena floor
    float yaw = 0.f;                // facing, radians about +Y
    float lean = 0.f;               // radians tilted INTO the direction of travel
    bool moving = false;            // picks the bounce clip over idle
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
    float knockForce = 26.0f; // m/s^2 per metre of bite
    float knockMax = 2.2f;    // m/s ceiling
    float knockDamp = 10.0f;  // m/s^2 linear bleed-off; 2.2 m/s stops in 0.22 s
    float recoil = 0.35f;     // the attacker takes this fraction, reversed
    // Hitstun. Without it the target's own accel cancels a shove in ~0.3 s and
    // the hit reads as nothing happening; with it the fighter stops steering
    // and just slides, which is what sells the weight.
    float stagger = 0.20f;     // s of no steering on a full-strength hit
    float staggerBite = 0.05f; // m of bite that earns all of it
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
// Defaults tuned by simulating the integrator at the 12 Hz step it actually
// runs at (an impulse response at 60 Hz would be a different curve): they land
// the walk at about -19% squish / +4% stretch and the idle breath at about
// -9%, which is a claymation range rather than a jelly one.
struct RigParams {
    float squishK = 60.f;     // spring stiffness (rad/s)^2; period ~0.81 s
    float squishDamp = 4.5f;  // velocity damping; lower = more overshoot/bounce
    float squishKick = 2.4f;  // impulse per footfall
    float gaitHz = 2.2f;      // footfalls per second while moving
    float idleHz = 0.5f;      // breaths per second at rest
    float idleScale = 0.45f;  // idle impulse as a fraction of the walk's
    // Metres of lift per unit of STRETCH, walk only. Stretch peaks near +0.04,
    // so 0.45 is a ~2 cm hop on a 0.69 m body — a skip, not a leap.
    float hop = 0.45f;
    float widen = 0.5f;       // sideways bulge per unit of squish
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
