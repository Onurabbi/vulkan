#include "string_utils.h"
#include "og_ds.h"
#include "platform.h"

static string_interning_system_t *gSystem;

static const char *PushString(const char *str)
{
    size_t strSize = strlen(str) + 1;
    char *result = PushArray(StringArena(), strSize, char);
    strncpy(result, str, strSize);
    return result;
}

const char *StringIntern(const char *str)
{
    u64 id = hash((const u8*)str, strlen(str));
    //lookup the string in the map, if it exists return the id, otherwise insert it and return the new id
    const char *result = NULL;
    if (!HashMapLookupString(&gSystem->map, id, &result)) {
        result = PushString(str);
        HashMapSetString(&gSystem->map, id, result);
    }
    return result;
}

void StringUtilsInit(string_interning_system_t *system)
{
    gSystem = system;
    HashMapInitWithArena(&gSystem->map, PermanentArena(0), MAX_STRING_COUNT);
    PushString("");
}

void StringUtilsHotReload(string_interning_system_t *system)
{
    gSystem = system;
}

void StringUtilsShutdown(void)
{
    HashMapFree(&gSystem->map);
    gSystem = NULL;
}

const char *StringUtilsGetExtensionFromPath(const char *path)
{
    if (!path) return "";
    const char *ptr = path;
    while (*ptr && *ptr++ != '.') {} 
    return ptr;
}
