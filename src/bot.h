#ifndef BOT_H
#define BOT_H

#include <uv.h>

typedef struct app_s app_t;

typedef struct bot_ctx_s {
    app_t *app;
    char *buf;
    size_t len;
    size_t cap;
    uv_timer_t debounce;
    int debounce_active;
    int strip_state;
} bot_ctx_t;

void bot_init(bot_ctx_t *b, app_t *app);
void bot_handle_message(app_t *app, const char *chat_id, const char *text);
void bot_handle_file(app_t *app, const char *chat_id, const char *file_id,
                     const char *file_name);
void bot_on_output(app_t *app, const char *data, size_t len);
void bot_flush(app_t *app);
void bot_stop(bot_ctx_t *b);

#endif
