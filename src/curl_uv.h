#ifndef CURL_UV_H
#define CURL_UV_H

#include <uv.h>
#include <stdio.h>
#include <curl/curl.h>

typedef struct curl_req_s curl_req_t;
typedef struct curl_uv_s curl_uv_t;

typedef void (*curl_req_cb)(curl_req_t *req, CURLcode code, long http_code,
                            char *data, size_t len, void *user_data);

struct curl_req_s {
    CURL *easy;
    curl_uv_t *cu;
    struct curl_slist *headers;
    char *url;
    char *post_data;
    char *resp_buf;
    size_t resp_len;
    size_t resp_cap;
    FILE *out;
    curl_mime *mime;
    curl_req_cb cb;
    void *user_data;
};

typedef struct curl_uv_s {
    uv_loop_t *loop;
    CURLM *multi;
    uv_timer_t timer;
    int still_running;
} curl_uv_t;

void curl_uv_init(curl_uv_t *cu, uv_loop_t *loop);
void curl_uv_close(curl_uv_t *cu);

/* Issue an async HTTP request. post_data may be NULL for GET.
 * headers may be NULL. The request and its buffers are owned by the
 * bridge and freed automatically on completion. */
void curl_uv_request(curl_uv_t *cu, const char *url, const char *post_data,
                     struct curl_slist *headers, curl_req_cb cb, void *user_data);

/* GET a URL and stream the body to a local file (opened "wb"). */
void curl_uv_download(curl_uv_t *cu, const char *url, const char *path,
                      curl_req_cb cb, void *user_data);

/* POST using a multipart/mime body. builder fills in the parts using the
 * curl_mime created for the request; it is called synchronously. */
void curl_uv_request_mime(curl_uv_t *cu, const char *url,
                          void (*builder)(curl_mime *mime, void *user_data),
                          void *builder_ud, curl_req_cb cb, void *user_data);

#endif
