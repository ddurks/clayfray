#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Rigid glass-bead prop (eyes, teeth, buttons): authored in Blender as
// objects named marble_*, never voxelized into the clay. `bone` (nearest
// joint at import) lets the prop ride the skeleton rigidly.
struct MarbleProp {
    float pos[3];
    float radius;
    float color[3];
    int bone = -1;
};

struct AssetBone {
    std::string name;
    int parent = -1;
    float invBind[16];   // inverse bind matrix, column-major
    float restLocal[16]; // local rest transform, column-major
    // world transform of the joint's non-joint ancestor chain (e.g. the
    // Armature node); identity for joints parented to another joint
    float pre[16];
};

// One keyframed property of one joint. CUBICSPLINE tangents are dropped at
// parse time (mid values only) — clay poses at 12 Hz don't earn Hermite.
struct AnimTrack {
    int joint = 0;
    int path = 0;   // 0 translation, 1 rotation (quat xyzw), 2 scale
    int interp = 1; // 0 step, 1 linear
    std::vector<float> times;
    std::vector<float> values; // 3 or 4 floats per key
};

struct AnimClip {
    std::string name;
    float duration = 0.f;
    std::vector<AnimTrack> tracks;
};

// M-RIG: one "brush" — a contiguous run of the merged triangle soup that is
// voxelized into its own DISJOINT region of the shared rest volume, and is
// then addressed at runtime by a single rigid/affine transform. This is
// Claybook's brush model (docs/claybook/ANIMATION-AND-BUDGET.md) with the
// brushes packed into the one volume we already have rather than into a
// texture each: no new buffer, no new binding, so CLAUDE.md trap 8 (Metal's
// 10-storage-buffer stage cap, which `trace` already sits at) is satisfied by
// construction.
//
// `lo`/`hi` are the TIGHT rest-space bounds of this brush's geometry. Because
// the brushes are disjoint and well separated, a box test against them is an
// exact separation of one brush from the others — see the clipping note in
// shaders/brick_read.wgsl.
struct MeshPart {
    std::string name;
    uint32_t idxBegin = 0, idxCount = 0; // into CharacterAsset::indices
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    // Enclosed volume, m^3, by the divergence theorem over the closed shell.
    // This is how much clay the brush IS, which is the denominator M-DEATH's
    // "half of it is gone" test needs. Measured off the MESH rather than the
    // voxels on purpose: the voxel occupancy is only obtainable through a
    // blocking GPU readback (BrickSystem::debugStats, dev-tools only), and the
    // two agree closely because the voxelizer is a watertight parity fill.
    // Every brush here is a closed 2-manifold once split vertices are welded,
    // which is the precondition for this to mean anything at all.
    float volume = 0.f;
};

// ---- the mitt's DISCRETE poses ----
// Each is its own brush, voxelized into its own region of the one rest volume
// and selected WHOLE at runtime — never blended. Blending two of them would
// mean sampling two SDF regions per march step, which is exactly the
// per-sample work the brush rig exists to delete, and a hand that eases
// between grips does not read as stop-motion: it should pop (CLAUDE.md, "the
// grip is a discrete brush swap").
//
// `rest` is the shape as authored and doubles as the CANONICAL frame every
// placement is measured in (its offset is zero by construction). The other
// three are shape keys on the same mesh, imported at full weight.
enum HandBrush {
    kHandRest = 0, // as authored — open mitt, and the canonical frame
    kHandGrab = 1, // "grab" shape key: wrapped around a hilt
    kHandIdle = 2, // "idle" shape key: hanging loose, nothing held
    kHandFist = 3, // "fist" shape key: closed, for guard and punch
    kHandBrushCount = 4,
};

