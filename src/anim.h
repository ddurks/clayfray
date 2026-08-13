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

// 2-bone IK (M4.7): rotate a shoulder->elbow->wrist chain so `wrist` reaches
// `target` (world), the elbow biased toward `pole`. Runs as a post-pass over
// evalPose's skin matrices: reconstruct world transforms, rotate the two
// subtrees rigidly (shoulder subtree about the shoulder, elbow subtree about
// the solved elbow), rewrite the affected skin mats. Subtree bone lists are
// precomputed by the caller so the hand/fingers/thumb ride along.
struct ArmIkChain {
    int shoulder = -1, elbow = -1, wrist = -1;
    std::vector<int> upperSubtree; // shoulder + all descendants
    std::vector<int> lowerSubtree; // elbow + all descendants
    float target[3] = {0, 0, 0};   // world grip position (where the PALM sits)
    float pole[3] = {0, -0.5f, -1.f}; // world-ish elbow-bend hint (fallback)
    // wrist-to-palm reach: the hand mesh extends past the wrist joint, so the
    // wrist is aimed short of the grip by this much (along shoulder->grip) to
    // land the palm on the grip instead of the wrist. Rest hand->fingertips.
    float handLen = 0.f;
};
void applyArmIk(const std::vector<AssetBone>& bones,
                const std::vector<ArmIkChain>& chains, std::vector<float>& skinMats);

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
