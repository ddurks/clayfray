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

// A Blender-authored character: triangle mesh (engine space, y-up, meters),
// per-vertex color + skin weights, skeleton, marble props.
struct CharacterAsset {
    std::vector<float> positions;  // xyz per vertex
    std::vector<uint32_t> indices;
    std::vector<float> colors;    // rgb per vertex, linear
    std::vector<uint16_t> joints; // 4 per vertex
    std::vector<float> weights;   // 4 per vertex
    std::vector<AssetBone> bones;
    std::vector<MarbleProp> marbles;
    std::vector<AnimClip> clips;

    bool load(const std::string& path);
    uint32_t vertexCount() const { return (uint32_t)positions.size() / 3; }
    uint32_t triangleCount() const { return (uint32_t)indices.size() / 3; }
};
