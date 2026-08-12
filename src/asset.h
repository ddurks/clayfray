#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Rigid glass-bead prop (eyes, teeth, buttons): authored in Blender as
// objects named marble_*, never voxelized into the clay.
struct MarbleProp {
    float pos[3];
    float radius;
    float color[3];
};

struct AssetBone {
    std::string name;
    int parent = -1;
    float invBind[16];   // inverse bind matrix, column-major
    float restLocal[16]; // local rest transform, column-major
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

    bool load(const std::string& path);
    uint32_t vertexCount() const { return (uint32_t)positions.size() / 3; }
    uint32_t triangleCount() const { return (uint32_t)indices.size() / 3; }
};
