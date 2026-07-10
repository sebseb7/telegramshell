#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <pty.h>

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void shell_handle_exit(shell_ctx_t *s);
static void shell_on_read(uv_poll_t *p, int status, int events);

void shell_init(shell_ctx_t *s, app_t *app) {
    s->app = app;
    s->loop = app->loop;
    s->master_fd = -1;
    s->pid = -1;
    s->exited = 0;
}

void shell_start(shell_ctx_t *s) {
    int master = -1, slave = -1;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        perror("openpty");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (ioctl(slave, TIOCSCTTY, 1) < 0)
            perror("TIOCSCTTY");
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        close(master);

        /* Disable echo so typed input (commands, passwords) is not
         * reflected back through the PTY into Telegram output. */
        struct termios t;
        if (tcgetattr(slave, &t) == 0) {
            t.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
            tcsetattr(slave, TCSANOW, &t);
        }

        setenv("TERM", "dumb", 1);

        const char *home = getenv("HOME");
        if (home) {
            if (chdir(home) < 0) {
                perror("chdir");
            }
        }

        execl("/bin/bash", "bash", "-l", (char *)NULL);
        _exit(127);
    }

    close(slave);
    s->master_fd = master;
    s->pid = pid;
    s->exited = 0;
    set_nonblock(master);
    uv_poll_init(s->loop, &s->poll, master);
    s->poll.data = s;
    uv_poll_start(&s->poll, UV_READABLE, shell_on_read);
}

void shell_write(shell_ctx_t *s, const char *data, size_t len) {
    if (s->master_fd < 0)
        return;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(s->master_fd, data + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        } else {
            break;
        }
    }
}

void shell_on_read(uv_poll_t *p, int status, int events) {
    (void)status;
    (void)events;
    shell_ctx_t *s = (shell_ctx_t *)p->data;
    char buf[4096];
    for (;;) {
        ssize_t n = read(s->master_fd, buf, sizeof(buf));
        if (n > 0) {
            bot_on_output(s->app, buf, (size_t)n);
        } else if (n == 0) {
            shell_handle_exit(s);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EIO) {
                shell_handle_exit(s);
                return;
            }
            break;
        }
    }
}

static void shell_handle_exit(shell_ctx_t *s) {
    if (s->exited)
        return;
    s->exited = 1;
    uv_poll_stop(&s->poll);
    if (s->pid > 0) {
        waitpid(s->pid, NULL, 0);
        s->pid = -1;
    }
    if (!s->app->stopping) {
        if (s->app->last_chat_id)
            telegram_send_message(&s->app->tg, s->app->last_chat_id,
                                  "Shell exited. Restarting…", 0, NULL, NULL);
        shell_start(s);
    }
}

void shell_restart(shell_ctx_t *s) {
    if (s->master_fd >= 0) {
        uv_poll_stop(&s->poll);
        close(s->master_fd);
        s->master_fd = -1;
    }
    if (s->pid > 0) {
        kill(s->pid, SIGHUP);
        waitpid(s->pid, NULL, 0);
        s->pid = -1;
    }
    s->exited = 0;
    shell_start(s);
}

void shell_stop(shell_ctx_t *s) {
    if (s->master_fd >= 0) {
        uv_poll_stop(&s->poll);
        close(s->master_fd);
        s->master_fd = -1;
    }
    if (s->pid > 0) {
        kill(s->pid, SIGTERM);
        waitpid(s->pid, NULL, 0);
        s->pid = -1;
    }
}
