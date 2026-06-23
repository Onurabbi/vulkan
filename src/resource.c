#include "resource.h"
#include "common.h"
#include "platform.h"
#include "string_utils.h"
#include "log.h"

#include "vulkan/vulkan.h"

#define FAST_OBJ_IMPLEMENTATION
#include <fast_obj.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

#include <meshoptimizer/src/meshoptimizer.h>

#include <SDL3/SDL_filesystem.h>

#define INVALID_TEXTURE ~(0U)

static resource_system_t *gResourceSystem;

typedef struct {
    u32 meshOffset;
    u32 meshCount;
} mesh_primitive_t;

typedef struct {
    char **glob;
    memory_arena_t *scratchArena;
    memory_arena_t *permanentArena;
} glob_data_t;

static const char *GetSubDirFromType(resource_type_t type)
{
    const char *dir = NULL;
    if (type == RESOURCE_TYPE_SHADER) {
        dir = "shaders/";
    } else if (type == RESOURCE_TYPE_MESH) {
        dir = "meshes/";
    } else if (type == RESOURCE_TYPE_TEXTURE) {
        dir = "textures/";
    } else {
        LOGF("Unknown resource type");
        return NULL;
    }
    return dir;
}

static const char *GetFullPathFromUri(const char *uri, resource_type_t type, memory_arena_t *arena)
{
    const char *dir = GetSubDirFromType(type);
    return StringIntern(ArenaPrintf(arena, "%s%s%s", gResourceSystem->assetDir, dir, uri));
}

static void LoadTexture(resource_t *textureResource, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    textureResource->type = RESOURCE_TYPE_TEXTURE;
    (void)scratchArena;
    (void)permanentArena;

    textureResource->texture.textureIndex = gResourceSystem->textureCount - 1;
    VulkanLoadTexture(textureResource, textureResource->path);
}

static void AppendMesh(vertex_t *vertices, u32 *indices, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    u32 *remap = NULL;
    ArrayInitWithArena(remap, scratchArena, ArrayCount(vertices));
    ArrayResize(remap, ArrayCount(vertices));
    size_t uniqueVertices = meshopt_generateVertexRemap(remap, indices, ArrayCount(indices), vertices, ArrayCount(vertices), sizeof(vertex_t));
    meshopt_remapVertexBuffer(vertices, vertices, ArrayCount(vertices), sizeof(vertex_t), remap);

    meshopt_remapIndexBuffer(indices, indices, ArrayCount(indices), remap);
    ArrayResize(vertices, uniqueVertices);

    meshopt_optimizeVertexCache(indices, indices, ArrayCount(indices), ArrayCount(vertices));
    meshopt_optimizeVertexFetch(vertices, indices, ArrayCount(indices), vertices, ArrayCount(vertices), sizeof(vertex_t));

    mesh_t mesh = {0};
    mesh.vertexOffset = ArrayCount(gResourceSystem->vertices);
    mesh.vertexCount = ArrayCount(vertices);

    ArrayPushArray(gResourceSystem->vertices, vertices, ArrayCount(vertices));

    vec3_t *positions = NULL;
    ArrayInitWithArena(positions, scratchArena, ArrayCount(vertices));
    for (u32 i = 0; i < ArrayCount(vertices); i++) {
        vec3_t pos = (vec3_t){ meshopt_dequantizeHalf(vertices[i].vx), meshopt_dequantizeHalf(vertices[i].vy), meshopt_dequantizeHalf(vertices[i].vz) };  
        ArrayPush(positions, pos);
    }

    vec3_t *normals = NULL;
    ArrayInitWithArena(normals, scratchArena, ArrayCount(vertices));
    for (u32 i = 0; i < ArrayCount(vertices); i++) {
        vec3_t normal = (vec3_t){ vertices[i].nx / 127.0f - 1.0f, vertices[i].ny / 127.0f - 1.0f, vertices[i].nz / 127.0f - 1.0f };
        ArrayPush(normals, normal);
    }

    vec3_t center =  {0};
    for (u32 i = 0; i < ArrayCount(positions); i++) {
        HMM_Add(center, positions[i]);
    }
    HMM_Div(center, (f32)ArrayCount(positions));

    f32 radius = 0.0f;
    for (u32 i = 0; i < ArrayCount(positions); i++) {
        radius = MAX(radius, HMM_Len(HMM_Sub(positions[i], center)));
    }

    mesh.center = center;
    mesh.radius = radius;
    
    f32 lodScale = meshopt_simplifyScale(&positions[0].X, ArrayCount(vertices), sizeof(vec3_t));

    u32 *lodIndices = indices;

    f32 lodError = 0.0f;
    f32 normalWeights[3] = { 1.0f, 1.0f, 1.0f };

    while (mesh.lodCount < ARRAY_SIZE(mesh.meshLods)) {
        mesh_lod_t *lod = &mesh.meshLods[mesh.lodCount++];

        lod->indexOffset = ArrayCount(gResourceSystem->indices);
        lod->indexCount  = ArrayCount(lodIndices);

        ArrayPushArray(gResourceSystem->indices, lodIndices, lod->indexCount);
        lod->error = lodError * lodScale;

        if (mesh.lodCount < ARRAY_SIZE(mesh.meshLods)) {
            const f32 maxError = 1e-1f;
            const u32 options = meshopt_SimplifySparse;

            size_t nextIndicesTarget = ((size_t)((double)(ArrayCount(lodIndices)) * 0.6) / 3) * 3;
            f32 nextError = 0.0f;

            size_t nextIndices = meshopt_simplifyWithAttributes(lodIndices, lodIndices, ArrayCount(lodIndices), &positions[0].X,
                ArrayCount(vertices), sizeof(vec3_t), &normals[0].X, sizeof(vec3_t), normalWeights, 3, NULL, nextIndicesTarget, maxError, options, &nextError);

            LV_ASSERT(nextIndices <= ArrayCount(lodIndices));

            if (nextIndices == ArrayCount(lodIndices) || nextIndices == 0) {
                break;
            }

            if (nextIndices >= (size_t)((double)(ArrayCount(lodIndices)) * 0.85)) {
                break;
            }

            ArrayResize(lodIndices, nextIndices);
            lodError = MAX(lodError * 1.5f, nextError);

            meshopt_optimizeVertexCache(lodIndices, lodIndices, ArrayCount(lodIndices), ArrayCount(vertices));
        }
    }

    ArrayPush(gResourceSystem->meshes, mesh);
}

