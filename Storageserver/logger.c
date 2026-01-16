#include "logger.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

// ANSI color codes for log levels
#define COLOR_RESET   "\x1b[0m"
#define COLOR_CYAN    "\x1b[36m" // INFO
#define COLOR_YELLOW  "\x1b[33m" // WARN
#define COLOR_RED     "\x1b[31m" // ERROR
#define COLOR_GREEN   "\x1b[32m" // DEBUG

// Converts LogLevel enum to a string representation
static const char* level_to_string(LogLevel level) {
    switch (level) {
        case LOGLVL_INFO:  return "INFO";
        case LOGLVL_WARN:  return "WARN";
        case LOGLVL_ERROR: return "ERROR";
        case LOGLVL_DEBUG: return "DEBUG";
        default: return "UNKNOWN";
    }
}

// Converts LogLevel enum to a color code
static const char* level_to_color(LogLevel level) {
    switch (level) {
        case LOGLVL_INFO:  return COLOR_CYAN;
        case LOGLVL_WARN:  return COLOR_YELLOW;
        case LOGLVL_ERROR: return COLOR_RED;
        case LOGLVL_DEBUG: return COLOR_GREEN;
        default: return COLOR_RESET;
    }
}

void log_message(LogLevel level, const char *format, ...) {
    // Get current time
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[20];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    // Print log prefix with color
    fprintf(stdout, "%s[%s] [%s] " COLOR_RESET, 
            level_to_color(level), time_buf, level_to_string(level));

    // Print the actual log message
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);

    // Append newline and flush
    fprintf(stdout, "\n");
    fflush(stdout);
}
