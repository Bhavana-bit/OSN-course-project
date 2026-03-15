#include "search.h"
#include "index.h"
#include "logging.h"
#include "errors.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define CACHE_SIZE 32

static char cache_names[CACHE_SIZE][256];
static int  cache_results[CACHE_SIZE];
static int  cache_top = 0;

/* -----------------------------------------
   Initialize Search Cache
----------------------------------------- */
void search_init() {
    cache_top = 0;
    log_event("NM", "CACHE", "Search cache initialized");
}

/* -----------------------------------------
   Invalidate one filename in cache
----------------------------------------- */
void search_invalidate(const char *filename) {
    for (int i = 0; i < cache_top; i++) {
        if (strcmp(cache_names[i], filename) == 0) {

            // Shift elements left to overwrite removed entry
            for (int j = i; j < cache_top - 1; j++) {
                strcpy(cache_names[j], cache_names[j + 1]);
                cache_results[j] = cache_results[j + 1];
            }
            cache_top--;

            char msg[256];
            snprintf(msg, sizeof(msg), "CACHE INVALIDATED for '%s'", filename);
            log_event("NM", "CACHE_INVALIDATE", msg);

            return;
        }
    }
}

/* -----------------------------------------
   Check cache
----------------------------------------- */
static int cache_lookup(const char *filename, int *result) {
    for (int i = 0; i < cache_top; i++) {
        if (strcmp(cache_names[i], filename) == 0) {
            *result = cache_results[i];
            log_event("NM", "CACHE_HIT", filename);
            return 1;
        }
    }
    return 0;
}

/* -----------------------------------------
   Store result in cache
----------------------------------------- */
static void cache_store(const char *filename, int exists) {

    /* If already exists, update it */
    for (int i = 0; i < cache_top; i++) {
        if (strcmp(cache_names[i], filename) == 0) {
            cache_results[i] = exists;
            return;
        }
    }

    /* Insert new */
    if (cache_top < CACHE_SIZE) {
        strcpy(cache_names[cache_top], filename);
        cache_results[cache_top] = exists;
        cache_top++;
    } else {
        /* FIFO eviction */
        for (int i = 1; i < CACHE_SIZE; i++) {
            strcpy(cache_names[i - 1], cache_names[i]);
            cache_results[i - 1] = cache_results[i];
        }
        strcpy(cache_names[CACHE_SIZE - 1], filename);
        cache_results[CACHE_SIZE - 1] = exists;
    }

    log_event("NM", "CACHE_ADD", filename);
}

/* -----------------------------------------
   MAIN SEARCH HANDLER
----------------------------------------- */
void handle_search(int client_sock, const char *filename) {

    if (!filename || strlen(filename) == 0) {
        send_error(client_sock, E400_INVALID_CMD);
        log_event("NM", "ERROR", "SEARCH: Empty filename");
        return;
    }

    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "SEARCH '%s'", filename);
    log_event("NM", "SEARCH", logbuf);

    int exists;

    /* 1. Cache lookup */
    if (cache_lookup(filename, &exists)) {
        if (exists)
            send_error(client_sock, OK200_SUCCESS);
        else
            send_error(client_sock, E404_NOT_FOUND);
        return;
    }

    /* 2. Check index */
    exists = index_exists(filename);

    /* 3. Update cache */
    cache_store(filename, exists);

    /* 4. Send result */
    if (exists) {
        send_error(client_sock, OK200_SUCCESS);
        log_event("NM", "SEARCH_RESULT", "Found");
    } else {
        send_error(client_sock, E404_NOT_FOUND);
        log_event("NM", "SEARCH_RESULT", "Not Found");
    }
}