// TODO: Move this to its own file
static void LoadMesh(resource_t *meshResource, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    meshResource->type = RESOURCE_TYPE_MESH;

    vertex_t *vertices = NULL;
    u32 *indices = NULL;

    fastObjMesh *obj = fast_obj_read(meshResource->path);
    if (obj) {
        u32 indexCount = 0;
        for (u32 i = 0; i < obj->face_count; i++) {
            indexCount += 3 * (obj->face_vertices[i] - 2);
        }

        ArrayInitWithArena(vertices, scratchArena, indexCount);
        ArrayResize(vertices, indexCount);

        ArrayInitWithArena(indices, scratchArena, indexCount);
        ArrayResize(indices, indexCount);
        for (u32 i = 0; i < indexCount; i++) {
            indices[i] = i;
        }

        u32 vertex_offset = 0;
        u32 index_offset = 0;

        for (u32 i = 0; i < obj->face_count; i++) {
            for (u32 j = 0; j < obj->face_vertices[i]; j++) {
                fastObjIndex gi = obj->indices[index_offset + j];
                if (j >= 3) {
                    vertices[vertex_offset + 0] = vertices[vertex_offset - 3];
                    vertices[vertex_offset + 1] = vertices[vertex_offset - 1];
                    vertex_offset += 2;
                }

                vertex_t *v = &vertices[vertex_offset++];
            }
            index_offset += obj->face_vertices[i];
        }
        LV_ASSERT(vertex_offset == indexCount);
    }

    AppendMesh(vertices, indices, scratchArena, permanentArena);

    meshResource->mesh.firstMeshIndex = ArrayCount(gResourceSystem->meshes) - 1;
    meshResource->mesh.meshCount      = 1;

    fast_obj_destroy(obj);
}

