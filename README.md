# Telegram Shell Bot

A Telegram bot written in C that spawns an interactive `bash` session over a PTY,
forwards every user message as input to the shell, and streams all shell output
back as Telegram messages.

It is fully async using **libuv** (event loop, PTY polling, timers), **libcurl**
(multi/socket interface for the Telegram HTTP API), and the vendored **yyjson**
JSON parser. A single `uv_run()` loop drives everything — no threads.

> [!CAUTION]
> This bot executes arbitrary shell commands received from Telegram. It is a
> remote shell and should only run on a trusted, firewalled machine. You must
> restrict access to your own Telegram `chat_id` via `ALLOWED_CHAT_ID`, and it
> is highly recommended to add a second layer of security using `TOTP_SECRET`.

## Build

```bash
make
```

(CMake is also supported: `mkdir build && cd build && cmake .. && make`.)

Dependencies (Debian/Ubuntu): `libuv1-dev`, `libcurl4-openssl-dev`, `libssl-dev`, `libqrencode-dev`, `libjpeg-dev`, and the
`libutil` that ships with glibc. `yyjson` is vendored under `vendor/yyjson`.

## Systemd Service

You can install `telegramshell` as a systemd user service using the provided script:

```bash
./install-service.sh
```

This will build the binary, copy it and your `.env` file, and set up a systemd user service.

**Important Notes for Systemd User Services:**
- By default, user services only start when you log in and stop when you log out.
- To keep the bot running 24/7 in the background even after you log out (and to start it automatically on system boot), you must enable **lingering**. This requires root privileges to set up once:
  ```bash
  sudo loginctl enable-linger $USER
  ```
- Use `systemctl --user status telegramshell` to check the status.
- View real-time logs using `journalctl --user -u telegramshell -f`.

## Run

```bash
export TELEGRAM_BOT_TOKEN="123456:ABC-your-token"
export ALLOWED_CHAT_ID="987654321"   # optional, but strongly recommended
export TOTP_SECRET="JBSWY3DPEHPK3PXP" # optional, enables 2FA if set
./telegramshell
```

| Variable | Required | Description |
|----------|----------|-------------|
| `TELEGRAM_BOT_TOKEN` | yes | Bot token from [@BotFather](https://t.me/BotFather). |
| `ALLOWED_CHAT_ID` | no | If set, only this chat is served; all other messages are ignored. If **not** set, the bot replies to every sender with that sender's `chat_id` (so you can copy it into `ALLOWED_CHAT_ID`) and runs no commands. |
| `TOTP_SECRET` | no | A base32 secret for TOTP 2FA. If set, all commands must be prefixed with a 6-digit TOTP code. |

Without a token the bot prints `TELEGRAM_BOT_TOKEN not set` and exits 1.

## 2FA (TOTP)

You can require a 6-digit Time-based One-Time Password (TOTP) to be sent at the beginning of each message. To generate a secret and a QR code you can scan with Google Authenticator or Authy, run:

```bash
./telegramshell --generate-totp
```

Scan the QR code printed in your terminal, and add the generated `TOTP_SECRET` to your `.env` file. Once enabled, you must prefix your commands with the 6-digit code, for example: `123456 ls -la`. The code is stripped before the command is sent to the shell.


## Usage

Send messages to the bot in Telegram:

- `whoami` — returns the username.
- `ls /` — returns the directory listing in a code block.
- `sudo echo hi` — prompts for the password in Telegram; type it and send.
- `/start` — shows usage instructions.
- `/restart` — restarts the shell session.
- `/image_on` — enables Color Shell Mode (renders terminal as a JPEG image).
- `/image_off` — disables Color Shell Mode.
- `/ctrl_c` — sends Ctrl-C (SIGINT) to the foreground process in the shell.
- `/ctrl_z` — sends Ctrl-Z (SIGTSTP) to the foreground process in the shell.
- Send a **file** (document or photo) to the bot — it is downloaded and saved
  into your home directory (`~/`), and the bot replies with the saved name.
- `/download <filename>` — sends the file named `<filename>` from `~/` back to
  you as a Telegram document. The filename is treated as a basename (directory
  components are stripped) for safety.

Shell output is coalesced with a 300 ms debounce timer, HTML-escaped, and
sent with `parse_mode=HTML` wrapped in `<pre>` code blocks. Long output (e.g.
`cat /etc/services`) is split across multiple ≤4080-char messages; rapid output
(e.g. `for i in $(seq 1 100); do echo $i; done`) arrives as one or few messages.

### Color Shell Mode

By default, the bot strips all ANSI escape codes and streams output as plain text. However, if you enable Color Shell Mode using `/image_on`, the bot utilizes an embedded VT100 terminal emulator to render your shell output (including colors, ANSI formatting, and box-drawing characters) into a JPEG image that is sent directly to Telegram. The virtual screen supports up to 150 columns and automatically scrolls to capture the last 100 rows of output. Toggling this mode automatically restarts your shell to set `TERM=xterm-256color`.

> [!NOTE]
> PTY echo is disabled, so typed commands and passwords are **not** reflected
> back into the Telegram output stream.

> [!NOTE]
> The bot auto-reconnects: a failed `getUpdates` (network error, parse error,
> or a non-401 Telegram error) is retried with exponential backoff (1s → 2s →
> 4s … capped at 30s). Successful long-poll cycles re-issue immediately. A 401
> (bad token) is fatal and stops the bot. The shell process is also restarted
> automatically if it exits.

## Project layout

```
telegramshell/
├── CMakeLists.txt
├── Makefile
├── src/
│   ├── main.c          # entry point, config, libuv loop, signal handling
│   ├── curl_uv.c/.h    # libcurl multi ↔ libuv integration
│   ├── telegram.c/.h   # getUpdates / sendMessage / setMyCommands
│   ├── shell.c/.h      # PTY allocation, fork+exec bash, I/O
│   ├── bot.c/.h        # message dispatch, output buffering, /restart
│   ├── render.c/.h     # VT100 emulator and JPEG rendering
│   └── totp.c/.h       # 2FA TOTP verification
├── vendor/yyjson/      # vendored JSON parser
├── install-service.sh  # script to install as systemd user service
└── README.md
```
