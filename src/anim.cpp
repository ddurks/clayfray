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

float mat3MinSingular(const float m[16]) {
    // M = A^T A, symmetric positive semi-definite; sigmaMin = sqrt(lambdaMin).
    // Column-major, so column c is m[c*4 + 0..2].
    float M[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            M[i][j] = m[i * 4] * m[j * 4] + m[i * 4 + 1] * m[j * 4 + 1] +
                      m[i * 4 + 2] * m[j * 4 + 2];
        }
    }
    const float p1 = M[0][1] * M[0][1] + M[0][2] * M[0][2] + M[1][2] * M[1][2];
    float lambdaMin;
    if (p1 <= 1e-20f) {
        // already diagonal (A's columns are orthogonal — the common case: a
        // pure rotation, or a rotation times an axis scale)
        lambdaMin = std::min(M[0][0], std::min(M[1][1], M[2][2]));
    } else {
        // Standard closed form for the eigenvalues of a symmetric 3x3
        // (Smith 1961): shift by the mean, normalise, recover the angle.
        const float q = (M[0][0] + M[1][1] + M[2][2]) / 3.f;
        const float d0 = M[0][0] - q, d1 = M[1][1] - q, d2 = M[2][2] - q;
        const float p2 = d0 * d0 + d1 * d1 + d2 * d2 + 2.f * p1;
        const float pp = std::sqrt(p2 / 6.f);
        float B[3][3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                B[i][j] = (M[i][j] - (i == j ? q : 0.f)) / pp;
            }
        }
        const float det =
            B[0][0] * (B[1][1] * B[2][2] - B[1][2] * B[2][1]) -
            B[0][1] * (B[1][0] * B[2][2] - B[1][2] * B[2][0]) +
            B[0][2] * (B[1][0] * B[2][1] - B[1][1] * B[2][0]);
        const float r = std::min(std::max(det * 0.5f, -1.f), 1.f);
        const float phi = std::acos(r) / 3.f;
        // eig1 is the largest at phi, eig3 the smallest at phi + 2pi/3
        lambdaMin = q + 2.f * pp * std::cos(phi + 2.0943951f);
    }
    return std::sqrt(std::max(lambdaMin, 0.f));
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

