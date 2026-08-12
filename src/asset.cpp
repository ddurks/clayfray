#include "asset.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace {

// bounding radius of a mesh's positions around its centroid-ish origin
float meshBoundRadius(const cgltf_mesh* mesh, float scale) {
    float r2 = 0.f;
    for (cgltf_size p = 0; p < mesh->primitives_count; p++) {
        const cgltf_primitive& prim = mesh->primitives[p];
        for (cgltf_size a = 0; a < prim.attributes_count; a++) {
            if (prim.attributes[a].type != cgltf_attribute_type_position) continue;
            const cgltf_accessor* acc = prim.attributes[a].data;
            for (cgltf_size i = 0; i < acc->count; i++) {
                float v[3];
                cgltf_accessor_read_float(acc, i, v, 3);
                float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
                if (d2 > r2) r2 = d2;
            }
        }
    }
    return std::sqrt(r2) * scale;
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

    const cgltf_node* bodyNode = nullptr;
    for (cgltf_size n = 0; n < data->nodes_count; n++) {
        const cgltf_node* node = &data->nodes[n];
        if (!node->mesh) continue;
        const char* name = node->name ? node->name : "";
        if (std::strncmp(name, "marble_", 7) == 0) {
            float world[16];
            cgltf_node_transform_world(node, world);
            MarbleProp m{};
            m.pos[0] = world[12];
            m.pos[1] = world[13];
            m.pos[2] = world[14];
            float scale = std::sqrt(world[0] * world[0] + world[1] * world[1] +
                                    world[2] * world[2]);
            m.radius = meshBoundRadius(node->mesh, scale);
            m.color[0] = 0.8f; m.color[1] = 0.8f; m.color[2] = 0.8f;
            if (node->mesh->primitives_count > 0 &&
                node->mesh->primitives[0].material &&
                node->mesh->primitives[0].material->has_pbr_metallic_roughness) {
                const float* bc = node->mesh->primitives[0]
                                      .material->pbr_metallic_roughness.base_color_factor;
                m.color[0] = bc[0]; m.color[1] = bc[1]; m.color[2] = bc[2];
            }
            marbles.push_back(m);
        } else if (node->skin || !bodyNode) {
            if (node->skin || (bodyNode && !bodyNode->skin)) bodyNode = node;
            if (!bodyNode) bodyNode = node;
        }
    }
    if (!bodyNode || !bodyNode->mesh) {
        std::fprintf(stderr, "asset: no body mesh found in %s\n", path.c_str());
        cgltf_free(data);
        return false;
    }

    // skinned vertices are in armature space per glTF; non-skinned get node world
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!bodyNode->skin) cgltf_node_transform_world(bodyNode, world);

    for (cgltf_size p = 0; p < bodyNode->mesh->primitives_count; p++) {
        const cgltf_primitive& prim = bodyNode->mesh->primitives[p];
        if (prim.type != cgltf_primitive_type_triangles) continue;
        uint32_t base = vertexCount();

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

        for (cgltf_size i = 0; i < pos->count; i++) {
            float v[3];
            cgltf_accessor_read_float(pos, i, v, 3);
            float x = world[0] * v[0] + world[4] * v[1] + world[8] * v[2] + world[12];
            float y = world[1] * v[0] + world[5] * v[1] + world[9] * v[2] + world[13];
            float z = world[2] * v[0] + world[6] * v[1] + world[10] * v[2] + world[14];
            positions.insert(positions.end(), {x, y, z});

            float c[4] = {0.5f, 0.5f, 0.5f, 1.f};
            if (col) cgltf_accessor_read_float(col, i, c, 4);
            colors.insert(colors.end(), {c[0], c[1], c[2]});

            cgltf_uint j[4] = {0, 0, 0, 0};
            if (jnt) cgltf_accessor_read_uint(jnt, i, j, 4);
            joints.insert(joints.end(), {(uint16_t)j[0], (uint16_t)j[1], (uint16_t)j[2],
                                         (uint16_t)j[3]});

            float w[4] = {1.f, 0.f, 0.f, 0.f};
            if (wgt) cgltf_accessor_read_float(wgt, i, w, 4);
            weights.insert(weights.end(), {w[0], w[1], w[2], w[3]});
        }
        if (prim.indices) {
            for (cgltf_size i = 0; i < prim.indices->count; i++) {
                indices.push_back(base +
                                  (uint32_t)cgltf_accessor_read_index(prim.indices, i));
            }
        }
    }

    if (bodyNode->skin) {
        const cgltf_skin* skin = bodyNode->skin;
        std::unordered_map<const cgltf_node*, int> jointIndex;
        for (cgltf_size j = 0; j < skin->joints_count; j++) {
            jointIndex[skin->joints[j]] = (int)j;
        }
        for (cgltf_size j = 0; j < skin->joints_count; j++) {
            const cgltf_node* jn = skin->joints[j];
            AssetBone b;
            b.name = jn->name ? jn->name : "bone";
            b.parent = -1;
            if (jn->parent) {
                auto it = jointIndex.find(jn->parent);
                if (it != jointIndex.end()) b.parent = it->second;
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

    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t v = 0; v < vertexCount(); v++) {
        for (int a = 0; a < 3; a++) {
            lo[a] = std::min(lo[a], positions[v * 3 + a]);
            hi[a] = std::max(hi[a], positions[v * 3 + a]);
        }
    }
    std::printf("asset: %s -> %u verts, %u tris, %zu bones, %zu marbles\n", path.c_str(),
                vertexCount(), triangleCount(), bones.size(), marbles.size());
    std::printf("asset: body aabb (%.3f %.3f %.3f)..(%.3f %.3f %.3f)\n", lo[0], lo[1],
                lo[2], hi[0], hi[1], hi[2]);
    cgltf_free(data);
    return true;
}
