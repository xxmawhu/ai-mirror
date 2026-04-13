/*
 * CBMC Formal Verification Harness: Username Derivation Safety
 *
 * Verifies that generate_username produces safe Linux usernames:
 *   U1: Only contains [a-z0-9_]
 *   U2: Length <= 32
 *   U3: Special chars ('.', '-') are replaced with '_'
 *   U4: All chars are lowercase
 */

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_USER_LEN 32
#define MAX_INPUT_LEN 16

bool is_safe_username_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

void generate_username(const char *prefix, int prefix_len,
                       const char *owner, int owner_len,
                       const char *stem, int stem_len,
                       char *output, int *output_len) {
    int pos = 0;
    int max_len = MAX_USER_LEN;

    for (int i = 0; i < prefix_len && pos < max_len; i++) {
        char c = prefix[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        output[pos++] = c;
    }

    for (int i = 0; i < owner_len && pos < max_len; i++) {
        char c = owner[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        output[pos++] = c;
    }

    output[pos++] = '_';

    for (int i = 0; i < stem_len && pos < max_len; i++) {
        char c = stem[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if (c == '.' || c == '-') c = '_';
        if (c >= 'a' && c <= 'z') output[pos++] = c;
        else if (c >= '0' && c <= '9') output[pos++] = c;
        else if (c == '_') output[pos++] = c;
    }

    *output_len = pos;
}

void main() {
    char prefix[4];
    char owner[MAX_INPUT_LEN];
    char stem[MAX_INPUT_LEN];
    char output[MAX_USER_LEN + 1];
    int prefix_len, owner_len, stem_len, output_len;

    __CPROVER_assume(prefix_len >= 1 && prefix_len <= 3);
    __CPROVER_assume(owner_len >= 1 && owner_len < MAX_INPUT_LEN);
    __CPROVER_assume(stem_len >= 1 && stem_len < MAX_INPUT_LEN);

    for (int i = 0; i < prefix_len; i++)
        __CPROVER_assume(prefix[i] >= 'a' && prefix[i] <= 'z');
    prefix[prefix_len] = '\0';

    for (int i = 0; i < owner_len; i++)
        __CPROVER_assume(owner[i] >= 'a' && owner[i] <= 'z');
    owner[owner_len] = '\0';

    for (int i = 0; i < stem_len; i++)
        __CPROVER_assume(stem[i] >= 1 && stem[i] <= 127);
    stem[stem_len] = '\0';

    generate_username(prefix, prefix_len, owner, owner_len,
                      stem, stem_len, output, &output_len);
    output[output_len] = '\0';

    __CPROVER_assert(output_len > 0, "U1: username is non-empty");
    __CPROVER_assert(output_len <= MAX_USER_LEN, "U2: length <= 32");

    for (int i = 0; i < output_len; i++) {
        __CPROVER_assert(is_safe_username_char(output[i]),
                         "U3: all chars in [a-z0-9_]");
    }

    int dot_count = 0, dash_count = 0;
    for (int i = 0; i < stem_len; i++) {
        if (stem[i] == '.') dot_count++;
        if (stem[i] == '-') dash_count++;
    }
    int output_dot = 0, output_dash = 0;
    for (int i = 0; i < output_len; i++) {
        if (output[i] == '.') output_dot++;
        if (output[i] == '-') output_dash++;
    }
    __CPROVER_assert(output_dot == 0, "U4: no dots in output");
    __CPROVER_assert(output_dash == 0, "U5: no dashes in output");
}
