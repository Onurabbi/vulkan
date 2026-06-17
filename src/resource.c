#include "resource.h"
#include "platform.h"
#include "string_utils.h"
#include "log.h"

#include "vulkan/vulkan.h"

#define FAST_OBJ_IMPLEMENTATION
#include "fast_obj.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <meshoptimizer/src/meshoptimizer.h>

#include <SDL3/SDL_filesystem.h>

static resource_system_t *gResourceSystem;

static void LoadTexture(void *data, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    resource_t *textureResource = (resource_t *)data;
    textureResource->type = RESOURCE_TYPE_TEXTURE;
    (void)scratchArena;
    (void)permanentArena;
    //TODO: It seems like an absolute hassle to parallelize this :(
    SDL_LockMutex(gResourceSystem->mutex);
    VulkanLoadTexture(textureResource, textureResource->path);
    SDL_UnlockMutex(gResourceSystem->mutex);
}

static void AppendMesh(resource_t *meshResource, vertex_t *vertices, memory_arena_t *permanentArena, memory_arena_t *scratchArena)
{
    u32 *indices = NULL;
    ArrayInitWithArena(indices, scratchArena, ArrayCount(vertices));
    for (u32 i = 0; i < ArrayCount(vertices); i++) {
        ArrayPush(indices, i);
    }

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
        ArrayPush(positions, vertices[i].p);
    }

    vec3_t *normals = NULL;
    ArrayInitWithArena(normals, scratchArena, ArrayCount(vertices));
    for (u32 i = 0; i < ArrayCount(vertices); i++) {
        ArrayPush(normals, vertices[i].n);
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
    mesh.textureIndex = SDL_rand(3);

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

    meshResource->mesh.firstMeshIndex = ArrayCount(gResourceSystem->meshes);
    meshResource->mesh.meshCount = 1;

    ArrayPush(gResourceSystem->meshes, mesh);
}

// TODO: Move this to its own file
static void LoadMesh(void *data, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    resource_t *meshResource = (resource_t *)data;
    meshResource->type = RESOURCE_TYPE_MESH;

    vertex_t *vertices = NULL;
    const char *path = meshResource->path;

    fastObjMesh *obj = fast_obj_read(path);
    if (obj) {
        u32 index_count = 0;
        for (u32 i = 0; i < obj->face_count; i++) {
            index_count += 3 * (obj->face_vertices[i] - 2);
        }

        ArrayInitWithArena(vertices, permanentArena, index_count);
        ArrayResize(vertices, index_count);

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
                v->p.X = obj->positions[3 * gi.p + 0];
                v->p.Y = obj->positions[3 * gi.p + 1];
                v->p.Z = obj->positions[3 * gi.p + 2];
                v->t.X = obj->texcoords[2 * gi.t + 0];
                v->t.Y = 1.0f - obj->texcoords[2 * gi.t + 1];
                v->n.X = obj->normals[3 * gi.n + 0];
                v->n.Y = obj->normals[3 * gi.n + 1];
                v->n.Z = obj->normals[3 * gi.n + 2];
            }
            index_offset += obj->face_vertices[i];
        }
        LV_ASSERT(vertex_offset == index_count);
    }

    AppendMesh(meshResource, vertices, permanentArena, scratchArena);
    fast_obj_destroy(obj);
}

static void LoadScene(void *resource, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    resource_t *sceneResource = (resource_t *)resource;
    //sceneResource->type = RESOURCE_TYPE_SCENE;

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

exit:
    cgltf_free(data);
}

static void LoadShader(void *data, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    resource_t *shaderResource = (resource_t *)data;
    shaderResource->type = RESOURCE_TYPE_SHADER;

    VulkanLoadShader(shaderResource, shaderResource->path, scratchArena, permanentArena);
}

static resource_type_t GetResourceType(const char *path)
{
    const char *extension = StringUtilsGetExtensionFromPath(path);
    if (strncmp(extension, "spv", 3) == 0) {
        return RESOURCE_TYPE_SHADER;
    } else if (strncmp(extension, "obj", 3) == 0) {
        return RESOURCE_TYPE_MESH;
    } else if (strncmp(extension, "ktx", 3) == 0) {
        return RESOURCE_TYPE_TEXTURE;
    } else if (strncmp(extension, "gltf", 4) == 0) {
        return RESOURCE_TYPE_MESH;
    } 
    return RESOURCE_TYPE_INVALID;
}

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

static const char *GetFullPathFromName(const char *name, resource_type_t type, memory_arena_t *arena)
{
    const char *dir = GetSubDirFromType(type);
    return StringIntern(ArenaPrintf(arena, "%s%s%s", gResourceSystem->assetDir, dir, name));
}