static void LoadVertices(vertex_t *vertices, const cgltf_primitive *prim, memory_arena_t *arena)
{
    size_t vertexCount = ArrayCount(vertices);
    f32 *scratch = NULL;
    ArrayInitWithArena(scratch, arena, vertexCount * 4);
    ArrayResize(scratch, vertexCount * 4);

    const cgltf_accessor *pos = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0);
    if (pos) {
        LV_ASSERT(cgltf_num_components(pos->type) == 3);
        cgltf_accessor_unpack_floats(pos, scratch, vertexCount * 3);

        for (u32 j = 0; j < vertexCount; j++) {
            vertices[j].vx = meshopt_quantizeHalf(scratch[j * 3 + 0]);
            vertices[j].vy = meshopt_quantizeHalf(scratch[j * 3 + 1]);
            vertices[j].vz = meshopt_quantizeHalf(scratch[j * 3 + 2]);
        }
    }

    const cgltf_accessor *nrm = cgltf_find_accessor(prim, cgltf_attribute_type_normal, 0);
    if (nrm) {
        LV_ASSERT(cgltf_num_components(nrm->type) == 3);
        cgltf_accessor_unpack_floats(nrm, scratch, vertexCount * 3);

        for (u32 j = 0; j < vertexCount; j++) {
            vertices[j].nx = (u8)(scratch[j * 3 + 0] * 127.0f + 127.5f);
            vertices[j].ny = (u8)(scratch[j * 3 + 1] * 127.0f + 127.5f);
            vertices[j].nz = (u8)(scratch[j * 3 + 2] * 127.0f + 127.5f);
        }
    }

    const cgltf_accessor *tan = cgltf_find_accessor(prim, cgltf_attribute_type_tangent, 0);
    if (tan) {
        LV_ASSERT(cgltf_num_components(tan->type) == 4);
        cgltf_accessor_unpack_floats(tan, scratch, vertexCount * 4);
        for (u32 j = 0; j < vertexCount; j++) {
            vertices[j].tx = (u8)(scratch[j * 4 + 0] * 127.0f + 127.5f);
            vertices[j].ty = (u8)(scratch[j * 4 + 1] * 127.0f + 127.5f);
            vertices[j].tz = (u8)(scratch[j * 4 + 2] * 127.0f + 127.5f);
            vertices[j].tw = (u8)(scratch[j * 4 + 3] * 127.0f + 127.5f);
        }
    }

    const cgltf_accessor *tex = cgltf_find_accessor(prim, cgltf_attribute_type_texcoord, 0);
    if (tex) {
        LV_ASSERT(cgltf_num_components(tex->type) == 2);
        cgltf_accessor_unpack_floats(tex, scratch, vertexCount * 2);

        for (u32 j = 0; j < vertexCount; j++) {
            vertices[j].u = meshopt_quantizeHalf(scratch[j * 2 + 0]);
            vertices[j].v = meshopt_quantizeHalf(scratch[j * 2 + 1]);
        }
    }
}

static void decomposeTransform(f32 translation[3], f32 rotation[4], f32 scale[3], const f32* transform)
{
	f32 m[4][4] = {};
	memcpy(m, transform, 16 * sizeof(f32));

	// extract translation from last row
	translation[0] = m[3][0];
	translation[1] = m[3][1];
	translation[2] = m[3][2];

	// compute determinant to determine handedness
	f32 det =
	    m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2]) -
	    m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
	    m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

	f32 sign = (det < 0.f) ? -1.f : 1.f;

	// recover scale from axis lengths
	scale[0] = sqrtf(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]) * sign;
	scale[1] = sqrtf(m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2]) * sign;
	scale[2] = sqrtf(m[2][0] * m[2][0] + m[2][1] * m[2][1] + m[2][2] * m[2][2]) * sign;

	// normalize axes to get a pure rotation matrix
	f32 rsx = (scale[0] == 0.f) ? 0.f : 1.f / scale[0];
	f32 rsy = (scale[1] == 0.f) ? 0.f : 1.f / scale[1];
	f32 rsz = (scale[2] == 0.f) ? 0.f : 1.f / scale[2];

	f32 r00 = m[0][0] * rsx, r10 = m[1][0] * rsy, r20 = m[2][0] * rsz;
	f32 r01 = m[0][1] * rsx, r11 = m[1][1] * rsy, r21 = m[2][1] * rsz;
	f32 r02 = m[0][2] * rsx, r12 = m[1][2] * rsy, r22 = m[2][2] * rsz;

	// "branchless" version of Mike Day's matrix to quaternion conversion
	int qc = r22 < 0 ? (r00 > r11 ? 0 : 1) : (r00 < -r11 ? 2 : 3);
	f32 qs1 = qc & 2 ? -1.f : 1.f;
	f32 qs2 = qc & 1 ? -1.f : 1.f;
	f32 qs3 = (qc - 1) & 2 ? -1.f : 1.f;

	f32 qt = 1.f - qs3 * r00 - qs2 * r11 - qs1 * r22;
	f32 qs = 0.5f / sqrtf(qt);

	rotation[qc ^ 0] = qs * qt;
	rotation[qc ^ 1] = qs * (r01 + qs1 * r10);
	rotation[qc ^ 2] = qs * (r20 + qs2 * r02);
	rotation[qc ^ 3] = qs * (r12 + qs3 * r21);
}

