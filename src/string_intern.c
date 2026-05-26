#include "string_intern.h"
#include "og_ds.h"
#include "platform.h"

static const char *PushString(const char *str)
{
    size_t strSize = strlen(str) + 1;
    char *result = PushArray(StringArena(), strSize, char);
    strncpy(result, str, strSize);
    return result;
}

const char *StringIntern(string_interning_system_t *system, const char *str)
{
    u64 id = hash((const u8*)str, strlen(str));
    //lookup the string in the map, if it exists return the id, otherwise insert it and return the new id
    const char *result = NULL;
    if (!HashMapLookupString(&system->map, id, &result)) {
        result = PushString(str);
        HashMapSetString(&system->map, id, result);
    }
    return result;
}

void StringInterningInit(string_interning_system_t *system)
{
    HashMapInitWithArena(&system->map, PermanentArena(), MAX_STRING_COUNT);
    PushString("");
}

void StringInterningDeinit(string_interning_system_t *system)
{
    HashMapFree(&system->map);
}
