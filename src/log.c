#include "common.h"
#include "log.h"

#include <stdio.h>
#include <stdarg.h>

#define LOG_COLOURED(stream,string, color) fprintf(stream, color "%s" ANSI_COLOR_RESET, string)
const ssize_t buf_size = 1024;

static void TruncateBufIfNecessary(char *buf, ssize_t buf_size, i32 written)
{
    if (written >= buf_size - 4) {
        //add three dots
        buf[buf_size - 1] = '\0';
        buf[buf_size - 2] = '.';
        buf[buf_size - 3] = '.';
        buf[buf_size - 4] = '.';
    }
}

void LogOutput(i32 severity, const char *format, ...)
{
    const char *level_strings[] = {"INFO", "WARNING", "ERROR", "FATAL"};

    va_list args;
    va_start(args, format);
    char buffer[buf_size];
    i32 written = vsnprintf(buffer, 1024, format, args);
    va_end(args);

    TruncateBufIfNecessary(buffer, buf_size, written);

    const char *level_string = "NULL";
    if (severity >= INFO && severity <= FATAL) {
        level_string = level_strings[severity];
    }

    char buffer2[1024];
    written = snprintf(buffer2, 1024, "[%s] %s\n", level_string, buffer);
    TruncateBufIfNecessary(buffer2, buf_size, written);

    switch (severity) {
        case INFO:
            LOG_COLOURED(stdout, buffer2, ANSI_COLOR_GREEN);
            break;
        case WARNING:
            LOG_COLOURED(stdout, buffer2, ANSI_COLOR_YELLOW);
            break;
        case ERROR:
            LOG_COLOURED(stderr, buffer2, ANSI_COLOR_RED);
            break;
        case FATAL:
            LOG_COLOURED(stderr, buffer2, ANSI_COLOR_RED);
            break;
        default:
            break;
    }
    fflush(stdout);
    fflush(stderr);
}

LV_EXPORT void ReportAssertionFailure(const char *expr)
{
    LogOutput(FATAL, "[%s:%u] Assertion Failed: %s\n", __FUNCTION__, __LINE__, expr);
}
