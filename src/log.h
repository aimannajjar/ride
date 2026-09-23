#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void inline log_trace(const char *fmt, ...) {
#ifdef LOG_TRACE
  va_list ap;
  va_start(ap, fmt);
  time_t t = time(NULL);
  char *m = ctime(&t);

  printf("[%.*s] \e[33mTRACE\e[0m: ", (int)strlen(m) - 1, m);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
#endif
}

static void inline log_debug(const char *fmt, ...) {
#if defined(LOG_DEBUG) || defined(LOG_TRACE)
  va_list ap;
  va_start(ap, fmt);
  time_t t = time(NULL);
  char *m = ctime(&t);

  printf("[%.*s] \e[34mDEBUG\e[0m: ", (int)strlen(m) - 1, m);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
#endif
}

static void inline log_info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  time_t t = time(NULL);
  char *m = ctime(&t);

  printf("[%.*s] \e[32mINFO\e[0m;: ", (int)strlen(m) - 1, m);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

static void inline log_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  time_t t = time(NULL);
  char *m = ctime(&t);

  fprintf(stderr, "[%.*s] \e[31mERROR\e[0m: ", (int)strlen(m) - 1, m);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

