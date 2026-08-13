#pragma once
#include <vector>

#include "asset.h"

// Column-major 4x4 helpers (glTF / AssetBone layout).
void matIdentity(float* m);
void matMul(const float* a, const float* b, float* out); // out = a * b
void matInvAffine(const float* m, float* out);
void matTransformPoint(const float* m, const float* p, float* out);

// Samples `clip` at time t (caller loops/quantizes), composes the joint
// hierarchy, writes one skinning matrix (world * invBind) per bone. Null
// clip yields identity matrices — the rest pose by construction, so props
// and capsules transformed by these are always valid.
void evalPose(const std::vector<AssetBone>& bones, const AnimClip* clip, float t,
              std::vector<float>& skinMats);

// Per-bone capsule + chunk data, fitted at import in rest space.
// - a/b/r: shadow-proxy capsule (radius hugs the clay, 80th percentile of
//   the bone's dominant-weight vertices); posed rigidly by skin matrices.
// - rPiece/lo/hi (M4-P1): the chunk used to articulate the SDF. The piece is
//   body ∩ capsule(a,b,rPiece) evaluated at trace time; rPiece is swollen to
//   cover every vertex this bone influences (weight >= 0.25) plus the narrow
//   band, and lo/hi bound the piece's zero set for conservative culling.
struct BoneCapsule {
    float a[3], b[3];
    float r;
    int bone;
    float rPiece;
    float lo[3], hi[3];
};
std::vector<BoneCapsule> deriveCapsules(const CharacterAsset& asset);
