#include "eb_spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int eb_spawn_start(eb_spawn *s, char *const argv[])
{
    memset(s, 0, sizeof *s);
    s->fd = -1;
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    s->fd = pipefd[0];
    s->pid = (int)pid;
    return 0;
}

int eb_spawn_poll(eb_spawn *s, int *status, char *output, size_t cap)
{
    if (s->fd < 0) return 0;
    for (;;) {
        char tmp[256];
        ssize_t n = read(s->fd, tmp, sizeof tmp);
        if (n > 0) {
            size_t room = sizeof s->out - 1 - s->len;
            size_t take = (size_t)n < room ? (size_t)n : room;
            memcpy(s->out + s->len, tmp, take);
            s->len += take;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        break;                                       /* end of file: the child closed its output */
    }
    int st = 0;
    waitpid(s->pid, &st, 0);
    close(s->fd);
    s->fd = -1;
    s->out[s->len] = '\0';
    while (s->len && (s->out[s->len - 1] == '\n' || s->out[s->len - 1] == '\r')) s->out[--s->len] = '\0';
    if (output && cap) snprintf(output, cap, "%s", s->out);
    if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    return 1;
}

void eb_spawn_abort(eb_spawn *s)
{
    if (s->fd < 0) return;
    kill(s->pid, SIGTERM);
    waitpid(s->pid, NULL, 0);
    close(s->fd);
    s->fd = -1;
}