//TODO: Is this the right place to do this?
static const char *ReplaceURIWithDDS(const char *uri, memory_arena_t *scratchArena)
{
    if (!uri) {
        return NULL;
    }
    u32 uriLength = (u32)strlen(uri);
    char *dst = PushArray(scratchArena, uriLength + 1, char);
    const char *result = dst;
    const char *src = uri;
    while (*src && *src != '.') {
        *dst++ = *src++;
    }
    *dst++ = '.';
    *dst++ = 'd';
    *dst++ = 'd';
    *dst++ = 's';
    return result;
}

static u32 GetTextureIndexFromTextureView(cgltf_texture_view *view)
{
    const char *uri = NULL;
    //check what the extension is
    const char *extension = StringUtilsGetExtensionFromPath(view->texture->image->uri);
    //check if it's png
    if (strncmp(extension, "dds", 3) == 0) {
        uri = view->texture->image->uri;
    } else {
        if (strncmp(extension, "png", 3) == 0 ||
            strncmp(extension, "jpg", 3) == 0) {
            uri = ReplaceURIWithDDS(view->texture->image->uri, ScratchArena(0));
        } else {
            LV_ASSERT(false && "Unknown texture extension");
        }
    }

    u32 result = -1;
    resource_t *res = ResourceSystemGetResource(uri);
    if (res) {
        result = res->texture.textureIndex;
    }
    return result;
}

