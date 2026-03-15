#ifndef SEARCH_H
#define SEARCH_H

void search_init();
void handle_search(int client_sock, const char *filename);

/* Call when CREATE or DELETE happens */
void search_invalidate(const char *filename);

#endif
