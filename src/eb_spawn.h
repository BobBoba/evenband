/* Runs a helper program (file chooser, preset converter) without blocking the interface:
 * start it, poll from the interface's timer, read what it printed when it ends. No shell is
 * involved, so file names are never interpreted. */
#ifndef EB_SPAWN_H
#define EB_SPAWN_H

#include <stddef.h>

typedef struct {
    int fd;                 /* read end of the child's stdout, -1 when idle */
    int pid;
    char out[1024];
    size_t len;
} eb_spawn;

/* Starts argv[0] (looked up on PATH). Returns 0 when the process was created. */
int eb_spawn_start(eb_spawn *s, char *const argv[]);

/* Returns 0 while running, 1 when finished (status = exit code, 127 if the program does not
 * exist; output = what it printed without the trailing newline). */
int eb_spawn_poll(eb_spawn *s, int *status, char *output, size_t cap);

/* Stops a running helper. */
void eb_spawn_abort(eb_spawn *s);

#endif
