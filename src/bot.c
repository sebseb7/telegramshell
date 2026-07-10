#define _GNU_SOURCE
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "totp.h"

#define FLUSH_THRESHOLD 4000
#define DEBOUNCE_MS 300

enum {
    ST_NORMAL = 0,
    ST_ESC,
    ST_CSI,
    ST_OSC,
    ST_CONSUME1
};

/* Stateful ANSI/terminal escape stripper. Returns the number of bytes written
 * to out (out must be at least in_len bytes). State is preserved across calls
 * so sequences split across chunks are handled correctly. */
static size_t strip_ansi(int *state, const char *in, size_t n,
                         char *out, size_t outcap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (*state) {
        case ST_NORMAL:
            if (c == 0x1b) {
                *state = ST_ESC;
            } else if (c < 0x20) {
                if (c == '\n' || c == '\r' || c == '\t') {
                    if (o < outcap) out[o++] = (char)c;
                }
            } else if (c == 0x7f) {
                /* drop DEL */
            } else {
                if (o < outcap) out[o++] = (char)c;
            }
            break;
        case ST_ESC:
            if (c == '[')
                *state = ST_CSI;
            else if (c == ']')
                *state = ST_OSC;
            else if (c == '(' || c == ')' || c == '*' || c == '+' ||
                     c == '#' || c == '=' || c == '>')
                *state = ST_CONSUME1;
            else
                *state = ST_NORMAL; /* drop ESC and this byte */
            break;
        case ST_CSI:
            if (c >= 0x40 && c <= 0x7e)
                *state = ST_NORMAL; /* terminator consumed */
            break;
        case ST_OSC:
            if (c == 0x07)
                *state = ST_NORMAL; /* BEL terminator */
            else if (c == 0x1b)
                *state = ST_ESC; /* possible ST (ESC \) */
            break;
        case ST_CONSUME1:
            *state = ST_NORMAL; /* drop this byte */
            break;
        }
    }
    return o;
}

void bot_init(bot_ctx_t *b, app_t *app) {
    b->app = app;
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
    b->debounce_active = 0;
    b->strip_state = ST_NORMAL;
    uv_timer_init(app->loop, &b->debounce);
    b->debounce.data = b;
}

static void bot_debounce_cb(uv_timer_t *t) {
    bot_ctx_t *b = (bot_ctx_t *)t->data;
    bot_flush(b->app);
}

static void bot_ensure(bot_ctx_t *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap)
            b->cap = b->cap ? b->cap * 2 : 8192;
        b->buf = (char *)realloc(b->buf, b->cap);
    }
}

void bot_on_output(app_t *app, const char *data, size_t len) {
    bot_ctx_t *b = &app->bot;
    char *clean = (char *)malloc(len ? len : 1);
    size_t clen = strip_ansi(&b->strip_state, data, len, clean, len);
    if (clen == 0) {
        free(clean);
        return;
    }
    bot_ensure(b, clen);
    memcpy(b->buf + b->len, clean, clen);
    b->len += clen;
    b->buf[b->len] = '\0';
    free(clean);

    if (b->len >= FLUSH_THRESHOLD) {
        bot_flush(app);
    } else {
        uv_timer_start(&b->debounce, bot_debounce_cb, DEBOUNCE_MS, 0);
        b->debounce_active = 1;
    }
}

void bot_flush(app_t *app) {
    bot_ctx_t *b = &app->bot;
    if (b->debounce_active) {
        uv_timer_stop(&b->debounce);
        b->debounce_active = 0;
    }
    if (b->len == 0)
        return;

    if (!app->last_chat_id) {
        b->len = 0;
        return;
    }

    telegram_send_message(&app->tg, app->last_chat_id, b->buf, 1, NULL, NULL);
    b->len = 0;
}

