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

// Floating-hand IK (M4.7, reworked for the blob rig). The fighter has NO
// arms: a blob body (base.*) that barely deforms, and two detached mitts
// (hand.<side> + thumb/finger), so there is no shoulder->elbow->wrist chain
// to solve. Each hand is instead moved BODILY to its grip on the sword, and
// the only thing tethering it to the fighter is `reach` — a maximum distance
// from the body's centre of mass. Same post-pass shape as the old arm solve:
// reconstruct world transforms, transform the hand subtree rigidly, rewrite
// the affected skin mats.
struct HandIkChain {
    int wrist = -1;            // hand.<side>
    std::vector<int> subtree;  // wrist + thumb/finger descendants, moved as one
    // Each digit (thumb.*, finger.*) as its own subtree, root first. These are
    // curled about the handle AFTER the whole mitt has been placed, so the
    // fingers wrap the blade without the palm leaving the grip.
    std::vector<std::vector<int>> digits;
    float curl = 0.f;          // radians about the handle axis, signed per hand
    float target[3] = {0, 0, 0};  // world grip position (where the PALM sits)
    float aim[3] = {0, 0, 1};     // world blade/handle axis to grip around
    float roll = 0.f;             // spin about the handle: where fingers point
    // The grip sits INSIDE the mitt, not on the wrist joint, so the wrist is
    // placed this far back along the finger axis. Caller sets it per frame as
    // a fraction of restSpan (see HandParams::palmFrac).
    float palmLen = 0.f;
    float restSpan = 0.f;        // wrist -> fingertip rest length, meters
    float restAim[3] = {0, 1, 0}; // wrist-local unit vector toward the fingertip
    // Wrist-local axis laid ALONG the handle. It is the mitt's THINNEST axis
    // (measured off the mesh at import), which is what lets two hands stack:
    // gripping with the fingers pointing up the blade instead spends the
    // mitt's 0.27 m length on the handle and the two hands interpenetrate.
    float restEdge[3] = {1, 0, 0};
};

// com/reach: the tether. Each target is clamped into the ball of radius
// `reach` about `com` before solving, so a sword held out of range drags its
// hand only as far as the body allows (and the grip visibly slips). reach
// <= 0 disables the clamp. `orient` additionally rolls the mitt so its
// fingers follow `aim`; off leaves the rest orientation alone.
void applyHandIk(const std::vector<AssetBone>& bones,
                 const std::vector<HandIkChain>& chains, const float com[3],
                 float reach, bool orient, std::vector<float>& skinMats);

// Centre of mass of the BODY (the blob — every bone that is not part of a
// hand subtree), split per bone so it can be re-evaluated under any pose.
// Mass is vertex skin weight, which is what actually moves the surface.
struct BodyCom {
    std::vector<int> bone;    // body bones carrying weight
    std::vector<float> restC; // xyz rest-space centroid of each bone's mass
    std::vector<float> mass;  // total vertex weight bound to that bone
};
// Eye gaze. Each eye bone is a LEAF under the head, so posing it is a single
// rotation about its own origin. Blender aims a bone along its local +Y, and
// on this rig that axis comes out of the face, so it doubles as the look
// direction. The cone is measured from the bone's FK forward, which means it
// travels with the head instead of being pinned to world space.
struct GazeChain {
    int bone = -1;
    float restAim[3] = {0, 1, 0}; // bone-local look axis
};
// Rotates each eye toward `target` (world), clamped to `maxAngle` radians off
// its FK forward. Beyond that the eye holds at the cone edge rather than
// snapping or rolling past the head.
void applyGaze(const std::vector<AssetBone>& bones,
               const std::vector<GazeChain>& chains, const float target[3],
               float maxAngle, std::vector<float>& skinMats);

BodyCom deriveBodyCom(const CharacterAsset& asset, const std::vector<int>& handBones);
// COM = sum(mass_b * skinMat_b * restC_b) / sum(mass_b) — exactly the linear
// blend the surface itself gets, so the tether anchor tracks the blob.
void evalBodyCom(const BodyCom& bc, const std::vector<float>& skinMats, float out[3]);

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
