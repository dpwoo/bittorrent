#ifndef LOG_H
#define LOG_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LOG_LEVEL_SILENT = -1,
    LOG_LEVEL_FATAL = 0,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_ALARM,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
};

enum {
    LOG_TIME_FMT_FULL = 0,
    LOG_TIME_FMT_SHORT,
};

int set_log_level(int level, int tmr_fmt);

int log_write(const char *file, int line, int level, const char *fmt, ...);

int log_dump(const char *file, int line, int level, const char *buf, int buflen, const char *fmt, ...);

#define LOG_FATAL(fmt, ...) \
    log_write(__FILE__, __LINE__, LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    log_write(__FILE__, __LINE__, LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)

#define LOG_ALARM(fmt, ...) \
    log_write(__FILE__, __LINE__, LOG_LEVEL_ALARM, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    log_write(__FILE__, __LINE__, LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    log_write(__FILE__, __LINE__, LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

#define LOG_DUMP(buf, buflen, fmt, ...) \
    log_dump(__FILE__, __LINE__, LOG_LEVEL_DEBUG, buf, buflen, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
extern "C" }
#endif

#endif
