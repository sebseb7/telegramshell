#ifndef APP_H
#define APP_H

#include "curl_uv.h"
#include "telegram.h"
#include "shell.h"
#include "bot.h"

typedef struct app_s {
    uv_loop_t *loop;
    curl_uv_t curl_uv;
    telegram_ctx_t tg;
    shell_ctx_t shell;
    bot_ctx_t bot;
    const char *token;
    const char *allowed_chat_id;
    const char *totp_secret;
    char *last_chat_id;
    int stopping;
    int exit_code;
    int pending_pid_send;
    pid_t last_sent_pid;
} app_t;

extern app_t g_app;

#endif