static void LoadScene(resource_t *sceneResource, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    sceneResource->type = RESOURCE_TYPE_MESH;

    cgltf_options options = {0};
    cgltf_data *data = NULL;
    cgltf_result res = cgltf_parse_file(&options, sceneResource->path, &data);
    if (res != cgltf_result_success) {
        LOGE("Unable to load scene resource: %s", sceneResource->path);
        return;
    }

    res = cgltf_load_buffers(&options, data, sceneResource->path);
    if (res != cgltf_result_success) {
        LOGE("Unable to load buffers for scene resource: %s", sceneResource->path);
        goto exit;
    }

    res = cgltf_validate(data);
    if (res != cgltf_result_success) {
        LOGE("Unable to validate scene resource: %s", sceneResource->path);
        goto exit;
    }

    mesh_primitive_t *primitives = NULL;
    ArrayInitWithArena(primitives, scratchArena, data->meshes_count);

    size_t primitiveMaterialCount = 0;
    for (u32 i = 0; i < data->meshes_count; i++) {
        primitiveMaterialCount += data->meshes[i].primitives_count;
    }

    cgltf_material **primitiveMaterials = NULL;
    ArrayInitWithArena(primitiveMaterials, scratchArena, primitiveMaterialCount);

    size_t firstMeshOffset = ArrayCount(gResourceSystem->meshes);

    for (u32 i = 0; i < data->meshes_count; i++) {
        const cgltf_mesh *mesh = &data->meshes[i];
        size_t meshOffset = ArrayCount(gResourceSystem->meshes);

        for (u32 j = 0; j < mesh->primitives_count; j++) {
            const cgltf_primitive *prim = &mesh->primitives[j];
            if (prim->type != cgltf_primitive_type_triangles || prim->indices == NULL){
                continue;
            }

            vertex_t *vertices = NULL;
            ArrayInitWithArena(vertices, scratchArena, prim->attributes[0].data->count);
            ArrayResize(vertices, prim->attributes[0].data->count);
            LoadVertices(vertices, prim, scratchArena);

            u32 *indices = NULL;
            ArrayInitWithArena(indices, scratchArena, prim->indices->count);
            ArrayResize(indices, prim->indices->count);
            cgltf_accessor_unpack_indices(prim->indices, indices, 4, ArrayCount(indices));

            AppendMesh(vertices, indices, scratchArena, permanentArena);
            ArrayPush(primitiveMaterials, prim->material);
        }

        mesh_primitive_t primitive = {0};
        primitive.meshOffset = meshOffset;
        primitive.meshCount  = ArrayCount(gResourceSystem->meshes) - meshOffset;
        ArrayPush(primitives, primitive);
    }

    LV_ASSERT(ArrayCount(primitiveMaterials) + firstMeshOffset == ArrayCount(gResourceSystem->meshes));

    i32 *nodeDraws = NULL;
    ArrayInitWithArena(nodeDraws, scratchArena, data->nodes_count);
    ArrayResize(nodeDraws, data->nodes_count);
    for (u32 i = 0; i < ArrayCount(nodeDraws); i++) {
        nodeDraws[i] = -1;
    }

    size_t materialOffset = ArrayCount(gResourceSystem->materials);

    for (u32 i = 0; i < data->nodes_count; i++) {
        const cgltf_node *node = &data->nodes[i];

        if (node->mesh) {
            f32 matrix[16];
            cgltf_node_transform_world(node, matrix);

            f32 translation[3];
            f32 rotation[4];
            f32 scale[3];
            decomposeTransform(translation, rotation, scale, matrix);

            mesh_primitive_t range = primitives[cgltf_mesh_index(data, node->mesh)];

            for (u32 j = 0; j < range.meshCount; j++) {
                draw_data_t draw    = {0};
                draw.position    = (vec3_t){ translation[0], translation[1], translation[2] };
                draw.scale       = MAX(scale[0], MAX(scale[1], scale[2]));
                draw.rx          = rotation[0];
                draw.ry          = rotation[1];
                draw.rz          = rotation[2];
                draw.rw          = rotation[3];
                draw.meshIndex   = range.meshOffset + j;

                cgltf_material *material = primitiveMaterials[range.meshOffset + j - firstMeshOffset];
                if (!material) {
                    LOGE("No material associated with this mesh");
                }
                draw.materialIndex = material ? materialOffset +  (i32)(cgltf_material_index(data, material)) : 0;
                LV_ASSERT(draw.materialIndex >= 0 && draw.materialIndex < data->materials_count);
                if (material && material->alpha_mode != cgltf_alpha_mode_opaque) {
                    draw.postPass = 1;
                }

                if (material && material->has_transmission) {
                    draw.postPass = 2;
                }

                nodeDraws[i] = (i32)ArrayCount(gResourceSystem->meshDraws);
                ArrayPush(gResourceSystem->meshDraws, draw);
            }
        }
    }

    for (u32 i = 0; i < data->materials_count; i++) {
        cgltf_material *material = &data->materials[i];

        material_t mat = {0};
        mat.diffuseFactor[0] = 1;
        mat.diffuseFactor[1] = 1;
        mat.diffuseFactor[2] = 1;
        mat.diffuseFactor[3] = 1;
        mat.specularFactor[0] = 1;
        mat.specularFactor[1] = 1;
        mat.specularFactor[2] = 1;
        mat.specularFactor[3] = 1;
        mat.emissiveFactor[0] = 1;
        mat.emissiveFactor[1] = 1;
        mat.emissiveFactor[2] = 1;
        mat.albedo   = 0;
        mat.emissive = 0;
        mat.normal   = 0;

        if (material->has_pbr_specular_glossiness) {
            if  (material->pbr_specular_glossiness.diffuse_texture.texture) {
                mat.albedo = GetTextureIndexFromTextureView(&material->pbr_specular_glossiness.diffuse_texture);
            }
            LV_ASSERT(mat.albedo < gResourceSystem->textureCount);
            float *diffuseFactor = material->pbr_specular_glossiness.diffuse_factor;
            memcpy(mat.diffuseFactor, diffuseFactor, sizeof(mat.diffuseFactor));

            if (material->pbr_specular_glossiness.specular_glossiness_texture.texture) {
                mat.specular = GetTextureIndexFromTextureView(&material->pbr_specular_glossiness.specular_glossiness_texture);
            }
            float *specularFactor = material->pbr_specular_glossiness.specular_factor;
            memcpy(mat.specularFactor, specularFactor, sizeof(mat.specularFactor));
        } else if (material->has_pbr_metallic_roughness) {
            if (material->pbr_metallic_roughness.base_color_texture.texture) {
                mat.albedo = GetTextureIndexFromTextureView(&material->pbr_metallic_roughness.base_color_texture);
            }
            LV_ASSERT(mat.albedo < gResourceSystem->textureCount);
            float *baseColorFactor = material->pbr_metallic_roughness.base_color_factor;
            memcpy(mat.diffuseFactor, baseColorFactor, sizeof(mat.diffuseFactor)); 

            if (material->pbr_metallic_roughness.metallic_roughness_texture.texture) {
                mat.specular = GetTextureIndexFromTextureView(&material->pbr_metallic_roughness.metallic_roughness_texture);
            }

            mat.specularFactor[0] = 1;
            mat.specularFactor[1] = 1;
            mat.specularFactor[2] = 1;
            mat.specularFactor[3] = 1 - material->pbr_metallic_roughness.roughness_factor;
        }

        if (material->normal_texture.texture) {
            mat.normal = GetTextureIndexFromTextureView(&material->normal_texture);
        }

        if (material->emissive_texture.texture) {
            mat.emissive = GetTextureIndexFromTextureView(&material->emissive_texture);
        }
        mat.emissiveFactor[0] = material->emissive_factor[0];
        mat.emissiveFactor[1] = material->emissive_factor[1];
        mat.emissiveFactor[2] = material->emissive_factor[2];

        ArrayPush(gResourceSystem->materials, mat);
    }

    cgltf_animation_sampler** samplersT = NULL;
    ArrayInitWithArena(samplersT, scratchArena, data->nodes_count);
    ArrayResize(samplersT, data->nodes_count);

    cgltf_animation_sampler** samplersR = NULL;
    ArrayInitWithArena(samplersR, scratchArena, data->nodes_count);
    ArrayResize(samplersR, data->nodes_count);

    cgltf_animation_sampler** samplersS = NULL;
    ArrayInitWithArena(samplersS, scratchArena, data->nodes_count);
    ArrayResize(samplersS, data->nodes_count);

    for (u32 i = 0; i < data->animations_count; i++) {
        cgltf_animation *anim = &data->animations[i];
        
        for (u32 j = 0; j < anim->channels_count; j++) {
            cgltf_animation_channel *channel = &anim->channels[j];
            cgltf_animation_sampler *sampler = channel->sampler;

            if (!channel->target_node) {
                continue;
            }

            if (channel->target_path == cgltf_animation_path_type_translation) {
                samplersT[cgltf_node_index(data, channel->target_node)] = sampler;
            } else if (channel->target_path == cgltf_animation_path_type_rotation) {
                samplersR[cgltf_node_index(data, channel->target_node)] = sampler;
            } else if (channel->target_path == cgltf_animation_path_type_scale) {
                samplersS[cgltf_node_index(data, channel->target_node)] = sampler;
            }
        }
    }

    for (u32 i = 0; i < data->nodes_count; i++) {
        if (!samplersR[i] && !samplersT[i] && !samplersS[i]) {
            continue;
        }

        if (nodeDraws[i] == -1) {
            LOGW("Warning: skipping animation for node %u without draw", i);
            continue;
        }

        cgltf_accessor *input = 0;
        if (samplersT[i]) {
            input = samplersT[i]->input;
        } else if (samplersR[i]) {
            input = samplersR[i]->input;
        } else if (samplersS[i]) {
            input = samplersS[i]->input;
        }

        if ((samplersT[i] && samplersT[i]->input->count != input->count) ||
            (samplersR[i] && samplersR[i]->input->count != input->count) ||
            (samplersS[i] && samplersS[i]->input->count != input->count)) {
            LOGW("Warning: skipping animation for node %u due to mismatched sampler counts", i);
            continue;
        }

		if ((samplersT[i] && samplersT[i]->interpolation != cgltf_interpolation_type_linear) ||
		    (samplersR[i] && samplersR[i]->interpolation != cgltf_interpolation_type_linear) ||
		    (samplersS[i] && samplersS[i]->interpolation != cgltf_interpolation_type_linear))
		{
			LOGW("Warning: skipping animation for node %u due to mismatched sampler counts", i);
			continue;
		}

        if (input->count < 2) {
			LOGW("Warning: skipping animation for node %u with %d keyframes", i, (u32)input->count);
            continue;
        }

        f32 *times = NULL;
        ArrayInitWithArena(times, scratchArena, input->count);
        ArrayResize(times, input->count);
        cgltf_accessor_unpack_floats(input, times, ArrayCount(times));

        animation_t animation = {0};
        animation.drawIndex = nodeDraws[i];
        animation.startTime = times[0];
        animation.period = times[1] - times[0];
        ArrayInitWithArena(animation.keyframes, permanentArena, input->count);
    
        f32 *valuesR, *valuesT, *valuesS;

        if (samplersT[i]) {
            ArrayInitWithArena(valuesT, scratchArena, samplersT[i]->output->count * 3);
            ArrayResize(valuesT, samplersT[i]->output->count * 3);
            cgltf_accessor_unpack_floats(samplersT[i]->output, valuesT, ArrayCount(valuesT));
        }

        if (samplersR[i]) {
            ArrayInitWithArena(valuesR, scratchArena, samplersR[i]->output->count * 4);
            ArrayResize(valuesR, samplersR[i]->output->count * 4);
            cgltf_accessor_unpack_floats(samplersR[i]->output, valuesR, ArrayCount(valuesR));
        }

        if (samplersS[i]) {
            ArrayInitWithArena(valuesS, scratchArena, samplersS[i]->output->count * 3);
            ArrayResize(valuesS, samplersS[i]->output->count * 3);
            cgltf_accessor_unpack_floats(samplersS[i]->output, valuesS, ArrayCount(valuesS));
        }

        cgltf_node nodeCopy = data->nodes[i];

        for (u32 j = 0; j < input->count; j++) {
            if (samplersT[i]) {
                memcpy(nodeCopy.translation, &valuesT[j * 3], 3 * sizeof(f32));
            }
            
            if (samplersR[i]) {
                memcpy(nodeCopy.rotation, &valuesR[j * 4], 4 * sizeof(f32));
            }

            if (samplersS[i]) {
                memcpy(nodeCopy.scale, &valuesS[j * 3], 3 * sizeof(f32));
            }
            
            f32 matrix[16];
            cgltf_node_transform_world(&nodeCopy, matrix);

            f32 translation[3];
            f32 rotation[4];
            f32 scale[3];
            decomposeTransform(translation, rotation, scale, matrix);

            keyframe_t kf = {0};
            kf.translation = (vec3_t){ translation[0], translation[1], translation[2] };
            kf.rotation = (quat_t){ rotation[0], rotation[1], rotation[2], rotation[3] };
            kf.scale = MAX(scale[0], MAX(scale[1], scale[2]));

            ArrayPush(animation.keyframes, kf);
        }

        ArrayPush(gResourceSystem->animations, animation);
    }
exit:
    cgltf_free(data);
}

