/*
 * CBMC Formal Verification Harness: Username Sanitization
 *
 * Simplified: prefix="i", owner="maxx" are concrete.
 * Only stem is symbolic (length <= 8).
 *
 * Properties:
 *   U1: output length > 0
 *   U2: output length <= 32
 *   U3: all output chars in [a-z0-9_]
 *   U4: no '.' in output
 *   U5: no '-' in output
 */

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define MAX_STEM 8
#define MAX_OUT 32

bool is_safe(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

void derive(const char *stem, int stem_len, char *out, int *out_len) {
    const char *prefix = "i";
    const char *owner = "maxx";
    int pos = 0;

    while (*prefix && pos < MAX_OUT) out[pos++] = *prefix++;
    while (*owner && pos < MAX_OUT) out[pos++] = *owner++;
    if (pos < MAX_OUT) out[pos++] = '_';

    for (int i = 0; i < stem_len && pos < MAX_OUT; i++) {
        char c = stem[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if (c == '.' || c == '-') c = '_';
        if (is_safe(c)) out[pos++] = c;
    }

    *out_len = pos;
}

int main() {
    char stem[MAX_STEM + 1];
    int stem_len;
    char out[MAX_OUT + 1];
    int out_len;

    __CPROVER_assume(stem_len >= 0 && stem_len <= MAX_STEM);
    for (int i = 0; i < stem_len; i++)
        __CPROVER_assume(stem[i] >= 1 && stem[i] <= 127);
    stem[stem_len] = '\0';

    derive(stem, stem_len, out, &out_len);
    out[out_len] = '\0';

    __CPROVER_assert(out_len > 0, "U1: non-empty");
    __CPROVER_assert(out_len <= MAX_OUT, "U2: length <= 32");

    for (int i = 0; i < out_len; i++)
        __CPROVER_assert(is_safe(out[i]), "U3: safe chars only");

    for (int i = 0; i < out_len; i++) {
        __CPROVER_assert(out[i] != '.', "U4: no dots");
        __CPROVER_assert(out[i] != '-', "U5: no dashes");
    }

    return 0;
}
