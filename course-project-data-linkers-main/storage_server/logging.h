#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

void log_init(const char *filename);
void log_close();
void log_event(const char *component,
               const char *event_type,
               const char *detail);

char *timestamp();

#endif
