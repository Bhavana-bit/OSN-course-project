#ifndef INDEX_H
#define INDEX_H

#include "errors.h"

#define INDEX_SIZE 2048

/* Initialize index table */
void index_init();

/* Add a filename to the index (returns error-string or OK200_SUCCESS) */
const char *index_add(const char *filename);

/* Remove a filename (returns error-string or OK200_SUCCESS) */
const char *index_remove(const char *filename);

/* Check existence (1 = exists, 0 = no) */
int index_exists(const char *filename);

#endif
