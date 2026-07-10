#define _GNU_SOURCE
#include "app.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static char *urlencode(const char *in) {
    static const char *hex = "0123456789ABCDEF";
    size_t len = strlen(in);
    char *out = (char *)malloc(len * 3 + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = c;
        } else {
            out[j++] = '%';
            out[j++] = hex[(c >> 4) & 0xF];
            out[j++] = hex[c & 0xF];
        }
    }
    out[j] = '\0';
    return out;
}

static void telegram_poll_cb(curl_req_t *req, CURLcode code, long http_code,
                             char *data, size_t len, void *ud);

void telegram_init(telegram_ctx_t *tg, curl_uv_t *cu, const char *token, app_t *app) {
    tg->app = app;
    tg->cu = cu;
    tg->offset = 0;
    tg->polling = 0;
    tg->backoff = 0;
    tg->retry_timer_active = 0;
    uv_timer_init(app->loop, &tg->retry_timer);
    tg->retry_timer.data = tg;
    size_t n = strlen("https://api.telegram.org/bot") + strlen(token) + 2;
    tg->api_base = (char *)malloc(n);
    snprintf(tg->api_base, n, "https://api.telegram.org/bot%s/", token);

    size_t fn = strlen("https://api.telegram.org/file/bot") + strlen(token) + 2;
    tg->file_base = (char *)malloc(fn);
    snprintf(tg->file_base, fn, "https://api.telegram.org/file/bot%s/", token);
}

static void poll_retry_cb(uv_timer_t *t) {
    telegram_ctx_t *tg = (telegram_ctx_t *)t->data;
    tg->retry_timer_active = 0;
    telegram_start_polling(tg);
}

/* failed != 0: a poll failed, so back off before retrying.
 * failed == 0: a successful long-poll cycle, re-poll immediately. */
static void schedule_repoll(telegram_ctx_t *tg, int failed) {
    if (tg->app->stopping)
        return;
    if (!failed) {
        tg->backoff = 0;
        telegram_start_polling(tg);
        return;
    }
    if (tg->backoff < 1)
        tg->backoff = 1;
    else if (tg->backoff < 30)
        tg->backoff *= 2;
    if (tg->backoff > 30)
        tg->backoff = 30;
    fprintf(stderr, "Reconnecting in %ds...\n", tg->backoff);
    uv_timer_start(&tg->retry_timer, poll_retry_cb, (unsigned int)tg->backoff * 1000, 0);
    tg->retry_timer_active = 1;
}

void telegram_start_polling(telegram_ctx_t *tg) {
    if (tg->polling || tg->app->stopping)
        return;
    tg->polling = 1;
    char url[600];
    snprintf(url, sizeof(url), "%sgetUpdates?offset=%ld&timeout=30", tg->api_base, tg->offset);
    curl_uv_request(tg->cu, url, NULL, NULL, telegram_poll_cb, tg);
}