static void LoadShader(resource_t *shaderResource, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    shaderResource->type = RESOURCE_TYPE_SHADER;
    VulkanLoadShader(shaderResource, shaderResource->path, scratchArena, permanentArena);
}

static resource_type_t GetResourceType(const char *fName)
{
    const char *extension = StringUtilsGetExtensionFromPath(fName);
    if (*extension == '\0') {
        //It's a directory
        return RESOURCE_TYPE_INVALID;
    }

    if (strncmp(extension, "spv", 3) == 0) {
        return RESOURCE_TYPE_SHADER;
    } else if (strncmp(extension, "dds", 3) == 0) {
        return RESOURCE_TYPE_TEXTURE;
    } else if (strncmp(extension, "gltf", 4) == 0) {
        return RESOURCE_TYPE_MESH;
    }
    return RESOURCE_TYPE_INVALID;
}

static void ResourceSystemLoadResource(resource_t *resources, const char *uri, resource_type_t type)
{
    resource_t resource = {0};
    resource.type = type;
    resource.path = GetFullPathFromUri(uri, type, ScratchArena(0));

    void (*LoadFn)(resource_t *, memory_arena_t *, memory_arena_t *);

    switch(resource.type) {
        case RESOURCE_TYPE_MESH:
            gResourceSystem->meshCount++;
            LoadFn = LoadScene;
            break;
        case RESOURCE_TYPE_SHADER:
            gResourceSystem->shaderCount++;
            LoadFn = LoadShader;
            break;
        case RESOURCE_TYPE_TEXTURE:
            gResourceSystem->textureCount++;
            LoadFn = LoadTexture;
            break;
        default:
            LV_ASSERT(false);
            break;
    }

    u64 data = 0;
    if (!HashMapLookup(&gResourceSystem->map, resource.path, (u32)strlen(resource.path), &data)) {
        ArrayPush(resources, resource);
        resource_t *resourceData = ArrayBack(resources);
        LoadFn(resourceData, ScratchArena(0), PermanentArena(0));

        data = (u64)resourceData;
        HashMapSet(&gResourceSystem->map, resource.path, (u32)strlen(resource.path), data);
    } else {
        LOGW("Resource at %s already loaded!", resource.path);
    }
}

