#ifndef LOG_H
#define LOG_H
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>


#define LOG_DEBUG(msg, ...) log_debug("%s: " msg, __FUNCTION__, ## __VA_ARGS__ )
#define LOG_INFO(msg, ...) log_info("%s: " msg, __FUNCTION__, ## __VA_ARGS__ )
#define LOG_ERROR(msg, ...) log_error("%s: " msg, __FUNCTION__, ## __VA_ARGS__ )
#define LOG_TRACE(msg, ...) log_trace("%s: " msg, __FUNCTION__, ## __VA_ARGS__ )

static void inline log_trace(const char *fmt, ...) {
#ifdef LOG_LEVEL_TRACE
  char ts[26];
  va_list ap;
  va_start(ap, fmt);
  time_t t = time(NULL);
  char *m = ctime_r(&t, ts);

  printf("[%.*s] \e[33mTRACE\e[0m: ", (int)strlen(m) - 1, m);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
#endif
}

static void inline log_debug(const char *fmt, ...) {
#if defined(LOG_LEVEL_DEBUG) || defined(LOG_LEVEL_TRACE)
  char ts[26];
  va_list ap;
  va_start(ap, fmt);
  time_t t = time(NULL);
  char *m = ctime_r(&t, ts);

  printf("[%.*s] \e[34mDEBUG\e[0m: ", (int)strlen(m) - 1, m);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
#endif
}

static void inline log_info(const char *fmt, ...) {
  char ts[26];
  va_list ap;
  va_start(ap, fmt);
  time_t t = time(NULL);
  char *m = ctime_r(&t, ts);

  printf("[%.*s] \e[32mINFO\e[0m;: ", (int)strlen(m) - 1, m);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

static void inline log_error(const char *fmt, ...) {
  char ts[26];
  va_list ap;
  va_start(ap, fmt);
  time_t t = time(NULL);
  char *m = ctime_r(&t, ts);

  fprintf(stderr, "[%.*s] \e[31mERROR\e[0m: ", (int)strlen(m) - 1, m);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

#endif