static void ResourceSystemLoadResource(resource_t *resources, const char *fileName)
{
    SDL_LockMutex(gResourceSystem->mutex);

    resource_t resource = {0};
    resource.type = GetResourceType(fileName);
    if (resource.type == RESOURCE_TYPE_INVALID) {
        LOGE("Unknown resource type");
        return;
    }

    resource.path = GetFullPathFromName(fileName, resource.type, ScratchArena(0));

    void (*LoadFn)(void *, memory_arena_t *, memory_arena_t *arena);

    switch(resource.type) {
        case RESOURCE_TYPE_MESH:
            gResourceSystem->meshCount++; 
            LoadFn = LoadMesh;
            break;
        case RESOURCE_TYPE_SHADER:
            gResourceSystem->shaderCount++;
            LoadFn = LoadShader;
            break;
        case RESOURCE_TYPE_TEXTURE:
            gResourceSystem->textureCount++;
            LoadFn = LoadTexture;
            break;
    }

    u64 data = 0;
    if (!HashMapLookup(&gResourceSystem->map, resource.path, (u32)strlen(resource.path), &data)) {
        ArrayPush(resources, resource);
        resource_t *resourceData = ArrayBack(resources);
        PushJob(LoadFn, resourceData);

        data = (u64)resourceData;
        HashMapSet(&gResourceSystem->map, resource.path, (u32)strlen(resource.path), data);
    } else {
        LOGW("Resource at %s already loaded!", resource.path);
    }
    SDL_UnlockMutex(gResourceSystem->mutex);
}

static void LoadResources(resource_t *resources, const char *assetDir, const char *pattern, memory_arena_t *arena)
{
    resource_type_t type = GetResourceType(pattern);
    if (type == RESOURCE_TYPE_INVALID) {
        LOGF("Unknown resource type");
        return;
    }
    const char *subDir = GetSubDirFromType(type);
    if (!subDir) {
        return;
    }

    const char *dir = ArenaPrintf(arena, "%s%s", assetDir, subDir);

    i32 count = 0;
    char **glob = SDL_GlobDirectory(dir, pattern, 0, &count);
    if (!glob) {
        LOGE("Failed to enumerate directory: %s", assetDir);
    }

    for (u32 i = 0; i < count; i++) {
        ResourceSystemLoadResource(resources, glob[i]);
    }
    SDL_free(glob);
}

void ResourceSystemInit(resource_system_t *resourceSystem, u32 resourceCapacity)
{
    gResourceSystem = resourceSystem;

    HashMapInitWithArena(&resourceSystem->map, PermanentArena(0), resourceCapacity);
    resourceSystem->mutex = SDL_CreateMutex();
    if (!resourceSystem->mutex) {
        LOGF("Unable to initialize resource system mutex");
        return;
    }

    const char *basePath = SDL_GetBasePath();
    gResourceSystem->assetDir = StringIntern(ArenaPrintf(ScratchArena(0), "%s%s", basePath, "assets/"));
    ArrayInitWithArena(gResourceSystem->resources, PermanentArena(0), resourceCapacity);

    // these could all be allocated on scratch arena?
    ArrayInitWithArena(gResourceSystem->vertices, PermanentArena(0), MAX_VERTICES);
    ArrayInitWithArena(gResourceSystem->indices, PermanentArena(0), MAX_INDICES);
    ArrayInitWithArena(gResourceSystem->meshes, PermanentArena(0), MAX_MESHES);

    LoadResources(gResourceSystem->resources, gResourceSystem->assetDir, "*.spv", PermanentArena(0));
    LoadResources(gResourceSystem->resources, gResourceSystem->assetDir, "*.obj", PermanentArena(0));
    LoadResources(gResourceSystem->resources, gResourceSystem->assetDir, "*.ktx", PermanentArena(0));

    WaitForAllJobs();
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
        }
    }

    HashMapFree(&gResourceSystem->map);
    SDL_DestroyMutex(gResourceSystem->mutex);

    gResourceSystem = NULL;
}

void ResourceSystemHotReload(resource_system_t *resourceSystem)
{
    gResourceSystem = resourceSystem;
}

const resource_t *ResourceSystemGetResource(const char *fileName)
{
    const char *fullPath = GetFullPathFromName(fileName, GetResourceType(fileName), ScratchArena(0));
    u64 data = 0;
    if (!HashMapLookup(&gResourceSystem->map, fullPath, strlen(fullPath), &data)) {
        LOGE("Unable to find resource %s", fullPath);
    }
    return (const resource_t *)data;
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
    return geometry;
}