static void LoadResources(resource_t *resources, const char *assetDir, resource_type_t type, memory_arena_t *arena)
{
    const char *subDir = GetSubDirFromType(type);
    if (!subDir) {
        return;
    }

    const char *dir = ArenaPrintf(arena, "%s%s", assetDir, subDir);

    i32 count = 0;
    char **glob = SDL_GlobDirectory(dir, NULL, SDL_GLOB_CASEINSENSITIVE, &count);

    if (!glob) {
        LOGE("Failed to enumerate directory: %s", assetDir);
        return;
    }

    for (u32 i = 0; i < count; i++) {
        //skip directories
        if (GetResourceType(glob[i]) == type) {
            ResourceSystemLoadResource(resources, glob[i], type);
        }
    }
    SDL_free(glob);
}

void ResourceSystemInit(resource_system_t *resourceSystem, u32 resourceCapacity)
{
    gResourceSystem = resourceSystem;

    HashMapInitWithArena(&resourceSystem->map, PermanentArena(0), resourceCapacity);

    const char *basePath = SDL_GetBasePath();
    gResourceSystem->assetDir = StringIntern(ArenaPrintf(ScratchArena(0), "%s%s", basePath, "assets/"));
    ArrayInitWithArena(gResourceSystem->resources, PermanentArena(0), resourceCapacity);

    // these could all be allocated on scratch arena?
    ArrayInitWithArena(gResourceSystem->vertices, PermanentArena(0), MAX_VERTICES);
    ArrayInitWithArena(gResourceSystem->indices, PermanentArena(0), MAX_INDICES);
    ArrayInitWithArena(gResourceSystem->meshes, PermanentArena(0), MAX_MESHES);
    ArrayInitWithArena(gResourceSystem->materials, PermanentArena(0), MAX_MATERIALS);
    ArrayInitWithArena(gResourceSystem->meshDraws, PermanentArena(0), MAX_MESH_DRAWS);
    ArrayInitWithArena(gResourceSystem->animations, PermanentArena(0), MAX_ANIMATIONS);

    LoadResources(gResourceSystem->resources, gResourceSystem->assetDir, RESOURCE_TYPE_TEXTURE, PermanentArena(0));
    LoadResources(gResourceSystem->resources, gResourceSystem->assetDir, RESOURCE_TYPE_SHADER, PermanentArena(0));
    LoadResources(gResourceSystem->resources, gResourceSystem->assetDir, RESOURCE_TYPE_MESH, PermanentArena(0));
}

