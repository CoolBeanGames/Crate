#include "assets/FbxImport.h"
#include "core/Log.h"

#include "ufbx/ufbx.h"

#include <filesystem>
#include <vector>

namespace crate {
namespace fs = std::filesystem;

static Vec3 toVec3(ufbx_vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

// Pick the first path that exists on disk, else the first non-empty one.
static std::string resolveTexture(const ufbx_texture* tex, const fs::path& fbxDir) {
    if (!tex)
        return {};
    const char* candidates[] = {tex->filename.data, tex->absolute_filename.data,
                                tex->relative_filename.data};
    std::string firstNonEmpty;
    for (const char* c : candidates) {
        if (!c || !*c)
            continue;
        if (firstNonEmpty.empty())
            firstNonEmpty = c;
        std::error_code ec;
        if (fs::exists(c, ec))
            return c;
        fs::path rel = fbxDir / fs::path(c).filename();
        if (fs::exists(rel, ec))
            return rel.string();
    }
    return firstNonEmpty;
}

FbxImportResult importFbx(const std::string& path, MeshLibrary& lib) {
    FbxImportResult r;
    r.key = path;

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_left_handed_y_up; // match the engine's LH space
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
    opts.generate_missing_normals = true;

    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &err);
    if (!scene) {
        r.error = err.description.data ? err.description.data : "unknown ufbx error";
        CR_ERROR("assets", "FBX import failed: " + r.error);
        return r;
    }

    MeshData merged;
    const fs::path fbxDir = fs::path(path).parent_path();

    for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
        ufbx_node* node = scene->nodes.data[ni];
        if (!node->mesh)
            continue;
        ufbx_mesh* mesh = node->mesh;
        ++r.meshNodes;

        std::vector<uint32_t> tri(mesh->max_face_triangles * 3);
        for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
            ufbx_face face = mesh->faces.data[fi];
            uint32_t nTris = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
            for (uint32_t t = 0; t < nTris * 3; ++t) {
                uint32_t ix = tri[t];
                ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
                ufbx_vec3 n = mesh->vertex_normal.exists
                                  ? ufbx_get_vertex_vec3(&mesh->vertex_normal, ix)
                                  : ufbx_vec3{0, 1, 0};
                ufbx_vec2 uv = mesh->vertex_uv.exists
                                   ? ufbx_get_vertex_vec2(&mesh->vertex_uv, ix)
                                   : ufbx_vec2{0, 0};
                p = ufbx_transform_position(&node->geometry_to_world, p);
                n = ufbx_transform_direction(&node->geometry_to_world, n);

                Vertex v;
                v.position = toVec3(p);
                v.normal = normalize(toVec3(n));
                v.u = static_cast<float>(uv.x);
                v.v = 1.0f - static_cast<float>(uv.y);
                merged.indices.push_back(static_cast<uint32_t>(merged.vertices.size()));
                merged.vertices.push_back(v);
                ++r.triangles;
            }
        }
    }
    r.triangles /= 3;

    std::vector<Material> mats;
    for (size_t mi = 0; mi < scene->materials.count; ++mi) {
        ufbx_material* m = scene->materials.data[mi];
        Material mat;
        mat.name = m->name.length ? std::string(m->name.data, m->name.length) : "Material";

        const ufbx_material_map* colorMap =
            m->pbr.base_color.has_value ? &m->pbr.base_color : &m->fbx.diffuse_color;
        ufbx_vec4 c = colorMap->value_vec4;
        if (colorMap->has_value) {
            mat.baseColor[0] = static_cast<float>(c.x);
            mat.baseColor[1] = static_cast<float>(c.y);
            mat.baseColor[2] = static_cast<float>(c.z);
            mat.baseColor[3] = c.w > 0.0f ? static_cast<float>(c.w) : 1.0f;
        }
        const ufbx_texture* tex = m->pbr.base_color.texture ? m->pbr.base_color.texture
                                                            : m->fbx.diffuse_color.texture;
        mat.texturePath = resolveTexture(tex, fbxDir);
        mats.push_back(std::move(mat));
    }
    r.materials = static_cast<int>(mats.size());

    ufbx_free_scene(scene);

    if (merged.vertices.empty()) {
        r.error = "no mesh geometry found";
        CR_WARN("assets", "FBX '" + path + "' had no meshes");
        return r;
    }

    lib.addMesh(path, std::move(merged));
    lib.setMaterials(path, std::move(mats));
    r.ok = true;
    CR_LOG("assets", "Imported FBX " + fs::path(path).filename().string() + " (" +
                         std::to_string(r.meshNodes) + " mesh nodes, " +
                         std::to_string(r.triangles) + " tris, " +
                         std::to_string(r.materials) + " materials)");
    return r;
}

} // namespace crate
