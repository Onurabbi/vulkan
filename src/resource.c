#include "resource.h"
#include "platform.h"
#include "string_utils.h"
#include "log.h"

#include "vulkan/vulkan.h"

#define FAST_OBJ_IMPLEMENTATION
#include "fast_obj.h"

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

    meshResource->mesh.vertices = vertices;

    fast_obj_destroy(obj);
}

static void LoadShader(void *data, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    resource_t *shaderResource = (resource_t *)data;
    shaderResource->type = RESOURCE_TYPE_SHADER;

    VulkanLoadShader(shaderResource, shaderResource->path, scratchArena, permanentArena);
}

static void ResourceSystemLoadResource(resource_t *resources, const char *fileName)
{
    SDL_LockMutex(gResourceSystem->mutex);

    resource_t resource = {0};
    const char *internedPath = StringIntern(ArenaPrintf(ScratchArena(0), "%s%s", gResourceSystem->assetDir, fileName));
    resource.path = internedPath;
    u64 data = 0;
    if (!HashMapLookup(&gResourceSystem->map, internedPath, (u32)strlen(internedPath), &data)) {
        ArrayPush(resources, resource);
        resource_t *resourceData = ArrayBack(resources);
        // deduce the type from extension
        const char *extension = StringUtilsGetExtensionFromPath(internedPath);
        if (strncmp(extension, "spv", 3) == 0) {
            gResourceSystem->shaderCount++;
            PushJob(LoadShader, resourceData);
        } else if (strncmp(extension, "obj", 3) == 0) {
            gResourceSystem->meshCount++;
            PushJob(LoadMesh, resourceData);
        } else if (strncmp(extension, "ktx", 3) == 0) {
            gResourceSystem->textureCount++;
            PushJob(LoadTexture, resourceData);
        }

        data = (u64)resourceData;
        HashMapSet(&gResourceSystem->map, internedPath, (u32)strlen(internedPath), data);
    } else {
        LOGW("Resource at %s already loaded!", internedPath);
    }
    SDL_UnlockMutex(gResourceSystem->mutex);
}

static void LoadResources(resource_t *resources, const char *assetDir, const char *pattern, memory_arena_t *arena)
{
    i32 count = 0;
    char **glob = SDL_GlobDirectory(assetDir, pattern, 0, &count);
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
    const char *fullPath = ArenaPrintf(ScratchArena(0), "%s%s", gResourceSystem->assetDir, fileName);
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