void ResourceSystemShutdown(void)
{
    for (u32 i = 0; i < ArrayCount(gResourceSystem->resources); i++) {
        resource_t *resource = &gResourceSystem->resources[i];
        switch(resource->type) {
            case RESOURCE_TYPE_MESH:
                break;
            case RESOURCE_TYPE_SHADER:
                break;
            case RESOURCE_TYPE_TEXTURE:
                VulkanUnloadTexture(resource);
                break;
            default:
                LV_ASSERT(false);
                break;
        }
    }

    HashMapFree(&gResourceSystem->map);
    gResourceSystem = NULL;
}

void ResourceSystemHotReload(resource_system_t *resourceSystem)
{
    gResourceSystem = resourceSystem;
}

resource_t *ResourceSystemGetResource(const char *uri)
{
    const char *fullPath = GetFullPathFromUri(uri, GetResourceType(uri), ScratchArena(0));
    u64 data = 0;
    if (!HashMapLookup(&gResourceSystem->map, fullPath, strlen(fullPath), &data)) {
        LOGE("Unable to find resource %s", fullPath);
    }
    return (resource_t*)data;
}

const resource_t **ResourceSystemGetTextures(memory_arena_t *arena)
{
    const resource_t **ppTextures = NULL;
    ArrayInitWithArena(ppTextures, arena, gResourceSystem->textureCount);
    for (u32 i = 0; i < ArrayCount(gResourceSystem->resources); i++) {
        const resource_t *resource = &gResourceSystem->resources[i];
        if (resource->type == RESOURCE_TYPE_TEXTURE) {
            ArrayPush(ppTextures, resource);
        }
    }
    return ppTextures;
}

const geometry_t ResourceSystemGetGeometry(void)
{
    geometry_t geometry = {0};
    geometry.indices = gResourceSystem->indices;
    geometry.vertices = gResourceSystem->vertices;
    geometry.meshes = gResourceSystem->meshes;
    geometry.materials = gResourceSystem->materials;
    geometry.draws = gResourceSystem->meshDraws;

    return geometry;
}
