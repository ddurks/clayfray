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
};

// Rest-space translation applied to the GRAB hand when it is voxelized, which
// is what makes three brushes fit in one volume.
//
// The volume box is x,z in [-0.7062, +0.7102], y in [-0.1582, +1.2582]
// (BrickSystem::kOrigin + kGrid*kSpan). The authored geometry occupies only
// x in [-0.2090, +0.6644] — body +-0.2090, hand +0.3979..+0.6644 — so the
// entire NEGATIVE-X HALF of the volume is empty and free to hold a second
// hand pose. Shifting the grab hand by -0.9835 m puts it at
// x [-0.5856, -0.3294], which is:
//   0.1206 m clear of the volume's -x wall  (was 0.4408 m of slack to spend)
//   0.1205 m clear of the body's -x face    (body starts at -0.2090)
//   0.7273 m clear of the rest hand         (which starts at +0.3979)
// The offset is deliberately the MIDPOINT of the feasible interval, so the two
// tight clearances come out equal. Both are ~2.5x the narrow-band half-width
// (kBand * kVoxel = 12 * 0.004047 = 0.0486 m), which is the number that has to
// be cleared: within a band of the surface the field is real data, so two
// brushes closer than that would blend into each other's bands and the box
// clip would stop being exact. y and z are untouched — the grab hand spans
// y [0.2789, 0.4814] and z [-0.0651, +0.0739], both far inside the box.
//
// tools/verify_brush_layout.py recomputes every one of these numbers from
// assets/fighter.glb and src/brick.h; run it if the artist moves anything.
inline constexpr float kGrabBrushOffset[3] = {-0.9835f, 0.f, 0.f};

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
    int partHandRest = -1; // hand as authored          (brush B)
    int partHandGrab = -1; // hand + 1.0 * "grab" morph (brush C), translated

    bool load(const std::string& path);
    uint32_t vertexCount() const { return (uint32_t)positions.size() / 3; }
    uint32_t triangleCount() const { return (uint32_t)indices.size() / 3; }
    // True when the brush rig can drive this asset (body + both hand poses).
    bool hasBrushRig() const {
        return partBody >= 0 && partHandRest >= 0 && partHandGrab >= 0;
    }
};