// Rest-space translation applied to each hand brush when it is voxelized,
// which is what makes five brushes fit in one volume.
//
// The volume box is x,z in [-0.7062, +0.7102], y in [-0.1582, +1.2582]
// (BrickSystem::kOrigin + kGrid*kSpan). The authored geometry occupies only
// x in [-0.2090, +0.6644] — body +-0.2090, hand +0.3979..+0.6644 — and only
// z in [-0.2090, +0.2090] at the hands' own height, so most of the box is
// empty and free to hold the other poses.
//
// THE NUMBER THAT HAS TO BE CLEARED is the narrow-band half-width,
// kBand * kVoxel = 12 * 0.004047 = 0.0486 m: within a band of the surface the
// field is real data, so two brushes closer than that would blend into each
// other's bands and the AABB clip that separates the pieces would stop being
// exact. Every offset below is the MIDPOINT of its feasible interval, so the
// two tight clearances on that axis come out equal.
//
//   grab  -x, the half the body never uses. x [-0.5856, -0.3294]:
//           0.1206 m clear of the -x wall, 0.1204 m clear of the body.
//   idle  +z, in front. The four shapes' padded union is 0.2347 m deep, the
//           free strip from the body's +z face to the wall is 0.4892 m, so
//           0.1272 m of slack lands on each side. x is re-centred on 0 at the
//           same time, which also buys back the +x wall clearance the rest
//           hand sits tight against.
//   fist  -z, the mirror of that: strip 0.4852 m, 0.1253 m each side.
//
// The two z poses are separated from the body in Z ALONE, which is a change
// of kind from the grab hand's -x placement and is fine: the clip is a 3D box
// test, so one axis of separation is a separation.
//
// tools/verify_brush_layout.py recomputes every one of these numbers from
// assets/fighter.glb and src/brick.h; run it if the artist moves anything or
// adds a shape key.
inline constexpr float kHandBrushOffset[kHandBrushCount][3] = {
    {0.f, 0.f, 0.f},
    {-0.9835f, 0.f, 0.f},
    {-0.5346f, 0.f, 0.4338f},
    {-0.5346f, 0.f, -0.4954f},
};

// A Blender-authored character: triangle mesh (engine space, y-up, meters),
// per-vertex color, marble props, and (if the asset still has an armature)
// skin weights + skeleton + clips.
//
// The fighter asset has NO armature — no skins, no animations, three meshes
// authored in place. Every skeletal field below is therefore EMPTY for it, and
// the loader must degrade rather than reject: `bones`, `clips`, `joints` and
// `weights` are kept only so a rigged asset would still import.
struct CharacterAsset {
    std::vector<float> positions;  // xyz per vertex
    std::vector<uint32_t> indices;
    std::vector<float> colors;    // rgb per vertex, linear
    std::vector<uint16_t> joints; // 4 per vertex; all 0 without a skin
    std::vector<float> weights;   // 4 per vertex; identity without a skin
    std::vector<AssetBone> bones;
    std::vector<MarbleProp> marbles;
    std::vector<AnimClip> clips;

    // The brushes, in voxelization order, plus named lookups into `parts`.
    // -1 means the asset did not provide that piece.
    std::vector<MeshPart> parts;
    int partBody = -1;
    // One per HandBrush. [kHandRest] is the mesh as authored; the rest are the
    // same primitive re-imported with one shape key at full weight, each
    // translated by kHandBrushOffset. A shape key the asset does not have
    // leaves its entry at -1 and handPart() falls back to the rest shape, so
    // an older .glb still rigs — it just has fewer distinct grips.
    int partHand[kHandBrushCount] = {-1, -1, -1, -1};

    bool load(const std::string& path);
    uint32_t vertexCount() const { return (uint32_t)positions.size() / 3; }
    uint32_t triangleCount() const { return (uint32_t)indices.size() / 3; }
    // The brush actually imported for a pose: the requested one, or the rest
    // shape when the asset shipped no such shape key.
    int handPart(int pose) const {
        if (pose < 0 || pose >= kHandBrushCount || partHand[pose] < 0)
            return partHand[kHandRest];
        return partHand[pose];
    }
    // True when the brush rig can drive this asset. Body + rest + grab is the
    // floor, because those are the two poses the sword needs; idle and fist
    // degrade to rest rather than refusing the asset.
    bool hasBrushRig() const {
        return partBody >= 0 && partHand[kHandRest] >= 0 && partHand[kHandGrab] >= 0;
    }
};
