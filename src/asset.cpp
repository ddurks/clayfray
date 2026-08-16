#include "asset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "anim.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace {

// centroid and bounding radius of ONE primitive's positions. Marbles authored
// as origin-centred spheres carry position in the node translation, but a
// skinned/parented export bakes it into the vertices instead — reading the
// centroid handles both, so a raw Blender UI export can't corrupt the eyes.
// Per-primitive (not per-mesh) because one eye object is two nested spheres,
// eyeball + pupil, and each has to become its own bead.
void primCentroidRadius(const cgltf_primitive& prim, float scale, float outC[3],
                        float& outR) {
    const cgltf_accessor* acc = nullptr;
    for (cgltf_size a = 0; a < prim.attributes_count; a++) {
        if (prim.attributes[a].type == cgltf_attribute_type_position)
            acc = prim.attributes[a].data;
    }
    outC[0] = outC[1] = outC[2] = 0.f;
    outR = 0.f;
    if (!acc || acc->count == 0) return;

    double c[3] = {0, 0, 0};
    for (cgltf_size i = 0; i < acc->count; i++) {
        float v[3];
        cgltf_accessor_read_float(acc, i, v, 3);
        c[0] += v[0]; c[1] += v[1]; c[2] += v[2];
    }
    c[0] /= (double)acc->count; c[1] /= (double)acc->count; c[2] /= (double)acc->count;
    float r2 = 0.f;
    for (cgltf_size i = 0; i < acc->count; i++) {
        float v[3];
        cgltf_accessor_read_float(acc, i, v, 3);
        float dx = v[0] - (float)c[0], dy = v[1] - (float)c[1], dz = v[2] - (float)c[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) r2 = d2;
    }
    outC[0] = (float)c[0]; outC[1] = (float)c[1]; outC[2] = (float)c[2];
    outR = std::sqrt(r2) * scale;
}

// A primitive is a porcelain bead if its MATERIAL is named marble_*. Material
// rather than object name because the eyes are one skinned object per side
// (they carry gaze bones) whose two primitives are already authored as
// marble_eye_*_mat / marble_pupil_*_mat. Keying on the material keeps clay and
// porcelain separable inside a single object.
bool isMarblePrim(const cgltf_primitive& prim) {
    return prim.material && prim.material->name &&
           std::strncmp(prim.material->name, "marble_", 7) == 0;
}

} // namespace

