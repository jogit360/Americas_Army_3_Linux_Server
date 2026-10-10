#ifndef AA3_LOGGING_H
#define AA3_LOGGING_H

#include <cstdio>
#include <cstdarg>
#include <ctime>

enum LogLevel {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
};

void LOG_INIT();
void LOG_SET_LEVEL(LogLevel level);
void LOG_MESSAGE(LogLevel level, const char* fmt, ...);

#define LOG_DEBUG(fmt, ...) LOG_MESSAGE(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_MESSAGE(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_MESSAGE(LOG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_MESSAGE(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)

#endif // AA3_LOGGING_H
