CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
SRCS     = src/main.c src/curl_uv.c src/telegram.c src/shell.c src/bot.c src/totp.c src/render.c vendor/yyjson/yyjson.c
INC      = -Isrc -Ivendor/yyjson -I/usr/include/x86_64-linux-gnu
LIBS     = -luv -lcurl -lutil -lcrypto -lqrencode -ljpeg
BIN      = telegramshell

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) $(INC) $(SRCS) -o $(BIN) $(LIBS)

clean:
	rm -f $(BIN)

.PHONY: clean