void bot_handle_message(app_t *app, const char *chat_id, const char *text) {
    int allowed = 0;
    if (app->allowed_chat_id && strcmp(chat_id, app->allowed_chat_id) == 0) {
        allowed = 1;
    }

    if (!allowed) {
        fprintf(stderr, "Unauthorized message from chat_id=%s (text: %s)\n",
                chat_id, text);
        telegram_send_message(&app->tg, chat_id, chat_id, 0, NULL, NULL);
        return;
    }

    if (app->totp_secret && strlen(app->totp_secret) > 0) {
        if (strlen(text) < 6 || !totp_verify(app->totp_secret, text)) {
            fprintf(stderr, "Invalid or missing TOTP code from chat_id=%s\n", chat_id);
            telegram_send_message(&app->tg, chat_id, "Invalid or missing TOTP code.", 0, NULL, NULL);
            return;
        }
        text += 6;
        while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
            text++;
        }
    }

    free(app->last_chat_id);
    app->last_chat_id = strdup(chat_id);
    app->pending_pid_send = 1;

    if (strcmp(text, "/start") == 0) {
        telegram_send_message(&app->tg, chat_id,
            "Telegram shell bot.\n"
            "Send shell commands and I will stream output back here.\n"
            "/restart — restart the shell session.\n"
            "/ctrl_c — send Ctrl-C (SIGINT) to the shell.\n"
            "/ctrl_z — send Ctrl-Z (SIGTSTP) to the shell.", 0, NULL, NULL);
        return;
    }

    if (strcmp(text, "/restart") == 0) {
        shell_restart(&app->shell);
        telegram_send_message(&app->tg, chat_id, "Shell restarted.", 0, NULL, NULL);
        return;
    }

    if (strcmp(text, "/ctrl_c") == 0) {
        shell_write(&app->shell, "\x03", 1); /* INTR -> SIGINT */
        telegram_send_message(&app->tg, chat_id, "Sent Ctrl-C (SIGINT).", 0, NULL, NULL);
        return;
    }

    if (strcmp(text, "/ctrl_z") == 0) {
        shell_write(&app->shell, "\x1a", 1); /* SUSP -> SIGTSTP */
        telegram_send_message(&app->tg, chat_id, "Sent Ctrl-Z (SIGTSTP).", 0, NULL, NULL);
        return;
    }

    if (strncmp(text, "/download", 9) == 0) {
        const char *arg = text + 9;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        if (*arg == '\0') {
            telegram_send_message(&app->tg, chat_id,
                                  "Usage: /download <filename>", 0, NULL, NULL);
        } else {
            const char *b = strrchr(arg, '/');
            b = b ? b + 1 : arg;
            size_t alen = strlen(b);
            while (alen > 0 && (b[alen - 1] == ' ' || b[alen - 1] == '\t' ||
                                b[alen - 1] == '\r' || b[alen - 1] == '\n'))
                alen--;
            const char *home = getenv("HOME");
            char path[4096];
            snprintf(path, sizeof(path), "%s/%.*s", home ? home : ".",
                     (int)alen, b);
            telegram_send_document(&app->tg, chat_id, path, NULL, NULL);
        }
        return;
    }

    size_t n = strlen(text) + 2;
    char *line = (char *)malloc(n);
    snprintf(line, n, "%s\n", text);
    shell_write(&app->shell, line, strlen(line));
    free(line);
}

void bot_handle_file(app_t *app, const char *chat_id, const char *file_id,
                     const char *file_name) {
    int allowed = 0;
    if (app->allowed_chat_id && strcmp(chat_id, app->allowed_chat_id) == 0) {
        allowed = 1;
    }

    if (!allowed) {
        fprintf(stderr, "Unauthorized file from chat_id=%s\n", chat_id);
        telegram_send_message(&app->tg, chat_id, chat_id, 0, NULL, NULL);
        return;
    }
    app->pending_pid_send = 1;
    telegram_receive_file(&app->tg, chat_id, file_id, file_name);
}

void bot_stop(bot_ctx_t *b) {
    if (b->debounce_active) {
        uv_timer_stop(&b->debounce);
        b->debounce_active = 0;
    }
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}
