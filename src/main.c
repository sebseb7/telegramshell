#define _GNU_SOURCE
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <qrencode.h>
#include <time.h>

app_t g_app;

static uv_signal_t sigint_watcher;
static uv_signal_t sigterm_watcher;

/* Load KEY=VALUE pairs from ./.env into the process environment.
 * Lines starting with '#' and blank lines are ignored. Values may be
 * optionally quoted with single or double quotes. */
static void load_dotenv(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '\0' || *p == '#' || *p == '=')
            continue;

        char *key = p;
        while (*p && *p != '=' && !isspace((unsigned char)*p))
            p++;
        size_t klen = p - key;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p != '=')
            continue;
        p++;
        while (*p && isspace((unsigned char)*p))
            p++;

        char *val = p;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            isspace((unsigned char)val[vlen - 1])))
            val[--vlen] = '\0';

        if (vlen >= 2 && (val[0] == '"' || val[0] == '\'') &&
            val[vlen - 1] == val[0]) {
            memmove(val, val + 1, vlen - 2);
            val[vlen - 2] = '\0';
        }

        char *buf = (char *)malloc(klen + 1);
        memcpy(buf, key, klen);
        buf[klen] = '\0';
        setenv(buf, val, 0);
        free(buf);
    }
    fclose(f);
}

static void on_signal(uv_signal_t *handle, int signum) {
    (void)handle;
    fprintf(stderr, "\nReceived signal %d, shutting down.\n", signum);
    g_app.stopping = 1;
    uv_stop(g_app.loop);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--generate-totp") == 0) {
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f) {
            perror("fopen /dev/urandom");
            return 1;
        }
        unsigned char rand_buf[10];
        if (fread(rand_buf, 1, sizeof(rand_buf), f) != sizeof(rand_buf)) {
            perror("fread");
            fclose(f);
            return 1;
        }
        fclose(f);

        const char *b32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        char secret[17];
        int buffer = 0;
        int bits_left = 0;
        int count = 0;

        for (int i = 0; i < 10; i++) {
            buffer = (buffer << 8) | rand_buf[i];
            bits_left += 8;
            while (bits_left >= 5) {
                secret[count++] = b32[(buffer >> (bits_left - 5)) & 0x1F];
                bits_left -= 5;
            }
        }
        if (bits_left > 0) {
            secret[count++] = b32[(buffer << (5 - bits_left)) & 0x1F];
        }
        secret[count] = '\0';

        char uri[256];
        snprintf(uri, sizeof(uri), "otpauth://totp/TelegramShell?secret=%s&issuer=TelegramShell", secret);

        QRcode *qr = QRcode_encodeString(uri, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
        if (!qr) {
            fprintf(stderr, "Failed to encode QR code.\n");
            return 1;
        }

        printf("\nScan this QR Code in Google Authenticator or Authy:\n\n");
        int margin = 4;
        for (int y = -margin; y < qr->width + margin; y++) {
            printf("  "); /* Left terminal padding */
            for (int x = -margin; x < qr->width + margin; x++) {
                int is_black = 0;
                if (y >= 0 && y < qr->width && x >= 0 && x < qr->width) {
                    is_black = qr->data[y * qr->width + x] & 1;
                }
                if (is_black) {
                    printf("\033[40m  \033[0m"); /* Black block */
                } else {
                    printf("\033[47m  \033[0m"); /* White block */
                }
            }
            printf("\n");
        }
        QRcode_free(qr);

        printf("\nOr manually enter the secret: %s\n", secret);
        printf("\nAdd this line to your .env file:\n");
        printf("TOTP_SECRET=%s\n", secret);

        return 0;
    }

    load_dotenv(".env");

    const char *token = getenv("TELEGRAM_BOT_TOKEN");
    if (!token) {
        fprintf(stderr, "TELEGRAM_BOT_TOKEN not set\n");
        return 1;
    }
    const char *allowed = getenv("ALLOWED_CHAT_ID");
    const char *totp_secret = getenv("TOTP_SECRET");

    g_app.token = token;
    g_app.allowed_chat_id = allowed;
    g_app.totp_secret = totp_secret;
    g_app.last_chat_id = NULL;
    g_app.stopping = 0;
    g_app.exit_code = 0;
    g_app.pending_pid_send = 0;
    g_app.last_sent_pid = 0;
    g_app.loop = uv_default_loop();

    curl_global_init(CURL_GLOBAL_ALL);
    curl_uv_init(&g_app.curl_uv, g_app.loop);
    telegram_init(&g_app.tg, &g_app.curl_uv, g_app.token, &g_app);
    bot_init(&g_app.bot, &g_app);
    shell_init(&g_app.shell, &g_app);

    shell_start(&g_app.shell);
    telegram_set_commands(&g_app.tg);
    telegram_start_polling(&g_app.tg);

    uv_signal_init(g_app.loop, &sigint_watcher);
    uv_signal_start(&sigint_watcher, on_signal, SIGINT);
    uv_signal_init(g_app.loop, &sigterm_watcher);
    uv_signal_start(&sigterm_watcher, on_signal, SIGTERM);

    fprintf(stderr, "Telegram shell bot started. Polling for updates...\n");
    uv_run(g_app.loop, UV_RUN_DEFAULT);

    shell_stop(&g_app.shell);
    bot_stop(&g_app.bot);
    curl_uv_close(&g_app.curl_uv);
    curl_global_cleanup();
    free(g_app.last_chat_id);
    return g_app.exit_code;
}