// Rotation by `angle` about a UNIT axis (column-major 3x3 in a padded [9]).
void axisAngle(const float* axis, float angle, float* R) {
    float x = axis[0], y = axis[1], z = axis[2];
    float ca = std::cos(angle), sa = std::sin(angle), t = 1.f - ca;
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

void applyHandIk(const std::vector<AssetBone>& bones,
                 const std::vector<HandIkChain>& chains, const float com[3],
                 float reach, bool orient, std::vector<float>& skinMats) {
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
    for (const HandIkChain& ch : chains) {
        if (ch.wrist < 0 || (size_t)ch.wrist >= n) continue;
        // COPY: the subtree loop below overwrites world[wrist] in place
        const float* Wm = &world[ch.wrist * 16];
        float W[3] = {Wm[12], Wm[13], Wm[14]};

        // the tether: pull the grip inside the reach ball about the body's
        // centre of mass. Everything else is unconstrained — the hand floats.
        float t[3] = {ch.target[0], ch.target[1], ch.target[2]};
        if (reach > 0.f) {
            float d[3];
            v3sub(t, com, d);
            float L = v3len(d);
            if (L > reach) {
                float k = reach / L;
                t[0] = com[0] + d[0] * k;
                t[1] = com[1] + d[1] * k;
                t[2] = com[2] + d[2] * k;
            }
        }

        // carry the two wrist-local reference axes through the posed rotation
        auto localToWorld = [&](const float* v, float* o) {
            o[0] = Wm[0] * v[0] + Wm[4] * v[1] + Wm[8] * v[2];
            o[1] = Wm[1] * v[0] + Wm[5] * v[1] + Wm[9] * v[2];
            o[2] = Wm[2] * v[0] + Wm[6] * v[1] + Wm[10] * v[2];
            v3norm(o);
        };
        float aimNow[3], edgeNow[3];
        localToWorld(ch.restAim, aimNow);
        localToWorld(ch.restEdge, edgeNow);

        float M[16];
        float aimTo[3] = {aimNow[0], aimNow[1], aimNow[2]};
        float want[3] = {ch.aim[0], ch.aim[1], ch.aim[2]};
        if (orient && v3len(want) > 1e-6f) {
            v3norm(want);
            // FULL frame, not a single-axis swing: aiming one axis leaves the
            // spin about it undetermined, and an arbitrary minimal rotation is
            // what let the two mitts land in the same place. Handle axis ->
            // the mitt's thin edge; fingers -> across the handle, `roll`
            // choosing where around it they point.
            // The mitt is really a stubby ARM: 0.25 m from wrist to fingertip
            // on a 0.69 m body. So the finger axis runs along the REACH — body
            // out to the grip — and the hand end lands on the handle. Two
            // grips a hand's width apart then fan apart naturally from the
            // body instead of converging into one lump, which is what made
            // them interpenetrate when both were aimed down the blade.
            float Yd[3];
            v3sub(t, com, Yd);
            if (v3len(Yd) < 1e-4f) { Yd[0] = 0.f; Yd[1] = 0.f; Yd[2] = 1.f; }
            v3norm(Yd);
            // roll the mitt about that reach axis
            if (ch.roll != 0.f) {
                float R0[9];
                axisAngle(Yd, ch.roll, R0);
                float w2[3] = {R0[0] * want[0] + R0[3] * want[1] + R0[6] * want[2],
                               R0[1] * want[0] + R0[4] * want[1] + R0[7] * want[2],
                               R0[2] * want[0] + R0[5] * want[1] + R0[8] * want[2]};
                std::memcpy(want, w2, sizeof(want));
            }
            // narrow edge toward the handle: the mitt's thin dimension is what
            // sits on the grip, so stacked hands stay clear of each other
            float dd = v3dot(Yd, want);
            float Xd[3] = {want[0] - Yd[0] * dd, want[1] - Yd[1] * dd,
                           want[2] - Yd[2] * dd};
            if (v3len(Xd) < 1e-4f) { // blade parallel to the reach
                float alt[3] = {0.f, 1.f, 0.f};
                dd = v3dot(Yd, alt);
                Xd[0] = alt[0] - Yd[0] * dd;
                Xd[1] = alt[1] - Yd[1] * dd;
                Xd[2] = alt[2] - Yd[2] * dd;
                if (v3len(Xd) < 1e-4f) { Xd[0] = 1.f; Xd[1] = 0.f; Xd[2] = 0.f; }
            }
            v3norm(Xd);
            float Zd[3];
            v3cross(Xd, Yd, Zd);

            // the same two axes as they stand now C = [edge | aim | third]
            float Xc[3] = {edgeNow[0], edgeNow[1], edgeNow[2]};
            float dc = v3dot(Xc, aimNow);
            float Yc[3] = {aimNow[0] - Xc[0] * dc, aimNow[1] - Xc[1] * dc,
                           aimNow[2] - Xc[2] * dc};
            if (v3len(Yc) < 1e-4f) { // degenerate rig (edge parallel to fingers)
                matIdentity(M);
            } else {
                v3norm(Yc);
                float Zc[3];
                v3cross(Xc, Yc, Zc);
                // R = D * C^T maps each current axis onto its desired one
                const float A[9] = {Xd[0], Xd[1], Xd[2], Yd[0], Yd[1],
                                    Yd[2], Zd[0], Zd[1], Zd[2]};
                const float B[9] = {Xc[0], Xc[1], Xc[2], Yc[0], Yc[1],
                                    Yc[2], Zc[0], Zc[1], Zc[2]};
                float R[9];
                for (int col = 0; col < 3; col++)
                    for (int row = 0; row < 3; row++)
                        R[col * 3 + row] = A[row] * B[col] +
                                           A[3 + row] * B[3 + col] +
                                           A[6 + row] * B[6 + col];
                rotAboutPivot(R, W, M); // pivot at the wrist: W stays put
                std::memcpy(aimTo, Yd, sizeof(aimTo));
            }
        } else {
            matIdentity(M);
        }

        // back the wrist off along the (new) finger axis so the PALM, not the
        // joint, lands on the grip. Rotating about W left it in place, so the
        // remaining translation folds straight into M's last column.
        float Wp[3] = {t[0] - aimTo[0] * ch.palmLen, t[1] - aimTo[1] * ch.palmLen,
                       t[2] - aimTo[2] * ch.palmLen};
        M[12] += Wp[0] - W[0];
        M[13] += Wp[1] - W[1];
        M[14] += Wp[2] - W[2];

        for (int b : ch.subtree) {
            if (b < 0 || (size_t)b >= n) continue;
            float tmp[16];
            matMul(M, &world[b * 16], tmp);
            std::memcpy(&world[b * 16], tmp, sizeof(tmp));
            dirty[b] = 1;
        }

        // FINGER CURL, after the mitt is in place. Fingers close around a
        // handle by rotating in the plane perpendicular to it — i.e. about the
        // handle's own axis — so each digit spins about `aim` through its OWN
        // root. The wrist is not in any digit's list, so the palm stays exactly
        // where the IK put it: this curls the fingers without moving the hand.
        float axis[3] = {ch.aim[0], ch.aim[1], ch.aim[2]};
        if (ch.curl != 0.f && v3len(axis) > 1e-6f) {
            v3norm(axis);
            float Rc[9];
            axisAngle(axis, ch.curl, Rc);
            for (const std::vector<int>& dig : ch.digits) {
                if (dig.empty()) continue;
                int root = dig[0];
                if (root < 0 || (size_t)root >= n) continue;
                const float* Dm = &world[root * 16];
                const float pivot[3] = {Dm[12], Dm[13], Dm[14]};
                float C[16];
                rotAboutPivot(Rc, pivot, C);
                for (int b : dig) {
                    if (b < 0 || (size_t)b >= n) continue;
                    float tmp[16];
                    matMul(C, &world[b * 16], tmp);
                    std::memcpy(&world[b * 16], tmp, sizeof(tmp));
                    dirty[b] = 1;
                }
            }
        }
    }

    for (size_t j = 0; j < n; j++) {
        if (dirty[j]) matMul(&world[j * 16], bones[j].invBind, &skinMats[j * 16]);
    }
}

void applyGaze(const std::vector<AssetBone>& bones,
               const std::vector<GazeChain>& chains, const float target[3],
               float maxAngle, std::vector<float>& skinMats) {
    const size_t n = bones.size();
    if (skinMats.size() < n * 16 || chains.empty()) return;

    for (const GazeChain& ch : chains) {
        if (ch.bone < 0 || (size_t)ch.bone >= n) continue;
        float bind[16], world[16];
        matInvAffine(bones[ch.bone].invBind, bind);
        matMul(&skinMats[ch.bone * 16], bind, world);
        const float E[3] = {world[12], world[13], world[14]};

        // FK forward: the neutral the cone is measured from, so a turned head
        // carries its own gaze range around with it
        float f0[3] = {
            world[0] * ch.restAim[0] + world[4] * ch.restAim[1] + world[8] * ch.restAim[2],
            world[1] * ch.restAim[0] + world[5] * ch.restAim[1] + world[9] * ch.restAim[2],
            world[2] * ch.restAim[0] + world[6] * ch.restAim[1] + world[10] * ch.restAim[2]};
        v3norm(f0);

        float d[3];
        v3sub(target, E, d);
        if (v3len(d) < 1e-5f) continue;
        v3norm(d);

        // hold at the cone edge instead of tracking past it
        float c = v3dot(f0, d);
        float lim = std::cos(std::max(maxAngle, 0.f));
        if (c < lim) {
            float perp[3] = {d[0] - f0[0] * c, d[1] - f0[1] * c, d[2] - f0[2] * c};
            if (v3len(perp) < 1e-5f) continue; // exactly behind: no unique way out
            v3norm(perp);
            float s = std::sin(maxAngle);
            d[0] = f0[0] * lim + perp[0] * s;
            d[1] = f0[1] * lim + perp[1] * s;
            d[2] = f0[2] * lim + perp[2] * s;
            v3norm(d);
        }

        float R[9];
        rotationBetween(f0, d, R);
        float M[16], posed[16];
        rotAboutPivot(R, E, M);
        matMul(M, world, posed);
        matMul(posed, bones[ch.bone].invBind, &skinMats[ch.bone * 16]);
    }
}

BodyCom deriveBodyCom(const CharacterAsset& asset, const std::vector<int>& handBones) {
    BodyCom bc;
    const size_t nb = asset.bones.size();
    const uint32_t nv = asset.vertexCount();
    if (nb == 0 || nv == 0 || asset.joints.size() < nv * 4 ||
        asset.weights.size() < nv * 4)
        return bc;

    std::vector<uint8_t> isHand(nb, 0);
    for (int b : handBones)
        if (b >= 0 && (size_t)b < nb) isHand[b] = 1;

    std::vector<float> mass(nb, 0.f), acc(nb * 3, 0.f);
    for (uint32_t v = 0; v < nv; v++) {
        for (int s = 0; s < 4; s++) {
            float w = asset.weights[v * 4 + s];
            if (w <= 0.f) continue;
            uint16_t j = asset.joints[v * 4 + s];
            if (j >= nb || isHand[j]) continue; // hands are not the blob
            mass[j] += w;
            acc[j * 3] += w * asset.positions[v * 3];
            acc[j * 3 + 1] += w * asset.positions[v * 3 + 1];
            acc[j * 3 + 2] += w * asset.positions[v * 3 + 2];
        }
    }
    for (size_t j = 0; j < nb; j++) {
        if (mass[j] <= 1e-6f) continue;
        bc.bone.push_back((int)j);
        bc.mass.push_back(mass[j]);
        bc.restC.push_back(acc[j * 3] / mass[j]);
        bc.restC.push_back(acc[j * 3 + 1] / mass[j]);
        bc.restC.push_back(acc[j * 3 + 2] / mass[j]);
    }
    return bc;
}

void evalBodyCom(const BodyCom& bc, const std::vector<float>& skinMats, float out[3]) {
    out[0] = out[1] = out[2] = 0.f;
    float total = 0.f;
    for (size_t i = 0; i < bc.bone.size(); i++) {
        int b = bc.bone[i];
        if (b < 0 || (size_t)(b * 16 + 16) > skinMats.size()) continue;
        float p[3];
        matTransformPoint(&skinMats[b * 16], &bc.restC[i * 3], p);
        float m = bc.mass[i];
        out[0] += m * p[0];
        out[1] += m * p[1];
        out[2] += m * p[2];
        total += m;
    }
    if (total > 1e-6f) {
        out[0] /= total;
        out[1] /= total;
        out[2] /= total;
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
