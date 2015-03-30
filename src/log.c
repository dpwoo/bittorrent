#include <sys/time.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include "log.h"

static int short_time_fmt = LOG_TIME_FMT_FULL;
static int log_level = LOG_LEVEL_ERROR;

int
set_log_level(int level, int tmr_fmt)
{
    log_level = level;
    short_time_fmt = tmr_fmt;
    return 0;
}

static int
log_time(char *timebuf, int buflen)
{
    struct timespec tp;
    memset(&tp, 0, sizeof(tp));
    if(clock_gettime(CLOCK_REALTIME, &tp)) {
        return -1;
    }

    struct tm *tm = localtime(&tp.tv_sec);
    if(!tm) {
        return -1;
    }

    int wlen;
    if(short_time_fmt) {
        wlen = strftime(timebuf, buflen, "%H:%M:%S", tm);
    } else {
        wlen = strftime(timebuf, buflen, "%Y-%m-%d %H:%M:%S", tm);
    }
    snprintf(timebuf+wlen, buflen - wlen, ".%03d", (int)(tp.tv_nsec / 1000000));
    
    return 0;
}

static const char* 
log_basename(const char *file)
{
    char *s = strrchr(file, '/');
    if(!s) {
        return file;
    }
    return s+1;
}

int
log_write(const char *file, int line, int level, const char *fmt, ...)
{
    if(level > log_level) {
        return -1;
    }

    int wlen;
    char timebuf[64], buffer[2048];

    log_time(timebuf, sizeof(timebuf));
    wlen = snprintf(buffer, sizeof(buffer), "%s[%-12.12s:%04d]",timebuf, log_basename(file), line);

    va_list vl;
    va_start(vl, fmt);
    wlen += vsnprintf(buffer+wlen, sizeof(buffer)-wlen, fmt, vl);
    va_end(vl);

    fprintf(stderr, "%s", buffer);

    return 0;
}

int
log_dump(const char *file, int line, int level, const char *buf, int buflen, const char *fmt, ...)
{
    if(level > log_level) {
        return -1;
    } 

    int wlen;
    char timebuf[64], buffer[4096];

    log_time(timebuf, sizeof(timebuf));
    wlen = snprintf(buffer, sizeof(buffer), "%s[%-12.12s:%04d]",timebuf, log_basename(file), line);

    va_list vl; va_start(vl, fmt);
    wlen += vsnprintf(buffer+wlen, sizeof(buffer)-wlen, fmt, vl);
    va_end(vl);

    fprintf(stderr, "%s\n", buffer);

    char *s = buffer;
    int left = sizeof(buffer) - 2; /* 2 for '\n', '\0' */
    left = (left >> 2) >= buflen ? buflen : (left >> 2);

    int i;
    for(i = 0; i < left; i++) {
        s += snprintf(s, 5, "0x%02X", (unsigned char)buf[i]);
    }
    *s = '\0';

    fprintf(stderr, "%s\n", buffer);

    return 0;
}

