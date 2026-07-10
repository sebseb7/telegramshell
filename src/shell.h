#ifndef SHELL_H
#define SHELL_H

#include <uv.h>
#include <sys/types.h>

typedef struct app_s app_t;

typedef struct shell_ctx_s {
    app_t *app;
    uv_loop_t *loop;
    int master_fd;
    uv_poll_t poll;
    pid_t pid;
    int exited;
} shell_ctx_t;

void shell_init(shell_ctx_t *s, app_t *app);
void shell_start(shell_ctx_t *s);
void shell_write(shell_ctx_t *s, const char *data, size_t len);
void shell_restart(shell_ctx_t *s);
void shell_stop(shell_ctx_t *s);

#endif
