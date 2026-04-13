/*
 * CBMC Formal Verification Harness: is_subpath()
 *
 * Theorem: is_subpath(parent, child) == true  iff  child == parent
 *          or child starts with parent + "/"
 *
 * Properties verified:
 *   P1: Identical paths => is_subpath = true
 *   P2: child = parent + "/" + suffix => is_subpath = true
 *   P3: child shorter than parent => is_subpath = false
 *   P4: child with different root => is_subpath = false
 *   P5: parent="/a", child="/ab" (partial match) => is_subpath = false
 *   P6: No false positives: any accepted path truly starts with parent
 */

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define MAX_PATH_LEN 128

bool is_subpath(const char *parent, int parent_len,
                const char *child, int child_len) {
    if (parent_len == child_len) {
        return memcmp(parent, child, parent_len) == 0;
    }

    if (child_len > parent_len) {
        if (parent_len == 0) return false;
        if (memcmp(parent, child, parent_len) != 0) return false;
        if (child[parent_len] != '/') return false;
        return true;
    }

    return false;
}

void main() {
    char parent[MAX_PATH_LEN];
    char child[MAX_PATH_LEN];
    int parent_len, child_len;

    __CPROVER_assume(parent_len >= 0 && parent_len < MAX_PATH_LEN);
    __CPROVER_assume(child_len >= 0 && child_len < MAX_PATH_LEN);

    for (int i = 0; i < MAX_PATH_LEN; i++) {
        __CPROVER_assume(parent[i] >= 1 && parent[i] <= 127);
        __CPROVER_assume(child[i] >= 1 && child[i] <= 127);
    }
    parent[parent_len] = '\0';
    child[child_len] = '\0';

    bool result = is_subpath(parent, parent_len, child, child_len);

    // P1: Identical paths => accepted
    if (parent_len == child_len && memcmp(parent, child, parent_len) == 0) {
        __CPROVER_assert(result == true, "P1: identical paths accepted");
    }

    // P2: child = parent + "/" + suffix => accepted
    if (child_len > parent_len && parent_len > 0 &&
        memcmp(parent, child, parent_len) == 0 &&
        child[parent_len] == '/') {
        __CPROVER_assert(result == true, "P2: proper child accepted");
    }

    // P3: child shorter than parent => rejected
    if (child_len < parent_len) {
        __CPROVER_assert(result == false, "P3: shorter child rejected");
    }

    // P4: different prefix => rejected
    if (parent_len > 0 && child_len > 0 && parent[0] != child[0]) {
        __CPROVER_assert(result == false, "P4: different root rejected");
    }

    // P5: partial match without '/' separator => rejected
    // parent="/a" child="/ab" => prefix matches but child[parent_len] != '/'
    if (child_len > parent_len && parent_len > 0 &&
        memcmp(parent, child, parent_len) == 0 &&
        child[parent_len] != '/') {
        __CPROVER_assert(result == false, "P5: partial match rejected");
    }

    // P6: Soundness - if accepted, relationship holds
    if (result) {
        if (parent_len == child_len) {
            __CPROVER_assert(memcmp(parent, child, parent_len) == 0,
                             "P6a: same-length acceptance implies equality");
        } else {
            __CPROVER_assert(child_len > parent_len, "P6b: acceptance implies child >= parent");
            __CPROVER_assert(memcmp(parent, child, parent_len) == 0,
                             "P6c: acceptance implies prefix match");
            __CPROVER_assert(child[parent_len] == '/',
                             "P6d: acceptance implies '/' separator");
        }
    }
}