bool CharacterAsset::load(const std::string& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        std::fprintf(stderr, "asset: failed to parse %s\n", path.c_str());
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        std::fprintf(stderr, "asset: failed to load buffers for %s\n", path.c_str());
        cgltf_free(data);
        return false;
    }

    // CLAUDE.md trap 7 used to read "the character is EVERY mesh bound to the
    // armature, not one node", because the rig authored the blob and its two
    // mitts as separate objects sharing one skin and merging them was the only
    // way to get a whole fighter.
    //
    // M-RIG inverts that. There is no armature, and the meshes must stay
    // SEPARATE — each becomes its own brush at its own place in the volume, and
    // the hand is imported TWICE (rest and grab). So selection is by node name,
    // and the merge is gone. The trap survives in spirit: get the selection
    // wrong and you still get a fighter made of nothing but hands, which is why
    // the summary at the end prints a per-part vertex count.
    const cgltf_skin* skin = nullptr;
    std::vector<const cgltf_node*> bodyNodes;
    const cgltf_node* unskinned = nullptr; // fallback for rigless assets
    const cgltf_node* namedBody = nullptr; // node "body"
    const cgltf_node* namedHand = nullptr; // node "hand"
    for (cgltf_size n = 0; n < data->nodes_count; n++) {
        const cgltf_node* node = &data->nodes[n];
        if (!node->mesh) continue;
        const char* name = node->name ? node->name : "";
        // Beads come from either convention: a whole object named marble_*
        // (the original rule), or any primitive carrying a marble_* material
        // (how the gaze-rigged eyes are authored). Either way they are props,
        // never clay — the geometry pass below skips them.
        const bool wholeNodeIsProp = std::strncmp(name, "marble_", 7) == 0;
        float pworld[16];
        cgltf_node_transform_world(node, pworld);
        float pscale = std::sqrt(pworld[0] * pworld[0] + pworld[1] * pworld[1] +
                                 pworld[2] * pworld[2]);
        for (cgltf_size p = 0; p < node->mesh->primitives_count; p++) {
            const cgltf_primitive& prim = node->mesh->primitives[p];
            if (!wholeNodeIsProp && !isMarblePrim(prim)) continue;
            float centroid[3], radius;
            primCentroidRadius(prim, pscale, centroid, radius);
            if (radius <= 0.f) continue;
            MarbleProp m{};
            // node translation + rotated/scaled centroid: exact for the
            // origin-centred authoring rule, and still correct when a
            // skinned export bakes the position into the vertices
            m.pos[0] = pworld[0] * centroid[0] + pworld[4] * centroid[1] +
                       pworld[8] * centroid[2] + pworld[12];
            m.pos[1] = pworld[1] * centroid[0] + pworld[5] * centroid[1] +
                       pworld[9] * centroid[2] + pworld[13];
            m.pos[2] = pworld[2] * centroid[0] + pworld[6] * centroid[1] +
                       pworld[10] * centroid[2] + pworld[14];
            m.radius = radius;
            m.color[0] = 0.8f; m.color[1] = 0.8f; m.color[2] = 0.8f;
            if (prim.material && prim.material->has_pbr_metallic_roughness) {
                const float* bc =
                    prim.material->pbr_metallic_roughness.base_color_factor;
                m.color[0] = bc[0]; m.color[1] = bc[1]; m.color[2] = bc[2];
            }
            marbles.push_back(m);
        }
        if (wholeNodeIsProp) {
            // fully consumed as beads
        } else if (std::strcmp(name, "body") == 0) {
            namedBody = node;
        } else if (std::strcmp(name, "hand") == 0) {
            namedHand = node;
        } else if (node->skin) {
            if (!skin) skin = node->skin;
            if (node->skin == skin) {
                bodyNodes.push_back(node);
            } else {
                std::fprintf(stderr, "asset: mesh '%s' uses a second skin, skipped\n",
                             name);
            }
        } else if (!unskinned) {
            unskinned = node;
        }
    }
    // ---- brush assembly ----
    // Append one node's clay primitives as a MeshPart. `morph` selects a morph
    // target to apply at full weight (-1 = base shape); `offset` translates the
    // whole part in rest space, which is how the grab hand is moved into the
    // empty negative-x half of the volume.
    //
    // Every part lands in the SAME positions/indices arrays: the voxelizer
    // consumes one triangle soup and does not care that it is three disconnected
    // shells. Its watertight +x parity raycast counts crossings, so disjoint
    // CLOSED shells each resolve correctly on their own (verified: both meshes
    // are closed 2-manifolds once split vertices are welded).
    auto addPart = [&](const cgltf_node* node, const char* partName, int morph,
                       const float offset[3]) -> int {
        if (!node || !node->mesh) return -1;
        MeshPart part;
        part.name = partName;
        part.idxBegin = (uint32_t)indices.size();
        for (int a = 0; a < 3; a++) {
            part.lo[a] = 1e9f;
            part.hi[a] = -1e9f;
        }
        // Unskinned verts are authored in the node's own space; on this asset
        // every node is identity, but honouring the transform keeps a
        // re-export that parents something to an Empty from silently shifting.
        float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (!node->skin) cgltf_node_transform_world(node, world);

        for (cgltf_size p = 0; p < node->mesh->primitives_count; p++) {
            const cgltf_primitive& prim = node->mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            if (isMarblePrim(prim)) continue; // already a bead, not clay
            const uint32_t base = vertexCount();

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* col = nullptr;
            const cgltf_accessor* jnt = nullptr;
            const cgltf_accessor* wgt = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; a++) {
                switch (prim.attributes[a].type) {
                case cgltf_attribute_type_position: pos = prim.attributes[a].data; break;
                case cgltf_attribute_type_color:
                    if (prim.attributes[a].index == 0) col = prim.attributes[a].data;
                    break;
                case cgltf_attribute_type_joints:
                    if (prim.attributes[a].index == 0) jnt = prim.attributes[a].data;
                    break;
                case cgltf_attribute_type_weights:
                    if (prim.attributes[a].index == 0) wgt = prim.attributes[a].data;
                    break;
                default: break;
                }
            }
            if (!pos) continue;

            // Morph target POSITION deltas, applied at weight 1.0. glTF morph
            // deltas are per-vertex offsets in the SAME space as the base
            // positions, so this is a plain add before the node transform.
            const cgltf_accessor* dlt = nullptr;
            if (morph >= 0 && (cgltf_size)morph < prim.targets_count) {
                for (cgltf_size a = 0; a < prim.targets[morph].attributes_count; a++) {
                    if (prim.targets[morph].attributes[a].type ==
                        cgltf_attribute_type_position)
                        dlt = prim.targets[morph].attributes[a].data;
                }
            }
            if (morph >= 0 && !dlt) {
                std::fprintf(stderr,
                             "asset: '%s' wanted morph target %d but the primitive "
                             "has none — importing the base shape\n",
                             partName, morph);
            }

            for (cgltf_size i = 0; i < pos->count; i++) {
                float v[3];
                cgltf_accessor_read_float(pos, i, v, 3);
                if (dlt && i < dlt->count) {
                    float dv[3];
                    cgltf_accessor_read_float(dlt, i, dv, 3);
                    v[0] += dv[0]; v[1] += dv[1]; v[2] += dv[2];
                }
                float x = world[0] * v[0] + world[4] * v[1] + world[8] * v[2] + world[12];
                float y = world[1] * v[0] + world[5] * v[1] + world[9] * v[2] + world[13];
                float z = world[2] * v[0] + world[6] * v[1] + world[10] * v[2] + world[14];
                x += offset[0]; y += offset[1]; z += offset[2];
                positions.insert(positions.end(), {x, y, z});
                part.lo[0] = std::min(part.lo[0], x); part.hi[0] = std::max(part.hi[0], x);
                part.lo[1] = std::min(part.lo[1], y); part.hi[1] = std::max(part.hi[1], y);
                part.lo[2] = std::min(part.lo[2], z); part.hi[2] = std::max(part.hi[2], z);

                float c[4] = {0.5f, 0.5f, 0.5f, 1.f};
                if (col) cgltf_accessor_read_float(col, i, c, 4);
                colors.insert(colors.end(), {c[0], c[1], c[2]});

                // No skin on this asset: joints stay 0 and weights stay the
                // identity (1,0,0,0). Nothing in the pipeline reads either any
                // more — they exist so a rigged asset still imports.
                cgltf_uint j[4] = {0, 0, 0, 0};
                if (jnt) cgltf_accessor_read_uint(jnt, i, j, 4);
                joints.insert(joints.end(), {(uint16_t)j[0], (uint16_t)j[1],
                                             (uint16_t)j[2], (uint16_t)j[3]});

                float w[4] = {1.f, 0.f, 0.f, 0.f};
                if (wgt) cgltf_accessor_read_float(wgt, i, w, 4);
                weights.insert(weights.end(), {w[0], w[1], w[2], w[3]});
            }
            if (prim.indices) {
                for (cgltf_size i = 0; i < prim.indices->count; i++) {
                    indices.push_back(
                        base + (uint32_t)cgltf_accessor_read_index(prim.indices, i));
                }
            }
        }
        part.idxCount = (uint32_t)indices.size() - part.idxBegin;
        if (part.idxCount == 0) return -1;
        parts.push_back(std::move(part));
        return (int)parts.size() - 1;
    };

    static const float kNoOffset[3] = {0.f, 0.f, 0.f};
    if (namedBody || namedHand) {
        // The three brushes, in voxelization order. The hand is imported TWICE
        // from the SAME primitive: once as authored (rest) and once with the
        // "grab" morph target at full weight, translated into the empty half of
        // the volume. Discrete poses, never blended — see brick_read.wgsl.
        partBody = addPart(namedBody, "body", -1, kNoOffset);
        partHandRest = addPart(namedHand, "hand.rest", -1, kNoOffset);
        partHandGrab = addPart(namedHand, "hand.grab", 0, kGrabBrushOffset);
    } else {
        // Legacy/rigged assets: keep the old behaviour — every mesh bound to
        // the armature (or the lone unskinned mesh) merged into one body brush.
        if (bodyNodes.empty() && unskinned) bodyNodes.push_back(unskinned);
        for (const cgltf_node* bodyNode : bodyNodes) {
            const int pi = addPart(bodyNode, "body", -1, kNoOffset);
            if (partBody < 0) partBody = pi;
        }
    }
    if (parts.empty()) {
        std::fprintf(stderr, "asset: no body mesh found in %s\n", path.c_str());
        cgltf_free(data);
        return false;
    }

    std::unordered_map<const cgltf_node*, int> jointIndex;
    if (skin) {
        for (cgltf_size j = 0; j < skin->joints_count; j++) {
            jointIndex[skin->joints[j]] = (int)j;
        }
        for (cgltf_size j = 0; j < skin->joints_count; j++) {
            const cgltf_node* jn = skin->joints[j];
            AssetBone b;
            b.name = jn->name ? jn->name : "bone";
            b.parent = -1;
            matIdentity(b.pre);
            if (jn->parent) {
                auto it = jointIndex.find(jn->parent);
                if (it != jointIndex.end()) {
                    b.parent = it->second;
                } else {
                    // non-joint ancestor chain (the Armature node): its world
                    // transform pre-multiplies this root's animated local
                    cgltf_node_transform_world(jn->parent, b.pre);
                }
            }
            if (skin->inverse_bind_matrices) {
                cgltf_accessor_read_float(skin->inverse_bind_matrices, j, b.invBind, 16);
            } else {
                std::memset(b.invBind, 0, sizeof(b.invBind));
                b.invBind[0] = b.invBind[5] = b.invBind[10] = b.invBind[15] = 1.f;
            }
            cgltf_node_transform_local(jn, b.restLocal);
            bones.push_back(b);
        }
    }

    // animation clips: TRS channels targeting skin joints
    for (cgltf_size a = 0; a < data->animations_count; a++) {
        const cgltf_animation& an = data->animations[a];
        AnimClip clip;
        clip.name = an.name ? an.name : "clip";
        for (cgltf_size ch = 0; ch < an.channels_count; ch++) {
            const cgltf_animation_channel& channel = an.channels[ch];
            if (!channel.target_node || !channel.sampler) continue;
            auto it = jointIndex.find(channel.target_node);
            if (it == jointIndex.end()) continue;
            int path;
            switch (channel.target_path) {
            case cgltf_animation_path_type_translation: path = 0; break;
            case cgltf_animation_path_type_rotation: path = 1; break;
            case cgltf_animation_path_type_scale: path = 2; break;
            default: continue; // morph weights: P3
            }
            const cgltf_animation_sampler* smp = channel.sampler;
            const int comps = path == 1 ? 4 : 3;
            const bool cubic =
                smp->interpolation == cgltf_interpolation_type_cubic_spline;
            AnimTrack tr;
            tr.joint = it->second;
            tr.path = path;
            tr.interp = smp->interpolation == cgltf_interpolation_type_step ? 0 : 1;
            tr.times.resize(smp->input->count);
            for (cgltf_size k = 0; k < smp->input->count; k++) {
                cgltf_accessor_read_float(smp->input, k, &tr.times[k], 1);
            }
            tr.values.resize(smp->input->count * comps);
            for (cgltf_size k = 0; k < smp->input->count; k++) {
                // cubic output is (in-tangent, value, out-tangent) triples;
                // keep the value, sample linearly
                cgltf_size src = cubic ? k * 3 + 1 : k;
                float v[4];
                cgltf_accessor_read_float(smp->output, src, v, comps);
                std::memcpy(&tr.values[k * comps], v, comps * sizeof(float));
            }
            if (!tr.times.empty()) {
                clip.duration = std::max(clip.duration, tr.times.back());
                clip.tracks.push_back(std::move(tr));
            }
        }
        if (!clip.tracks.empty()) clips.push_back(std::move(clip));
    }

    // The artist authors ONE side and mirrors across x. That is true of the
    // hand (handled as a brush above) and of the eye, whose two primitives
    // (marble_pupil_L / marble_eye_L) arrive here as beads — so synthesise the
    // right eye by reflecting each bead's centre. Radius is unchanged: a mirror
    // is an isometry, and these are spheres.
    //
    // Guarded on there being no already-mirrored partner, so a future asset
    // that ships both eyes does not get four.
    if (!marbles.empty() && hasBrushRig()) {
        bool anyLeft = false, anyRight = false;
        for (const MarbleProp& m : marbles) {
            if (m.pos[0] > 1e-4f) anyLeft = true;
            if (m.pos[0] < -1e-4f) anyRight = true;
        }
        if (anyLeft && !anyRight) {
            const size_t n = marbles.size();
            for (size_t i = 0; i < n; i++) {
                MarbleProp m = marbles[i];
                m.pos[0] = -m.pos[0];
                marbles.push_back(m);
            }
        }
    }

    // marbles ride the nearest joint (rest-space origin distance)
    if (!bones.empty()) {
        for (MarbleProp& m : marbles) {
            float best = 1e9f;
            for (size_t j = 0; j < bones.size(); j++) {
                float inv[16];
                matInvAffine(bones[j].invBind, inv);
                float dx = m.pos[0] - inv[12], dy = m.pos[1] - inv[13],
                      dz = m.pos[2] - inv[14];
                float d = dx * dx + dy * dy + dz * dz;
                if (d < best) {
                    best = d;
                    m.bone = (int)j;
                }
            }
        }
    }

    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t v = 0; v < vertexCount(); v++) {
        for (int a = 0; a < 3; a++) {
            lo[a] = std::min(lo[a], positions[v * 3 + a]);
            hi[a] = std::max(hi[a], positions[v * 3 + a]);
        }
    }
    std::printf("asset: %s -> %u verts, %u tris, %zu bones, %zu marbles\n", path.c_str(),
                vertexCount(), triangleCount(), bones.size(), marbles.size());
    // Per-brush bounds. This is the line to read when the fighter looks wrong
    // after a re-export (the spirit of CLAUDE.md trap 7): a missing part, a
    // part with no triangles, or two parts whose boxes touch all show up here
    // before anything is rendered.
    for (const MeshPart& mp : parts) {
        std::printf("asset: brush '%s' %u tris, aabb (%.4f %.4f %.4f)..(%.4f %.4f %.4f)\n",
                    mp.name.c_str(), mp.idxCount / 3, mp.lo[0], mp.lo[1], mp.lo[2],
                    mp.hi[0], mp.hi[1], mp.hi[2]);
    }
    for (const AnimClip& c : clips) {
        std::printf("asset: clip '%s' %.2fs, %zu tracks\n", c.name.c_str(), c.duration,
                    c.tracks.size());
    }
    for (const MarbleProp& m : marbles) {
        if (m.bone >= 0) {
            std::printf("asset: marble @(%.3f %.3f %.3f) -> bone '%s'\n", m.pos[0],
                        m.pos[1], m.pos[2], bones[m.bone].name.c_str());
        }
    }
    std::printf("asset: body aabb (%.3f %.3f %.3f)..(%.3f %.3f %.3f)\n", lo[0], lo[1],
                lo[2], hi[0], hi[1], hi[2]);
    cgltf_free(data);
    return true;
}