static void telegram_poll_cb(curl_req_t *req, CURLcode code, long http_code,
                             char *data, size_t len, void *ud) {
    (void)req;
    (void)http_code;
    telegram_ctx_t *tg = (telegram_ctx_t *)ud;
    tg->polling = 0;

    if (code != CURLE_OK) {
        fprintf(stderr, "getUpdates failed: %s\n", curl_easy_strerror(code));
        schedule_repoll(tg, 1);
        return;
    }

    yyjson_doc *doc = yyjson_read(data, len, 0);
    if (!doc) {
        fprintf(stderr, "getUpdates: failed to parse JSON\n");
        schedule_repoll(tg, 1);
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *okv = yyjson_obj_get(root, "ok");
    if (!yyjson_is_true(okv)) {
        long errcode = yyjson_get_int(yyjson_obj_get(root, "error_code"));
        const char *desc = yyjson_get_str(yyjson_obj_get(root, "description"));
        fprintf(stderr, "Telegram error %ld: %s\n", errcode, desc ? desc : "");
        if (errcode == 401) {
            fprintf(stderr, "Fatal: invalid or unauthorized bot token.\n");
            yyjson_doc_free(doc);
            tg->app->stopping = 1;
            tg->app->exit_code = 1;
            uv_stop(tg->app->loop);
            return;
        }
        yyjson_doc_free(doc);
        schedule_repoll(tg, 1);
        return;
    }

    yyjson_val *results = yyjson_obj_get(root, "result");
    if (yyjson_is_arr(results)) {
        yyjson_arr_iter it;
        yyjson_val *upd;
        yyjson_arr_iter_init(results, &it);
        while ((upd = yyjson_arr_iter_next(&it))) {
            long uid = yyjson_get_int(yyjson_obj_get(upd, "update_id"));
            if (uid + 1 > tg->offset)
                tg->offset = uid + 1;

            yyjson_val *msg = yyjson_obj_get(upd, "message");
            if (!msg)
                continue;
            yyjson_val *chat = yyjson_obj_get(msg, "chat");
            if (!chat)
                continue;
            long cid = yyjson_get_int(yyjson_obj_get(chat, "id"));
            char cidbuf[32];
            snprintf(cidbuf, sizeof(cidbuf), "%ld", cid);

            /* Files: document or photo. */
            yyjson_val *docv = yyjson_obj_get(msg, "document");
            if (docv) {
                const char *fid = yyjson_get_str(yyjson_obj_get(docv, "file_id"));
                const char *fname = yyjson_get_str(yyjson_obj_get(docv, "file_name"));
                if (fid) {
                    bot_handle_file(tg->app, cidbuf, fid, fname);
                    continue;
                }
            }
            yyjson_val *photov = yyjson_obj_get(msg, "photo");
            if (photov && yyjson_is_arr(photov) && yyjson_arr_size(photov) > 0) {
                yyjson_val *p = yyjson_arr_get(photov, yyjson_arr_size(photov) - 1);
                const char *fid = yyjson_get_str(yyjson_obj_get(p, "file_id"));
                if (fid) {
                    bot_handle_file(tg->app, cidbuf, fid, NULL);
                    continue;
                }
            }

            yyjson_val *textv = yyjson_obj_get(msg, "text");
            if (!textv)
                continue;
            const char *text = yyjson_get_str(textv);
            if (!text)
                continue;
            bot_handle_message(tg->app, cidbuf, text);
        }
    }

    yyjson_doc_free(doc);
    schedule_repoll(tg, 0);
}

typedef struct send_ctx_s {
    void (*cb)(void *, int);
    void *ud;
} send_ctx_t;

static void telegram_send_cb(curl_req_t *req, CURLcode code, long http_code,
                             char *data, size_t len, void *ud) {
    (void)req;
    (void)http_code;
    (void)data;
    (void)len;
    send_ctx_t *sc = (send_ctx_t *)ud;
    if (sc->cb)
        sc->cb(sc->ud, (int)code);
    free(sc);
}

/* --- File receive / send --- */

typedef struct recv_file_ctx_s {
    telegram_ctx_t *tg;
    char *chat_id;
    char *dest_path;
} recv_file_ctx_t;

static void recv_free(recv_file_ctx_t *c) {
    free(c->chat_id);
    free(c->dest_path);
    free(c);
}

/* Keep only the basename of in (drop any directory components). */
static void safe_basename(const char *in, char *out, size_t outsz,
                          const char *fallback) {
    const char *base = in ? strrchr(in, '/') : NULL;
    base = base ? base + 1 : in;
    if (!base || *base == '\0')
        base = fallback;
    size_t i = 0;
    while (base[i] && i + 1 < outsz) {
        out[i] = base[i];
        i++;
    }
    out[i] = '\0';
}

static void telegram_download_file(telegram_ctx_t *tg, const char *file_path,
                                    const char *dest_path, curl_req_cb cb, void *ud);

static void recv_fail(recv_file_ctx_t *c, const char *why) {
    fprintf(stderr, "File receive failed (%s) for chat_id=%s\n", why, c->chat_id);
    telegram_send_message(c->tg, c->chat_id, "Failed to receive file.", 0, NULL, NULL);
    recv_free(c);
}

static void recv_download_cb(curl_req_t *req, CURLcode code, long http_code,
                             char *data, size_t len, void *ud) {
    (void)req;
    (void)http_code;
    (void)data;
    (void)len;
    recv_file_ctx_t *c = (recv_file_ctx_t *)ud;
    if (code == CURLE_OK) {
        const char *base = strrchr(c->dest_path, '/');
        base = base ? base + 1 : c->dest_path;
        char msg[320];
        snprintf(msg, sizeof(msg), "Saved file as %s (~/%s)", base, base);
        telegram_send_message(c->tg, c->chat_id, msg, 0, NULL, NULL);
    } else {
        telegram_send_message(c->tg, c->chat_id, "Failed to save file.", 0, NULL, NULL);
    }
    recv_free(c);
}

static void recv_getfile_cb(curl_req_t *req, CURLcode code, long http_code,
                            char *data, size_t len, void *ud) {
    (void)req;
    (void)http_code;
    recv_file_ctx_t *c = (recv_file_ctx_t *)ud;
    if (code != CURLE_OK) {
        recv_fail(c, "getFile network error");
        return;
    }
    yyjson_doc *doc = yyjson_read(data, len, 0);
    if (!doc) {
        recv_fail(c, "getFile bad response");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_true(yyjson_obj_get(root, "ok"))) {
        yyjson_doc_free(doc);
        recv_fail(c, "getFile api error");
        return;
    }
    yyjson_val *res = yyjson_obj_get(root, "result");
    const char *fp = yyjson_get_str(yyjson_obj_get(res, "file_path"));
    if (!fp) {
        yyjson_doc_free(doc);
        recv_fail(c, "no file_path");
        return;
    }
    char *fp_dup = strdup(fp);
    yyjson_doc_free(doc);
    telegram_download_file(c->tg, fp_dup, c->dest_path, recv_download_cb, c);
    free(fp_dup);
}

void telegram_receive_file(telegram_ctx_t *tg, const char *chat_id,
                           const char *file_id, const char *file_name) {
    char name[256];
    if (!file_name)
        snprintf(name, sizeof(name), "photo_%s.jpg", file_id);
    else
        safe_basename(file_name, name, sizeof(name), "file");

    const char *home = getenv("HOME");
    char *dest = (char *)malloc(strlen(home ? home : ".") + strlen(name) + 2);
    sprintf(dest, "%s/%s", home ? home : ".", name);

    recv_file_ctx_t *c = (recv_file_ctx_t *)malloc(sizeof(*c));
    c->tg = tg;
    c->chat_id = strdup(chat_id);
    c->dest_path = dest;

    char *enc = urlencode(file_id);
    char url[700];
    snprintf(url, sizeof(url), "%sgetFile?file_id=%s", tg->api_base, enc);
    free(enc);
    curl_uv_request(tg->cu, url, NULL, NULL, recv_getfile_cb, c);
}

void telegram_download_file(telegram_ctx_t *tg, const char *file_path,
                            const char *dest_path, curl_req_cb cb, void *ud) {
    char *url = (char *)malloc(strlen(tg->file_base) + strlen(file_path) + 1);
    strcpy(url, tg->file_base);
    strcat(url, file_path);
    curl_uv_download(tg->cu, url, dest_path, cb, ud);
    free(url);
}

typedef struct send_doc_s {
    const char *chat_id;
    const char *path;
} send_doc_t;

static void build_doc_mime(curl_mime *mime, void *ud) {
    send_doc_t *d = (send_doc_t *)ud;
    curl_mimepart *part;
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, d->chat_id, CURL_ZERO_TERMINATED);
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "document");
    curl_mime_filedata(part, d->path);
}

