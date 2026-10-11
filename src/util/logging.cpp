#include "util/logging.h"
#include <cstdlib>

static LogLevel g_log_level = LOG_LEVEL_INFO;
static FILE* g_log_file = nullptr;

void LOG_INIT() {
    g_log_file = stdout;
    g_log_level = LOG_LEVEL_INFO;
}

void LOG_SET_LEVEL(LogLevel level) {
    g_log_level = level;
}

void LOG_MESSAGE(LogLevel level, const char* fmt, ...) {
    if (level < g_log_level) {
        return;
    }

    const char* level_str = "INFO";
    switch (level) {
        case LOG_LEVEL_DEBUG:
            level_str = "DEBUG";
            break;
        case LOG_LEVEL_INFO:
            level_str = "INFO";
            break;
        case LOG_LEVEL_WARN:
            level_str = "WARN";
            break;
        case LOG_LEVEL_ERROR:
            level_str = "ERROR";
            break;
    }

    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);

    if (!g_log_file) {
        g_log_file = stdout;
    }

    fprintf(g_log_file, "[%s] [%s] ", time_str, level_str);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);

    fprintf(g_log_file, "\n");
    fflush(g_log_file);
}
