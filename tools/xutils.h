/*
 * Utility routines
 *
 * Copyright (C) 2013-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#ifndef __XUTILS_H__
#define __XUTILS_H__

#include <errno.h>    /* errno */
#include <limits.h>   /* PATH_MAX */
#include <stdarg.h>   /* va_* */
#include <stdio.h>    /* fclose fopen *printf */
#include <stdlib.h>   /* abort *alloc free system */
#include <string.h>   /* strdup strerror */
#include <sys/time.h> /* gettimeofday */

/* Enable debugging messages */
#define XU_DEBUG

/* Import config.h for PACKAGE_NAME in configure.ac */
#if HAVE_CONFIG_H
#include "config.h" /* PACKAGE_NAME */
#endif

/* Code position string */
/* Macros beginning with '__' are internal only. */
#ifdef PACKAGE_NAME
#define __package_str PACKAGE_NAME ": "
#else
#define __package_str
#endif
#define __code_pos_format __package_str "%s(), #%d: "
#define __code_pos_args   __FUNCTION__, __LINE__

/* Force flushing for fprintf/printf() */
#define __xu_fprintf(fp, format, ...)                                         \
    do {                                                                      \
        fprintf(fp, format, ##__VA_ARGS__);                                   \
        fflush(fp);                                                           \
    } while (0)
#define __xu_printf(format, ...) __xu_fprintf(stdout, format, ##__VA_ARGS__)

/* Logging: LOG() */

/*
 * Define the variable xu_log_fp in your C code.
 * If xu_log_fp!=NULL, write debugging messages to a log file.
 */
extern FILE *xu_log_fp;

static inline void
xu_open_log_file(const char *path)
{
    xu_log_fp = fopen(path, "w");
}

static inline void
xu_close_log_file(void)
{
    if (!xu_log_fp)
        return;
    (void)fclose(xu_log_fp);
    xu_log_fp = NULL;
}

/*
 * '##' before __VA_ARGS__ removes the comma before it if __VA_ARGS__ is empty.
 * This feature depends on the GCC extensions to C99.
 */
#define __xu_log_printf(format, ...)                                          \
    do {                                                                      \
        if (!xu_log_fp)                                                       \
            break;                                                            \
        fprintf(xu_log_fp, format, ##__VA_ARGS__);                            \
        fflush(xu_log_fp);                                                    \
    } while (0)
#define LOG(format, ...)                                                      \
    __xu_log_printf(__code_pos_format format, __code_pos_args, ##__VA_ARGS__)
#define NLOG(format, ...) 0 /* disabled LOG() */

/* Debugging: DB(), TRACE() */
/* Define XU_DEBUG to enable debugging marcos. */
#ifdef XU_DEBUG
#define XU_ASSERT 1 /* enable assertion */
#define DB(format, ...)                                                       \
    __xu_printf(__code_pos_format format, __code_pos_args, ##__VA_ARGS__)
#else
#define DB(format, ...) 0
#endif /* !XU_DEBUG */
#define TRACE()          DB("tracing...\n")
#define NDB(format, ...) 0 /* disabled DB() */
#define NTRACE()         0 /* disabled TRACE() */

/* Assertion: ASSERT() */
/* Define XU_ASSERT to enable assertion. */
#ifdef XU_ASSERT
#undef NDEBUG /* enable assert() */
#endif
#include <assert.h>   /* assert */
#define ASSERT assert /* ASSERT() is more readable than assert() */

/* Error messages: xu_error/perror/warning/unimpl() */
#define xu_error(msg, ...)                                                    \
    __xu_fprintf(stderr, __code_pos_format "[ERROR] " msg, __code_pos_args,   \
                 ##__VA_ARGS__)
#define xu_perror(msg, ...)                                                   \
    __xu_fprintf(stderr, __code_pos_format "[ERROR] %s (err=%d): " msg,       \
                 __code_pos_args, strerror(errno), errno, ##__VA_ARGS__)
#define xu_warning(msg, ...)                                                  \
    __xu_fprintf(stderr, __code_pos_format "[WARNING] " msg, __code_pos_args, \
                 ##__VA_ARGS__)
#define xu_unimpl(msg, ...)                                                   \
    __xu_fprintf(stderr, __code_pos_format "[UMIMPL] " msg, __code_pos_args,  \
                 ##__VA_ARGS__)

/* Exit for a fatal error: xu_exit() */
/*
 * Call this to terminate the application on a programming error (bug).
 * The abort() raises the SIGABRT signal and results in a core dump. It's
 * a simple but effective way to get enough runtime data for debugging.
 */
#define xu_exit()                                                             \
    do {                                                                      \
        abort();                                                              \
    } while (0)

/* Memory allocation: xu_*alloc(), xu_strdup(), xu_new*() */

/*
 * Wrap *alloc() to simplify memory checking: xu_*alloc() return a valid
 * memory pointer, or terminate the program if memory exhausted.
 */
static inline void *
xu_malloc(size_t size)
{
    void *ptr;

    /* malloc()'s behavior for 'size==0' is implementation dependent. */
    if (size == 0)
        return NULL;

    ptr = malloc(size);
    if (!ptr) {
        xu_error("memory exhausted (malloc %ld bytes)\n", size);
        xu_exit();
    }

    return ptr;
}

static inline void *
xu_calloc(size_t nmemb, size_t size)
{
    void *ptr;

    /* calloc()'s behavior for 'size==0' is implementation dependent. */
    if (nmemb == 0 || size == 0)
        return NULL;

    ptr = calloc(nmemb, size);
    if (!ptr) {
        xu_error("memory exhausted (calloc %ldx%ld bytes)\n", nmemb, size);
        xu_exit();
    }

    return ptr;
}

static inline void *
xu_realloc(void *ptr, size_t size)
{
    void *new_ptr;

    new_ptr = realloc(ptr, size);
    if (!new_ptr) {
        xu_error("memory exhausted (realloc %ld bytes)\n", size);
        xu_exit();
    }

    return new_ptr;
}

static inline void
xu_free(void *ptr)
{
    if (!ptr)
        return;

    free(ptr);
}

static inline char *
xu_strdup(const char *s)
{
    char *p;

    /* strdup() doesn't check for NULL-input! */
    if (!s)
        return NULL;

    p = strdup(s);
    if (!p) {
        xu_error("memory exhausted (strdup '%s')\n", s);
        xu_exit();
    }

    return p;
}

/* Wrappers for 'new' operators */
#define xu_new(T)       (T *)xu_malloc(sizeof(T))
#define xu_new_n(T, n)  (T *)xu_malloc((n) * sizeof(T))
#define xu_new0(T)      (T *)xu_calloc(1, sizeof(T))
#define xu_new0_n(T, n) (T *)xu_calloc((n), sizeof(T))
/* for variable-length structure */
#define xu_new_vl_n(T, E, n)  (T *)xu_malloc(sizeof(T) + sizeof(E) * (n))
#define xu_new0_vl_n(T, E, n) (T *)xu_calloc(1, sizeof(T) + sizeof(E) * (n))

/* Wrapper for system() */
static inline int
xu_system(const char *command, ...)
{
    va_list ap;
    char cmd_line[PATH_MAX];
    int n;

    va_start(ap, command);
    n = vsnprintf(cmd_line, sizeof(cmd_line), command, ap);
    va_end(ap);

    if (n >= sizeof(cmd_line)) {
        xu_error("command is longer than %ld: %s ...\n", sizeof(cmd_line),
                 command);
        xu_exit();
    }

    return system(cmd_line);
}

/* Useful marcos */
#define xu_n_elems(array) (sizeof(array) / sizeof(array[0]))
#define xu_max(a, b)                                                          \
    ({                                                                        \
        int _a, _b;                                                           \
        _a = (a);                                                             \
        _b = (b);                                                             \
        _a > _b ? _a : _b;                                                    \
    })
#define xu_min(a, b)                                                          \
    ({                                                                        \
        int _a, _b;                                                           \
        _a = (a);                                                             \
        _b = (b);                                                             \
        _a < _b ? _a : _b;                                                    \
    })

/* Performance measurement */
static inline int
xu_get_usec(void)
{
    struct timeval tv;

    (void)gettimeofday(&tv, NULL);
    return (tv.tv_sec % 1024) * 1000 * 1000 + tv.tv_usec;
}

#endif /* __XUTILS_H__ */
