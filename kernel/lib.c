#include "lib.h"

void *k_memset(void *dst, u8 value, usize count) {
    u8 *d = (u8 *)dst;
    for (usize i = 0; i < count; ++i) d[i] = value;
    return dst;
}

void *k_memcpy(void *dst, const void *src, usize count) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (usize i = 0; i < count; ++i) d[i] = s[i];
    return dst;
}

usize k_strlen(const char *s) {
    usize n = 0;
    while (s && s[n]) ++n;
    return n;
}

char k_ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

bool k_streq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

bool k_strieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (k_ascii_lower(*a) != k_ascii_lower(*b)) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

bool k_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
