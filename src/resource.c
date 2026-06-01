#include "resource.h"
#include "platform.h"
#include "string_utils.h"
#include "log.h"

#define FAST_OBJ_IMPLEMENTATION
#include "fast_obj.h"

static resource_system_t *gResourceSystem;

static void LoadMesh(void *data, memory_arena_t *arena)
{
    mesh_resource_t *meshResource = (mesh_resource_t *)data;

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

void ResourceSystemInit(resource_system_t *resourceSystem, u32 resourceCapacity)
{
    gResourceSystem = resourceSystem;

    HashMapInitWithArena(&resourceSystem->map, PermanentArena(), resourceCapacity);
    resourceSystem->mutex = SDL_CreateMutex();
    if (!resourceSystem->mutex) {
        LOGF("Unable to initialize resource system mutex");
        return;
    }

    ArrayInitWithArena(resourceSystem->meshResources, PermanentArena(), MAX_MESHES);

    ResourceSystemLoadResource("assets/suzanne.obj");

    ResourceSystemLoadResource("assets/suzanne3.ktx");
    ResourceSystemLoadResource("assests/shader.spv");
    ResourceSystemLoadResource("assets/compute_shader.spv");

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

mesh_resource_t *ResourceSystemGetMeshResource(const char *path)
{
    SDL_LockMutex(gResourceSystem->mutex);
    mesh_resource_t *result = NULL;
    u64 data;
    if (HashMapLookup(&gResourceSystem->map, path, (u32)strlen(path), &data)) {
        result = (mesh_resource_t *)data;
    } else {
        LOGE("Unable to find resource: %s", path);
    }
    SDL_UnlockMutex(gResourceSystem->mutex);
    return result;
}

void ResourceSystemLoadResource(const char *path)
{
    SDL_LockMutex(gResourceSystem->mutex);
    void *dataPtr = NULL;
    u64 data;
    if (!HashMapLookup(&gResourceSystem->map, path, (u32)strlen(path), &data)) {
        const char *internedPath = StringIntern(path);
        // deduce the type from extension
        const char *extension = StringUtilsGetExtensionFromPath(internedPath);
        if (strncmp(extension, "spv", 3) == 0) {
        } else if (strncmp(extension, "obj", 3) == 0) {
            mesh_resource_t res = {
                .path = internedPath,
                .vertices = NULL,
            };
            ArrayPush(gResourceSystem->meshResources, res);
            dataPtr = ArrayBack(gResourceSystem->meshResources);
            PushJob(LoadMesh, dataPtr);
        } else if (strncmp(extension, "ktx", 3) == 0) {
            LOGI("%s is a KTX texture path", extension);
        }

        data = (u64)dataPtr;
        HashMapSet(&gResourceSystem->map, internedPath, (u32)strlen(internedPath), data);
    } else {
        LOGW("Resource at %s already loaded!", path);
    }
    SDL_UnlockMutex(gResourceSystem->mutex);
}
