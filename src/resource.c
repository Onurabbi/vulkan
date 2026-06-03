#include "resource.h"
#include "platform.h"
#include "string_utils.h"
#include "log.h"

#define FAST_OBJ_IMPLEMENTATION
#include "fast_obj.h"

#include <SDL3/SDL_filesystem.h>

static resource_system_t *gResourceSystem;

static void LoadMesh(void *data, memory_arena_t *arena)
{
    resource_t *meshResource = (resource_t *)data;

    vertex_t *vertices = NULL;
    const char *path = meshResource->path;

    fastObjMesh *obj = fast_obj_read(path);
    if (obj) {
        u32 index_count = 0;
        for (u32 i = 0; i < obj->face_count; i++) {
            index_count += 3 * (obj->face_vertices[i] - 2);
        }

        ArrayInitWithArena(vertices, arena, index_count);
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

    meshResource->vertices = vertices;

    fast_obj_destroy(obj);
}

static void LoadShader(void *data, memory_arena_t *arena)
{
    resource_t *shaderResource = (resource_t *)data;

    const char *path = shaderResource->path;
    uint8_t *spirv = NULL;
    FILE *file = fopen(path, "rb");
    fseek(file, 0, SEEK_END);
    u32 fileSize = (u32)ftell(file);
    fseek(file, 0, SEEK_SET);

    LV_ASSERT((fileSize % sizeof(u32) == 0) && "SPIR-V file size must be a multiple of 4");

    ArrayInitWithArena(spirv, arena, fileSize);
    ArrayResize(spirv, fileSize);

    LV_ASSERT((fread(spirv, 1, fileSize, file) == fileSize) && ("Failed to read entire shader file"));

    fclose(file);

    shaderResource->spirv = spirv;
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
        void *ptrData = ArrayBack(resources);
        // deduce the type from extension
        const char *extension = StringUtilsGetExtensionFromPath(internedPath);
        if (strncmp(extension, "spv", 3) == 0) {
            PushJob(LoadShader, ptrData);
        } else if (strncmp(extension, "obj", 3) == 0) {
            PushJob(LoadMesh, ptrData);
        } else if (strncmp(extension, "ktx", 3) == 0) {
            LOGI("%s is a KTX texture path", extension);
        }

        data = (u64)ptrData;
        HashMapSet(&gResourceSystem->map, internedPath, (u32)strlen(internedPath), data);
    } else {
        LOGW("Resource at %s already loaded!", internedPath);
    }
    SDL_UnlockMutex(gResourceSystem->mutex);
}

static resource_t *LoadResources(const char *assetDir, const char *pattern, memory_arena_t *arena)
{
    resource_t *resources = NULL;

    i32 count = 0;
    char **glob = SDL_GlobDirectory(assetDir, pattern, 0, &count);
    if (!glob) {
        LOGE("Failed to enumerate directory: %s", assetDir);
        return NULL;
    }

    ArrayInitWithArena(resources, arena, count);
    for (u32 i = 0; i < count; i++) {
        ResourceSystemLoadResource(resources, glob[i]);
    }
    SDL_free(glob);

    return resources;
}

void ResourceSystemInit(resource_system_t *resourceSystem, u32 resourceCapacity)
{
    gResourceSystem = resourceSystem;

    HashMapInitWithArena(&resourceSystem->map, PermanentArena(), resourceCapacity);
    resourceSystem->mutex = SDL_CreateMutex();
    if (!resourceSystem->mutex) {
        LOGF("Unable to initialize resource system mutex");
        return;
    }

    const char *basePath = SDL_GetBasePath();
    gResourceSystem->assetDir = StringIntern(ArenaPrintf(ScratchArena(0), "%s%s", basePath, "assets/"));
    gResourceSystem->shaderResources = LoadResources(gResourceSystem->assetDir, "*.spv", PermanentArena());
    gResourceSystem->meshResources = LoadResources(gResourceSystem->assetDir, "*.obj", PermanentArena());

    WaitForAllJobs();
}

void ResourceSystemShutdown(void)
{
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
