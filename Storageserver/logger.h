#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

// Log levels for classifying messages
typedef enum {
    LOGLVL_INFO,    // General operational information
    LOGLVL_WARN,    // Potentially harmful situations
    LOGLVL_ERROR,   // Error events that might still allow the system to continue
    LOGLVL_DEBUG    // Detailed debug information
} LogLevel;

/**
 * @brief Logs a formatted message to stdout with a timestamp and log level.
 * 
 * @param level The severity level of the log message.
 * @param format The format string for the message (like printf).
 * @param ... Variable arguments for the format string.
 */
void log_message(LogLevel level, const char *format, ...);

#endif // LOGGER_H
