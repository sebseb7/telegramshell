CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
SRC      = src/main.c src/curl_uv.c src/telegram.c src/shell.c src/bot.c src/totp.c vendor/yyjson/yyjson.c
INC      = -Isrc -Ivendor/yyjson -I/usr/include/x86_64-linux-gnu
LIBS     = -luv -lcurl -lutil -lcrypto -lqrencode
BIN      = telegramshell

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o $(BIN) $(LIBS)

clean:
	rm -f $(BIN)

.PHONY: clean
