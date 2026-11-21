#pragma once
#include <stddef.h>
#include <string.h>
#define strlcpy host_strlcpy
static inline size_t host_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = 0;
    }
    return len;
}
