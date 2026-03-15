#include "index.h"
#include "logging.h"
#include <string.h>
#include <stdio.h>

/* Hash Table */
static char table[INDEX_SIZE][256];
static int used[INDEX_SIZE];

/* Simple DJB2 hash */
static unsigned long hash(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = *s++))
        h = ((h << 5) + h) + c;
    return h;
}

/* -----------------------------------------
   Initialize index
----------------------------------------- */
void index_init() {
    memset(used, 0, sizeof(used));
    log_event("NM", "INDEX", "Index initialized");
}

/* -----------------------------------------
   Add filename to hash table
----------------------------------------- */
const char *index_add(const char *filename) {

    if (!filename || strlen(filename) == 0) {
        log_event("NM", "INDEX_ERR", "Attempt to add empty filename");
        return E400_INVALID_CMD;
    }

    unsigned long h = hash(filename) % INDEX_SIZE;

    /* Check duplicates */
    for (int i = 0; i < INDEX_SIZE; i++) {
        int idx = (h + i) % INDEX_SIZE;

        if (used[idx] && strcmp(table[idx], filename) == 0) {
            log_event("NM", "INDEX_DUP", filename);
            return E409_CONFLICT;
        }
    }

    /* Insert */
    for (int i = 0; i < INDEX_SIZE; i++) {
        int idx = (h + i) % INDEX_SIZE;

        if (!used[idx]) {
            strcpy(table[idx], filename);
            used[idx] = 1;

            char msg[256];
            snprintf(msg, sizeof(msg), "Indexed file '%s'", filename);
            log_event("NM", "INDEX_ADD", msg);

            return OK200_SUCCESS;
        }
    }

    log_event("NM", "INDEX_FULL", "Index overflow");
    return E500_INTERNAL;
}

/* -----------------------------------------
   Remove filename from table
----------------------------------------- */
const char *index_remove(const char *filename) {

    if (!filename || strlen(filename) == 0) {
        log_event("NM", "INDEX_ERR", "Attempt to remove empty filename");
        return E400_INVALID_CMD;
    }

    unsigned long h = hash(filename) % INDEX_SIZE;

    for (int i = 0; i < INDEX_SIZE; i++) {
        int idx = (h + i) % INDEX_SIZE;

        if (used[idx] && strcmp(table[idx], filename) == 0) {
            used[idx] = 0;

            char msg[256];
            snprintf(msg, sizeof(msg), "Removed '%s' from index", filename);
            log_event("NM", "INDEX_REMOVE", msg);

            return OK200_SUCCESS;
        }
    }

    log_event("NM", "INDEX_MISS", filename);
    return E404_NOT_FOUND;
}

/* -----------------------------------------
   Check if file exists in index
----------------------------------------- */
int index_exists(const char *filename) {

    if (!filename || strlen(filename) == 0)
        return 0;

    unsigned long h = hash(filename) % INDEX_SIZE;

    for (int i = 0; i < INDEX_SIZE; i++) {
        int idx = (h + i) % INDEX_SIZE;

        if (used[idx] && strcmp(table[idx], filename) == 0)
            return 1;
    }

    return 0;
}
