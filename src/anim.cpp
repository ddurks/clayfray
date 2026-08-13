#include "anim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

void matIdentity(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

void matMul(const float* a, const float* b, float* out) {
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int rw = 0; rw < 4; rw++) {
            r[c * 4 + rw] = a[rw] * b[c * 4] + a[4 + rw] * b[c * 4 + 1] +
                            a[8 + rw] * b[c * 4 + 2] + a[12 + rw] * b[c * 4 + 3];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void matInvAffine(const float* m, float* out) {
    // invert upper-left 3x3, then translation
    const float a = m[0], b = m[4], c = m[8];
    const float d = m[1], e = m[5], f = m[9];
    const float g = m[2], h = m[6], i = m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) det = 1e-12f;
    const float id = 1.f / det;
    float r[16];
    r[0] = (e * i - f * h) * id;
    r[4] = (c * h - b * i) * id;
    r[8] = (b * f - c * e) * id;
    r[1] = (f * g - d * i) * id;
    r[5] = (a * i - c * g) * id;
    r[9] = (c * d - a * f) * id;
    r[2] = (d * h - e * g) * id;
    r[6] = (b * g - a * h) * id;
    r[10] = (a * e - b * d) * id;
    r[3] = r[7] = r[11] = 0.f;
    r[12] = -(r[0] * m[12] + r[4] * m[13] + r[8] * m[14]);
    r[13] = -(r[1] * m[12] + r[5] * m[13] + r[9] * m[14]);
    r[14] = -(r[2] * m[12] + r[6] * m[13] + r[10] * m[14]);
    r[15] = 1.f;
    std::memcpy(out, r, sizeof(r));
}

void matTransformPoint(const float* m, const float* p, float* out) {
    float r[3];
    r[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    r[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    r[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

namespace {

struct Trs {
    float t[3];
    float r[4]; // quat xyzw
    float s[3];
};

void decompose(const float* m, Trs& o) {
    o.t[0] = m[12]; o.t[1] = m[13]; o.t[2] = m[14];
    float col[3][3];
    for (int c = 0; c < 3; c++) {
        float len = std::sqrt(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] +
                              m[c * 4 + 2] * m[c * 4 + 2]);
        o.s[c] = len < 1e-12f ? 1.f : len;
        for (int rw = 0; rw < 3; rw++) col[c][rw] = m[c * 4 + rw] / o.s[c];
    }
    // rotation matrix -> quat (Shepperd)
    const float tr = col[0][0] + col[1][1] + col[2][2];
    if (tr > 0.f) {
        float sq = std::sqrt(tr + 1.f) * 2.f;
        o.r[3] = 0.25f * sq;
        o.r[0] = (col[1][2] - col[2][1]) / sq;
        o.r[1] = (col[2][0] - col[0][2]) / sq;
        o.r[2] = (col[0][1] - col[1][0]) / sq;
    } else if (col[0][0] > col[1][1] && col[0][0] > col[2][2]) {
        float sq = std::sqrt(1.f + col[0][0] - col[1][1] - col[2][2]) * 2.f;
        o.r[3] = (col[1][2] - col[2][1]) / sq;
        o.r[0] = 0.25f * sq;
        o.r[1] = (col[1][0] + col[0][1]) / sq;
        o.r[2] = (col[2][0] + col[0][2]) / sq;
    } else if (col[1][1] > col[2][2]) {
        float sq = std::sqrt(1.f + col[1][1] - col[0][0] - col[2][2]) * 2.f;
        o.r[3] = (col[2][0] - col[0][2]) / sq;
        o.r[0] = (col[1][0] + col[0][1]) / sq;
        o.r[1] = 0.25f * sq;
        o.r[2] = (col[2][1] + col[1][2]) / sq;
    } else {
        float sq = std::sqrt(1.f + col[2][2] - col[0][0] - col[1][1]) * 2.f;
        o.r[3] = (col[0][1] - col[1][0]) / sq;
        o.r[0] = (col[2][0] + col[0][2]) / sq;
        o.r[1] = (col[2][1] + col[1][2]) / sq;
        o.r[2] = 0.25f * sq;
    }
}

void compose(const Trs& o, float* m) {
    const float x = o.r[0], y = o.r[1], z = o.r[2], w = o.r[3];
    const float rot[9] = {
        1.f - 2.f * (y * y + z * z), 2.f * (x * y + z * w),       2.f * (x * z - y * w),
        2.f * (x * y - z * w),       1.f - 2.f * (x * x + z * z), 2.f * (y * z + x * w),
        2.f * (x * z + y * w),       2.f * (y * z - x * w),       1.f - 2.f * (x * x + y * y)};
    for (int c = 0; c < 3; c++) {
        for (int rw = 0; rw < 3; rw++) m[c * 4 + rw] = rot[c * 3 + rw] * o.s[c];
        m[c * 4 + 3] = 0.f;
    }
    m[12] = o.t[0]; m[13] = o.t[1]; m[14] = o.t[2]; m[15] = 1.f;
}

void sampleTrack(const AnimTrack& tr, float t, float* out, int comps) {
    const size_t n = tr.times.size();
    if (n == 0) return;
    if (t <= tr.times[0] || n == 1) {
        std::memcpy(out, tr.values.data(), comps * sizeof(float));
        return;
    }
    if (t >= tr.times[n - 1]) {
        std::memcpy(out, &tr.values[(n - 1) * comps], comps * sizeof(float));
        return;
    }
    size_t hi = std::upper_bound(tr.times.begin(), tr.times.end(), t) - tr.times.begin();
    size_t lo = hi - 1;
    const float* a = &tr.values[lo * comps];
    if (tr.interp == 0) { // STEP
        std::memcpy(out, a, comps * sizeof(float));
        return;
    }
    const float* b = &tr.values[hi * comps];
    float u = (t - tr.times[lo]) / (tr.times[hi] - tr.times[lo]);
    if (tr.path == 1) {
        // shortest-path nlerp: at 12 Hz pose steps slerp's constant angular
        // velocity is invisible
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        float sgn = dot < 0.f ? -1.f : 1.f;
        float q[4], len = 0.f;
        for (int k = 0; k < 4; k++) {
            q[k] = a[k] * (1.f - u) + b[k] * sgn * u;
            len += q[k] * q[k];
        }
        len = std::sqrt(len);
        if (len < 1e-12f) len = 1.f;
        for (int k = 0; k < 4; k++) out[k] = q[k] / len;
    } else {
        for (int k = 0; k < comps; k++) out[k] = a[k] * (1.f - u) + b[k] * u;
    }
}

} // namespace

void evalPose(const std::vector<AssetBone>& bones, const AnimClip* clip, float t,
              std::vector<float>& skinMats) {
    const size_t n = bones.size();
    skinMats.resize(n * 16);
    if (!clip) {
        // rest pose: world * invBind == identity by definition of invBind
        for (size_t j = 0; j < n; j++) matIdentity(&skinMats[j * 16]);
        return;
    }

    // per-joint local transforms: rest unless a track overrides a component
    std::vector<float> local(n * 16);
    std::vector<uint8_t> tracked(n, 0);
    std::vector<Trs> trs(n);
    for (size_t j = 0; j < n; j++) {
        std::memcpy(&local[j * 16], bones[j].restLocal, 16 * sizeof(float));
    }
    for (const AnimTrack& tr : clip->tracks) {
        if (tr.joint < 0 || (size_t)tr.joint >= n) continue;
        if (!tracked[tr.joint]) {
            decompose(bones[tr.joint].restLocal, trs[tr.joint]);
            tracked[tr.joint] = 1;
        }
        Trs& o = trs[tr.joint];
        if (tr.path == 0) sampleTrack(tr, t, o.t, 3);
        else if (tr.path == 1) sampleTrack(tr, t, o.r, 4);
        else sampleTrack(tr, t, o.s, 3);
    }
    for (size_t j = 0; j < n; j++) {
        if (tracked[j]) compose(trs[j], &local[j * 16]);
    }

    // hierarchy compose; joints may appear before their parent in the skin
    // array, so resolve recursively with memoization
    std::vector<float> world(n * 16);
    std::vector<uint8_t> done(n, 0);
    struct Walker {
        const std::vector<AssetBone>& bones;
        std::vector<float>& local;
        std::vector<float>& world;
        std::vector<uint8_t>& done;
        void resolve(size_t j) {
            if (done[j]) return;
            done[j] = 1; // set before recursing: breaks accidental cycles
            const int p = bones[j].parent;
            if (p >= 0) {
                resolve((size_t)p);
                matMul(&world[p * 16], &local[j * 16], &world[j * 16]);
            } else {
                matMul(bones[j].pre, &local[j * 16], &world[j * 16]);
            }
        }
    } walker{bones, local, world, done};
    for (size_t j = 0; j < n; j++) walker.resolve(j);

    for (size_t j = 0; j < n; j++) {
        matMul(&world[j * 16], bones[j].invBind, &skinMats[j * 16]);
    }
}

namespace {

void v3sub(const float* a, const float* b, float* o) {
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
float v3dot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
void v3cross(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
float v3len(const float* a) { return std::sqrt(v3dot(a, a)); }
void v3norm(float* a) {
    float l = v3len(a);
    if (l > 1e-9f) { a[0] /= l; a[1] /= l; a[2] /= l; }
}

// Rotation (column-major 3x3 in a padded [9]) taking unit-ish `from` to `to`.
void rotationBetween(const float* fromIn, const float* toIn, float* R) {
    float from[3] = {fromIn[0], fromIn[1], fromIn[2]};
    float to[3] = {toIn[0], toIn[1], toIn[2]};
    v3norm(from); v3norm(to);
    float axis[3];
    v3cross(from, to, axis);
    float s = v3len(axis);
    float c = v3dot(from, to);
    if (s < 1e-6f) {
        // parallel (c~1: identity) or antiparallel (c~-1: 180° about any perp)
        if (c > 0.f) {
            R[0] = 1; R[1] = 0; R[2] = 0;
            R[3] = 0; R[4] = 1; R[5] = 0;
            R[6] = 0; R[7] = 0; R[8] = 1;
            return;
        }
        float t[3] = {1, 0, 0};
        if (std::fabs(from[0]) > 0.9f) { t[0] = 0; t[1] = 1; }
        v3cross(from, t, axis);
        v3norm(axis);
        s = 0.f; c = -1.f;
    } else {
        axis[0] /= s; axis[1] /= s; axis[2] /= s;
    }
    float angle = std::atan2(s, c);
    float x = axis[0], y = axis[1], z = axis[2];
    float ca = std::cos(angle), sa = std::sin(angle), t = 1.f - ca;
    // column-major: R[col*3 + row]
    R[0] = t * x * x + ca;     R[1] = t * x * y + sa * z; R[2] = t * x * z - sa * y;
    R[3] = t * x * y - sa * z; R[4] = t * y * y + ca;     R[5] = t * y * z + sa * x;
    R[6] = t * x * z + sa * y; R[7] = t * y * z - sa * x; R[8] = t * z * z + ca;
}

// 4x4 (column-major) applying R about `pivot`: x -> pivot + R*(x - pivot).
void rotAboutPivot(const float* R, const float* pivot, float* m) {
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) m[col * 4 + row] = R[col * 3 + row];
        m[col * 4 + 3] = 0.f;
    }
    // translation = pivot - R*pivot
    float rp[3];
    for (int row = 0; row < 3; row++)
        rp[row] = R[row] * pivot[0] + R[3 + row] * pivot[1] + R[6 + row] * pivot[2];
    m[12] = pivot[0] - rp[0];
    m[13] = pivot[1] - rp[1];
    m[14] = pivot[2] - rp[2];
    m[15] = 1.f;
}

} // namespace

void applyArmIk(const std::vector<AssetBone>& bones,
                const std::vector<ArmIkChain>& chains, std::vector<float>& skinMats) {
    const size_t n = bones.size();
    if (skinMats.size() < n * 16 || chains.empty()) return;

    // reconstruct world[j] = skinMats[j] * bind[j], bind = inv(invBind)
    std::vector<float> world(n * 16);
    for (size_t j = 0; j < n; j++) {
        float bind[16];
        matInvAffine(bones[j].invBind, bind);
        matMul(&skinMats[j * 16], bind, &world[j * 16]);
    }

    std::vector<uint8_t> dirty(n, 0);
    for (const ArmIkChain& ch : chains) {
        if (ch.shoulder < 0 || ch.elbow < 0 || ch.wrist < 0) continue;
        const float* S = &world[ch.shoulder * 16 + 12];
        const float* E = &world[ch.elbow * 16 + 12];
        const float* W = &world[ch.wrist * 16 + 12];
        float SE[3], EW[3];
        v3sub(E, S, SE);
        v3sub(W, E, EW);
        float L1 = v3len(SE), L2 = v3len(EW);
        if (L1 < 1e-5f || L2 < 1e-5f) continue;

        // target, clamped to the reachable annulus (arc-clamped-to-reach)
        float toT[3];
        v3sub(ch.target, S, toT);
        float d = v3len(toT);
        float dir[3] = {0, -1, 0};
        if (d > 1e-6f) { dir[0] = toT[0] / d; dir[1] = toT[1] / d; dir[2] = toT[2] / d; }
        float dmax = L1 + L2 - 0.005f, dmin = std::fabs(L1 - L2) + 0.005f;
        float dc = std::min(std::max(d, dmin), dmax);

        // elbow-bend plane: keep the FK pose's bend direction (perp component
        // of the shoulder->elbow vector), fall back to the hint if straight
        float along = v3dot(SE, dir);
        float pole[3] = {SE[0] - dir[0] * along, SE[1] - dir[1] * along,
                         SE[2] - dir[2] * along};
        if (v3len(pole) < 1e-5f) {
            float pa = v3dot(ch.pole, dir);
            pole[0] = ch.pole[0] - dir[0] * pa;
            pole[1] = ch.pole[1] - dir[1] * pa;
            pole[2] = ch.pole[2] - dir[2] * pa;
        }
        v3norm(pole);

        // law of cosines: elbow E' = S + dir*a + pole*h
        float a = (L1 * L1 - L2 * L2 + dc * dc) / (2.f * dc);
        float h = std::sqrt(std::max(L1 * L1 - a * a, 0.f));
        float Ep[3] = {S[0] + dir[0] * a + pole[0] * h, S[1] + dir[1] * a + pole[1] * h,
                       S[2] + dir[2] * a + pole[2] * h};
        float Wp[3] = {S[0] + dir[0] * dc, S[1] + dir[1] * dc, S[2] + dir[2] * dc};

        // q1: swing the whole arm so shoulder->elbow points at E'
        float newSE[3];
        v3sub(Ep, S, newSE);
        float R1[9];
        rotationBetween(SE, newSE, R1);
        float M1[16];
        rotAboutPivot(R1, S, M1);
        // propagate the wrist through M1, then q2 about E' aims forearm at W'
        float Wprop[3];
        matTransformPoint(M1, W, Wprop);
        float fwFrom[3], fwTo[3];
        v3sub(Wprop, Ep, fwFrom);
        v3sub(Wp, Ep, fwTo);
        float R2[9];
        rotationBetween(fwFrom, fwTo, R2);
        float M2[16];
        rotAboutPivot(R2, Ep, M2);

        // upper subtree rotates about S; lower (a subset) additionally about E'
        for (int b : ch.upperSubtree) {
            float tmp[16];
            matMul(M1, &world[b * 16], tmp);
            std::memcpy(&world[b * 16], tmp, sizeof(tmp));
            dirty[b] = 1;
        }
        for (int b : ch.lowerSubtree) {
            float tmp[16];
            matMul(M2, &world[b * 16], tmp);
            std::memcpy(&world[b * 16], tmp, sizeof(tmp));
            dirty[b] = 1;
        }
    }

    for (size_t j = 0; j < n; j++) {
        if (dirty[j]) matMul(&world[j * 16], bones[j].invBind, &skinMats[j * 16]);
    }
}

std::vector<BoneCapsule> deriveCapsules(const CharacterAsset& asset) {
    std::vector<BoneCapsule> out;
    const size_t n = asset.bones.size();
    if (n == 0 || asset.positions.empty()) return out;

    // rest-space joint origins
    std::vector<float> origin(n * 3);
    for (size_t j = 0; j < n; j++) {
        float inv[16];
        matInvAffine(asset.bones[j].invBind, inv);
        origin[j * 3] = inv[12];
        origin[j * 3 + 1] = inv[13];
        origin[j * 3 + 2] = inv[14];
    }
    std::vector<std::vector<int>> children(n);
    for (size_t j = 0; j < n; j++) {
        if (asset.bones[j].parent >= 0) children[asset.bones[j].parent].push_back((int)j);
    }

    // vertex buckets: dominant-weight (shadow capsule fit) and wide
    // (weight >= 0.25 — the chunk, including joint blend overlap)
    std::vector<std::vector<uint32_t>> verts(n);
    std::vector<std::vector<uint32_t>> wide(n);
    const uint32_t vc = asset.vertexCount();
    for (uint32_t v = 0; v < vc; v++) {
        int best = -1;
        float bw = 0.f;
        for (int s = 0; s < 4; s++) {
            float w = asset.weights[v * 4 + s];
            int j = asset.joints[v * 4 + s];
            if (j < 0 || (size_t)j >= n) continue;
            if (w > bw) {
                bw = w;
                best = j;
            }
            if (w >= 0.25f) wide[j].push_back(v);
        }
        if (best >= 0 && bw > 0.f) {
            verts[best].push_back(v);
            // dominant bone always owns its verts even under 0.25 (>2 strong
            // influences can dilute every weight)
            if (bw < 0.25f) wide[best].push_back(v);
        }
    }

    for (size_t j = 0; j < n; j++) {
        if (verts[j].size() < 4) continue;
        BoneCapsule c{};
        c.bone = (int)j;
        c.a[0] = origin[j * 3]; c.a[1] = origin[j * 3 + 1]; c.a[2] = origin[j * 3 + 2];
        if (!children[j].empty()) {
            // segment toward the mean child origin
            float b[3] = {0, 0, 0};
            for (int ch : children[j]) {
                for (int k = 0; k < 3; k++) b[k] += origin[ch * 3 + k];
            }
            for (int k = 0; k < 3; k++) c.b[k] = b[k] / (float)children[j].size();
        } else {
            // leaf: segment toward the vertex centroid
            float b[3] = {0, 0, 0};
            for (uint32_t v : verts[j]) {
                for (int k = 0; k < 3; k++) b[k] += asset.positions[v * 3 + k];
            }
            for (int k = 0; k < 3; k++) c.b[k] = b[k] / (float)verts[j].size();
        }

        // radius: 80th-percentile distance to the segment — capsules should
        // hug the clay, not circumscribe stray weights
        float ab[3] = {c.b[0] - c.a[0], c.b[1] - c.a[1], c.b[2] - c.a[2]};
        float abLen2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        auto segDist = [&](const float* p) {
            float ap[3] = {p[0] - c.a[0], p[1] - c.a[1], p[2] - c.a[2]};
            float h = 0.f;
            if (abLen2 > 1e-12f) {
                h = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / abLen2;
                h = std::min(std::max(h, 0.f), 1.f);
            }
            float d[3] = {ap[0] - ab[0] * h, ap[1] - ab[1] * h, ap[2] - ab[2] * h};
            return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        };
        std::vector<float> dist;
        dist.reserve(verts[j].size());
        for (uint32_t v : verts[j]) dist.push_back(segDist(&asset.positions[v * 3]));
        std::nth_element(dist.begin(), dist.begin() + dist.size() * 8 / 10, dist.end());
        c.r = std::min(std::max(dist[dist.size() * 8 / 10], 0.015f), 0.35f);

        // chunk: swollen capsule must cover every influenced vertex (piece =
        // body ∩ capsule; an uncovered surface vert would be sliced off) plus
        // the ±12-voxel narrow band. Tight box bounds the piece's zero set —
        // only body surface, capsule-cap surfaces are interior clay.
        float rMax = 0.f;
        c.lo[0] = c.lo[1] = c.lo[2] = 1e9f;
        c.hi[0] = c.hi[1] = c.hi[2] = -1e9f;
        for (uint32_t v : wide[j]) {
            const float* p = &asset.positions[v * 3];
            rMax = std::max(rMax, segDist(p));
            for (int k = 0; k < 3; k++) {
                c.lo[k] = std::min(c.lo[k], p[k] - 0.012f);
                c.hi[k] = std::max(c.hi[k], p[k] + 0.012f);
            }
        }
        c.rPiece = rMax + 0.035f;
        out.push_back(c);
    }
    return out;
}
