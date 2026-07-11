#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include "curl_uv.h"

#include <stdlib.h>
#include <string.h>

typedef struct curl_poll_s {
    uv_poll_t poll;
    curl_socket_t fd;
    curl_uv_t *cu;
} curl_poll_t;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    curl_req_t *req = (curl_req_t *)userdata;
    size_t total = size * nmemb;
    if (req->out) {
        size_t w = fwrite(ptr, 1, total, req->out);
        return w;
    }
    if (req->resp_len + total + 1 > req->resp_cap) {
        while (req->resp_len + total + 1 > req->resp_cap)
            req->resp_cap = req->resp_cap ? req->resp_cap * 2 : 4096;
        req->resp_buf = (char *)realloc(req->resp_buf, req->resp_cap);
    }
    memcpy(req->resp_buf + req->resp_len, ptr, total);
    req->resp_len += total;
    req->resp_buf[req->resp_len] = '\0';
    return total;
}

static void check_multi_done(curl_uv_t *cu) {
    CURLMsg *msg;
    int msgs_left;
    while ((msg = curl_multi_info_read(cu->multi, &msgs_left))) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        CURL *easy = msg->easy_handle;
        curl_req_t *req = NULL;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, (char **)&req);
        if (!req)
            continue;

        long http_code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
        CURLcode code = msg->data.result;
        curl_req_cb cb = req->cb;
        void *ud = req->user_data;
        char *data = req->resp_buf;
        size_t len = req->resp_len;

        curl_multi_remove_handle(cu->multi, easy);
        curl_easy_cleanup(easy);

        if (req->mime)
            curl_mime_free(req->mime);
        if (req->out) {
            fclose(req->out);
            req->out = NULL;
        }
        if (req->headers)
            curl_slist_free_all(req->headers);
        free(req->url);
        free(req->post_data);
        free(req);

        if (cb)
            cb(NULL, code, http_code, data, len, ud);
        free(data);
    }
}

static void on_poll_close(uv_handle_t *handle) {
    curl_poll_t *cp = (curl_poll_t *)handle;
    free(cp);
}

static void curl_poll_cb(uv_poll_t *p, int status, int events) {
    (void)status;
    curl_poll_t *cp = (curl_poll_t *)p;
    int action = 0;
    if (events & UV_READABLE)
        action |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE)
        action |= CURL_CSELECT_OUT;
    int running = 0;

    curl_uv_t *cu = cp->cu;
    curl_socket_t fd = cp->fd;

    curl_multi_socket_action(cu->multi, fd, action, &running);
    check_multi_done(cu);
}

static int curl_socket_cb(CURL *easy, curl_socket_t s, int action,
                          void *userp, void *socketp) {
    (void)easy;
    curl_uv_t *cu = (curl_uv_t *)userp;
    curl_poll_t *cp = (curl_poll_t *)socketp;

    if (action == CURL_POLL_REMOVE) {
        if (cp) {
            uv_poll_stop(&cp->poll);
            uv_close((uv_handle_t *)&cp->poll, on_poll_close);
        }
        return 0;
    }

    if (!cp) {
        cp = (curl_poll_t *)malloc(sizeof(*cp));
        cp->fd = s;
        cp->cu = cu;
        uv_poll_init(cu->loop, &cp->poll, (int)s);
        curl_multi_assign(cu->multi, s, cp);
    }

    int uv_events = 0;
    if (action & CURL_POLL_IN)
        uv_events |= UV_READABLE;
    if (action & CURL_POLL_OUT)
        uv_events |= UV_WRITABLE;

    if (uv_events)
        uv_poll_start(&cp->poll, uv_events, curl_poll_cb);
    else
        uv_poll_stop(&cp->poll);

    return 0;
}

static void curl_timer_fire(uv_timer_t *t) {
    curl_uv_t *cu = (curl_uv_t *)t->data;
    int running = 0;
    curl_multi_socket_action(cu->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    check_multi_done(cu);
}

static int curl_timer_cb(CURLM *multi, long timeout_ms, void *userp) {
    (void)multi;
    curl_uv_t *cu = (curl_uv_t *)userp;
    if (timeout_ms < 0) {
        uv_timer_stop(&cu->timer);
        return 0;
    }
    uv_timer_start(&cu->timer, curl_timer_fire, (unsigned int)timeout_ms, 0);
    return 0;
}

void curl_uv_init(curl_uv_t *cu, uv_loop_t *loop) {
    cu->loop = loop;
    cu->multi = curl_multi_init();
    curl_multi_setopt(cu->multi, CURLMOPT_SOCKETFUNCTION, curl_socket_cb);
    curl_multi_setopt(cu->multi, CURLMOPT_SOCKETDATA, cu);
    curl_multi_setopt(cu->multi, CURLMOPT_TIMERFUNCTION, curl_timer_cb);
    curl_multi_setopt(cu->multi, CURLMOPT_TIMERDATA, cu);
    uv_timer_init(loop, &cu->timer);
    cu->timer.data = cu;
    cu->still_running = 0;
}

void curl_uv_close(curl_uv_t *cu) {
    uv_timer_stop(&cu->timer);
    if (cu->multi) {
        curl_multi_cleanup(cu->multi);
        cu->multi = NULL;
    }
}

void curl_uv_request(curl_uv_t *cu, const char *url, const char *post_data,
                     struct curl_slist *headers, curl_req_cb cb, void *user_data) {
    curl_req_t *req = (curl_req_t *)calloc(1, sizeof(*req));
    req->cu = cu;
    req->url = strdup(url);
    req->cb = cb;
    req->user_data = user_data;
    req->headers = headers;

    CURL *easy = curl_easy_init();
    req->easy = easy;
    curl_easy_setopt(easy, CURLOPT_URL, req->url);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, req);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, req);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);

    if (post_data) {
        req->post_data = strdup(post_data);
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, req->post_data);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(req->post_data));
    }
    if (headers)
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

    curl_multi_add_handle(cu->multi, easy);
}

void curl_uv_download(curl_uv_t *cu, const char *url, const char *path,
                      curl_req_cb cb, void *user_data) {
    curl_req_t *req = (curl_req_t *)calloc(1, sizeof(*req));
    req->cu = cu;
    req->url = strdup(url);
    req->cb = cb;
    req->user_data = user_data;

    req->out = fopen(path, "wb");
    if (!req->out) {
        fprintf(stderr, "curl_uv_download: cannot open %s: %s\n", path, strerror(errno));
        free(req->url);
        free(req);
        if (cb)
            cb(NULL, CURLE_WRITE_ERROR, 0, NULL, 0, user_data);
        return;
    }

    CURL *easy = curl_easy_init();
    req->easy = easy;
    curl_easy_setopt(easy, CURLOPT_URL, req->url);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, req);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, req);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);

    curl_multi_add_handle(cu->multi, easy);
}

void curl_uv_request_mime(curl_uv_t *cu, const char *url,
                          void (*builder)(curl_mime *mime, void *user_data),
                          void *builder_ud, curl_req_cb cb, void *user_data) {
    curl_req_t *req = (curl_req_t *)calloc(1, sizeof(*req));
    req->cu = cu;
    req->url = strdup(url);
    req->cb = cb;
    req->user_data = user_data;

    CURL *easy = curl_easy_init();
    req->easy = easy;
    curl_easy_setopt(easy, CURLOPT_URL, req->url);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, req);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, req);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);

    curl_mime *mime = curl_mime_init(easy);
    builder(mime, builder_ud);
    req->mime = mime;
    curl_easy_setopt(easy, CURLOPT_MIMEPOST, mime);

    curl_multi_add_handle(cu->multi, easy);
}