void telegram_send_document(telegram_ctx_t *tg, const char *chat_id,
                            const char *local_path,
                            void (*cb)(void *, int), void *ud) {
    char *url = (char *)malloc(strlen(tg->api_base) + 16);
    snprintf(url, strlen(tg->api_base) + 16, "%ssendDocument", tg->api_base);

    send_doc_t d = {chat_id, local_path};
    send_ctx_t *sc = (send_ctx_t *)malloc(sizeof(*sc));
    sc->cb = cb;
    sc->ud = ud;

    curl_uv_request_mime(tg->cu, url, build_doc_mime, &d, telegram_send_cb, sc);
    free(url);
}

/* Escape the minimal HTML entities so the text is safe inside <pre>. */
static char *html_escape(const char *in) {
    size_t len = strlen(in);
    char *out = (char *)malloc(len * 4 + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '<') {
            memcpy(out + j, "&lt;", 4);
            j += 4;
        } else if (c == '>') {
            memcpy(out + j, "&gt;", 4);
            j += 4;
        } else if (c == '&') {
            memcpy(out + j, "&amp;", 5);
            j += 5;
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return out;
}

static void send_one(telegram_ctx_t *tg, const char *chat_id,
                     const char *body, int code_block,
                     void (*cb)(void *, int), void *ud) {
    char *url = (char *)malloc(strlen(tg->api_base) + 16);
    snprintf(url, strlen(tg->api_base) + 16, "%ssendMessage", tg->api_base);

    char *enc_chat = urlencode(chat_id);
    char *enc_text = urlencode(body);
    size_t plen = strlen(enc_chat) + strlen(enc_text) + 48;
    char *post = (char *)malloc(plen);
    if (code_block)
        snprintf(post, plen, "chat_id=%s&text=%s&parse_mode=HTML", enc_chat, enc_text);
    else
        snprintf(post, plen, "chat_id=%s&text=%s", enc_chat, enc_text);

    send_ctx_t *sc = (send_ctx_t *)malloc(sizeof(*sc));
    sc->cb = cb;
    sc->ud = ud;

    curl_uv_request(tg->cu, url, post, NULL, telegram_send_cb, sc);

    free(url);
    free(enc_chat);
    free(enc_text);
    free(post);
}

void telegram_send_message(telegram_ctx_t *tg, const char *chat_id,
                           const char *text, int code_block,
                           void (*cb)(void *, int), void *ud) {
    if (!chat_id || !text || !*text)
        return;

    char *allocated_text = NULL;
    pid_t current_pid = tg->app->shell.pid;
    if (current_pid > 0 &&
        tg->app->allowed_chat_id &&
        strcmp(chat_id, tg->app->allowed_chat_id) == 0 &&
        (tg->app->pending_pid_send || current_pid != tg->app->last_sent_pid)) {
        size_t pid_len = snprintf(NULL, 0, "[PID: %d]\n", (int)current_pid);
        allocated_text = (char *)malloc(pid_len + strlen(text) + 1);
        if (allocated_text) {
            sprintf(allocated_text, "[PID: %d]\n%s", (int)current_pid, text);
            text = allocated_text;
        }
        tg->app->pending_pid_send = 0;
        tg->app->last_sent_pid = current_pid;
    }

    if (!code_block) {
        send_one(tg, chat_id, text, 0, cb, ud);
        free(allocated_text);
        return;
    }

    char *esc = html_escape(text);
    size_t elen = strlen(esc);
    const size_t MAX = 4080;
    size_t i = 0;
    int first = 1;
    while (i < elen) {
        size_t end = i + MAX;
        if (end >= elen) {
            end = elen;
        } else {
            size_t e = end;
            /* Avoid splitting UTF-8 characters */
            while (e > i && ((unsigned char)esc[e] & 0xC0) == 0x80)
                e--;
            /* Avoid splitting HTML entities */
            size_t he = e;
            while (he > i && esc[he] != '&' && esc[he] != ';' && (e - he) < 10)
                he--;
            if (he > i && esc[he] == '&')
                e = he;
            end = (e == i) ? i + MAX : e;
        }
        size_t clen = end - i;
        char *wrapped = (char *)malloc(clen + 32);
        snprintf(wrapped, clen + 32, "<pre>%.*s</pre>", (int)clen, esc + i);
        send_one(tg, chat_id, wrapped, 1, first ? cb : NULL, ud);
        free(wrapped);
        i = end;
        first = 0;
    }
    free(esc);
    free(allocated_text);
}

void telegram_set_commands(telegram_ctx_t *tg) {
    char *url = (char *)malloc(strlen(tg->api_base) + 16);
    snprintf(url, strlen(tg->api_base) + 16, "%ssetMyCommands", tg->api_base);

    const char *json =
        "[{\"command\":\"start\",\"description\":\"Show usage instructions\"},"
         "{\"command\":\"restart\",\"description\":\"Restart the shell session\"},"
         "{\"command\":\"ctrl_c\",\"description\":\"Send Ctrl-C (SIGINT) to the shell\"},"
         "{\"command\":\"ctrl_z\",\"description\":\"Send Ctrl-Z (SIGTSTP) to the shell\"}]";
    char *enc = urlencode(json);
    size_t plen = strlen(enc) + 16;
    char *post = (char *)malloc(plen);
    snprintf(post, plen, "commands=%s", enc);

    send_ctx_t *sc = (send_ctx_t *)malloc(sizeof(*sc));
    sc->cb = NULL;
    sc->ud = NULL;

    curl_uv_request(tg->cu, url, post, NULL, telegram_send_cb, sc);

    free(url);
    free(enc);
    free(post);
}
