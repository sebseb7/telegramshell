#ifndef TELEGRAM_H
#define TELEGRAM_H

#include "curl_uv.h"

typedef struct app_s app_t;

typedef struct telegram_ctx_s {
    app_t *app;
    curl_uv_t *cu;
    char *api_base;
    char *file_base;
    long offset;
    int polling;
    uv_timer_t retry_timer;
    int retry_timer_active;
    int backoff;
} telegram_ctx_t;

void telegram_init(telegram_ctx_t *tg, curl_uv_t *cu, const char *token, app_t *app);
void telegram_start_polling(telegram_ctx_t *tg);
void telegram_send_message(telegram_ctx_t *tg, const char *chat_id,
                           const char *text, int code_block,
                           void (*cb)(void *, int), void *ud);
void telegram_set_commands(telegram_ctx_t *tg);

/* Download the file identified by file_id into dest_path under the user's
 * home directory, then confirm via chat. file_name may be NULL (photos). */
void telegram_receive_file(telegram_ctx_t *tg, const char *chat_id,
                           const char *file_id, const char *file_name);
/* Send a local file back to chat_id as a Telegram document. */
void telegram_send_document(telegram_ctx_t *tg, const char *chat_id,
                            const char *local_path,
                            void (*cb)(void *, int), void *ud);

#endif
