//
// Created by onurabbi on 25/07/25.
//

#ifndef LOG_H
#define LOG_H

enum {INFO, WARNING, ERROR, FATAL};

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

void LogOutput(i32 severity, const char *format, ...);
void ReportAssertionFailure(const char *expr);

#define LOGI(string,...) LogOutput(INFO, "[%s:%u] " string,__FUNCTION__,__LINE__,##__VA_ARGS__)
#define LOGW(string,...) LogOutput(WARNING, "[%s:%u] " string,__FUNCTION__,__LINE__,##__VA_ARGS__)
#define LOGE(string,...) LogOutput(ERROR, "[%s:%u] " string,__FUNCTION__,__LINE__,##__VA_ARGS__)
#define LOGF(string,...) LogOutput(FATAL, "[%s:%u] " string,__FUNCTION__,__LINE__,##__VA_ARGS__)

#endif //LOG_H
