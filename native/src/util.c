/*
 * Copyright 2026 Secragon Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "privacy.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p) { /* exit invokes the registered terminal restoration handler. */
        fputs("incognito-terminal: out of memory\n", stderr);
        exit(1);
    }
    return p;
}
void *xrealloc(void *p, size_t size) {
    void *q = realloc(p, size);
    if (!q) {
        fputs("incognito-terminal: out of memory\n", stderr);
        exit(1);
    }
    return q;
}
void buf_add(Buffer *b, const void *s, size_t n) {
    if (n > SIZE_MAX - b->len - 1)
        exit(1);
    size_t need = b->len + n + 1;
    if (need > b->cap) {
        b->cap = MAX(need, b->cap ? b->cap * 2 : 256);
        b->data = xrealloc(b->data, b->cap);
    }
    if (n)
        memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
void buf_str(Buffer *b, const char *s) {
    buf_add(b, s, strlen(s));
}
void buf_printf(Buffer *b, const char *fmt, ...) {
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n >= 0) {
        char *s = xcalloc((size_t)n + 1, 1);
        vsnprintf(s, (size_t)n + 1, fmt, ap);
        buf_add(b, s, (size_t)n);
        free(s);
    }
    va_end(ap);
}
void buf_consume(Buffer *b, size_t n) {
    n = MIN(n, b->len);
    b->len -= n;
    if (b->data) {
        memmove(b->data, b->data + n, b->len);
        b->data[b->len] = 0;
    }
}
void buf_free(Buffer *b) {
    if (b->data) {
        explicit_bzero(b->data, b->cap);
        free(b->data);
    }
    *b = (Buffer){0};
}
void buf_cp(Buffer *b, uint32_t c) {
    char v[4];
    size_t n;
    if (c < 0x80)
        v[0] = (char)c, n = 1;
    else if (c < 0x800)
        v[0] = (char)(0xc0 | (c >> 6)), v[1] = (char)(0x80 | (c & 63)), n = 2;
    else if (c < 0x10000)
        v[0] = (char)(0xe0 | (c >> 12)), v[1] = (char)(0x80 | ((c >> 6) & 63)),
        v[2] = (char)(0x80 | (c & 63)), n = 3;
    else if (c <= 0x10ffff)
        v[0] = (char)(0xf0 | (c >> 18)), v[1] = (char)(0x80 | ((c >> 12) & 63)),
        v[2] = (char)(0x80 | ((c >> 6) & 63)), v[3] = (char)(0x80 | (c & 63)), n = 4;
    else {
        buf_cp(b, 0xfffd);
        return;
    }
    buf_add(b, v, n);
}
double monotime(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int write_all(int fd, const void *s, size_t n) {
    const char *p = s;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!k)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}
