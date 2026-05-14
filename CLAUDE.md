# CLAUDE.md — ESP32 NTRIP DUO

## Project overview

Dual NTRIP source station firmware for ESP32. Reads RTCM correction data from a GNSS receiver (Unicore UM980) via UART and pushes it simultaneously to two NTRIP casters (RTK2go and Onocoy). Includes a web UI for configuration, live logging, OTA firmware updates, and remote diagnostics.

Based on [nebkat/esp32-xbee](https://github.com/nebkat/esp32-xbee), modified to add dual NTRIP output, OTA, improved stability, and remote diagnostics.

**Device:** ESP32 (4MB flash), deployed remotely — no serial access in normal operation.
**GNSS receiver:** Unicore UM980, connected via UART, outputs RTCM3 corrections.

---

## Build system

- **ESP-IDF v5.4.x** (pure CMake, not PlatformIO)
- IDF installed at: `C:\Users\WIN-7\esp\v5.4\esp-idf`
- Build via VSCode with ESP-IDF extension or CMD with IDF environment active
- **Do not attempt to build from Git Bash** — ESP-IDF does not support MSYS/MinGW

```cmd
idf.py build
idf.py flash   # USB only — used for initial flash or partition table changes
```

OTA updates (normal workflow after initial flash):

- Build → `build\esp32-xbee.bin`
- Upload at `http://<device-ip>/ota`

---

## Flash layout (`partitions.csv`)

| Name      | Type | Size  | Notes                        |
|-----------|------|-------|------------------------------|
| nvs       | data | 24K   | Config storage (NVS)         |
| phy_init  | data | 4K    |                              |
| factory   | app  | 1M    | Primary firmware             |
| ota_0     | app  | 1M    | OTA target partition         |
| www       | data | 1M    | SPIFFS web files             |
| coredump  | data | 192K  | Panic core dump storage      |
| otadata   | data | 8K    | OTA boot partition selector  |

**Important:** Partition table changes require USB flash. After that, all updates go through `/ota`.

---

## Source layout

```text
main/
  main.c                  — app_main, heap watchdog (60s monitor, restart <20KB free)
  wifi.c                  — WiFi STA/AP, reconnect logic
  web_server.c            — HTTP server, file serving, /status, /ota, /core_dump
  uart.c                  — UART read/write, event dispatch
  util.c                  — connect_socket(), destroy_socket(), TCP keepalive
  retry.c                 — Exponential backoff: retry_init / retry_delay / retry_reset
  config.c                — NVS config read/write helpers
  core_dump.c             — Core dump check and HTTP download endpoint
  log.c                   — Circular log buffer, log_vprintf() [IMPORTANT: allocates 512B on stack]
  stream_stats.c          — Byte counters for NTRIP streams
  interface/
    ntrip_server.c        — NTRIP server 1 (RTK2go), task + sleep task
    ntrip_server_2.c      — NTRIP server 2 (Onocoy), identical structure
    ntrip_util.c          — Shared NTRIP helpers
  protocol/
    nmea.c                — NMEA sentence utilities
  include/                — Headers for all above
www/
  index.html(.gz)         — Main config page
  log.html(.gz)           — Live log page (SSE)
  bs-4.6.2.min.css(.gz)   — Bootstrap 4.6.2
  bs-4.6.2.bundle.min.js(.gz) — Bootstrap + Popper bundle
  jquery-3.4.1.min.js(.gz)    — jQuery
```

**www/ gzip rule:** Both `.gz` and original files must be present. The web server transparently serves `.gz` with `Content-Encoding: gzip` if available. When adding new web files, always add a `.gz` version too.

---

## Key architecture

### Task structure

| Task                     | Stack  | Priority | Notes                                  |
|--------------------------|--------|----------|----------------------------------------|
| `ntrip_server_task`      | 8192   | 5        | Main NTRIP loop, suspends after connect |
| `ntrip_server_sl`        | 6144   | 5        | Monitors UART keepalive, wakes server  |
| `ntrip_server_2_task`    | 8192   | 5        | Identical for second caster            |
| `ntrip_server_2_sl`      | 6144   | 5        | Same as above                          |
| `wifi_sta_reconnect`     | 4096   | 0        | WiFi reconnect on disconnect           |
| `reset_button_task`      | 4096   | 0        | GPIO0 long-press factory reset         |
| `app_main`               | —      | —        | Becomes heap watchdog loop             |

**Stack size warning:** `log_vprintf()` in `log.c` allocates `char buffer[512]` on the stack on every ESP_LOG* call. Tasks that log must have sufficient stack — minimum 4096 bytes for any task using ESP_LOGX macros. Tasks with multiple log call sites or deeper call stacks need 6144+. The sleep tasks were sized up to 6144 after a confirmed stack overflow crash (core dump 2026-05-14).

### NTRIP server flow

```text
UART data → ntrip_server_uart_handler()
              └─ sets DATA_READY_BIT
              └─ writes to socket if CASTER_READY_BIT set
              └─ calls destroy_socket() + vTaskResume(server_task) on write error

ntrip_server_task (loop):
  retry_delay() → wait_for_ip() → connect_socket() → HTTP handshake
  → set CASTER_READY_BIT → vTaskSuspend(NULL)   ← woken by UART handler or sleep task
  → _error: vTaskSuspend(sleep_task) → destroy_socket() → loop

ntrip_server_sleep_task (loop):
  every 1s: data_keep_alive += 1000
  if >= NTRIP_KEEP_ALIVE_THRESHOLD (10000ms):
    if CASTER_READY_BIT set → destroy_socket() + vTaskResume(server_task)
    clear DATA_READY_BIT
```

### Socket handling (`util.c`)

`connect_socket()` sets these options on every socket:

- `SO_RCVTIMEO` / `SO_SNDTIMEO` = 10s (EAGAIN = "Timed out waiting for response (10s)")
- `SO_KEEPALIVE` + `TCP_KEEPIDLE=10` + `TCP_KEEPINTVL=5` + `TCP_KEEPCNT=3`
- `SO_REUSEADDR`

`destroy_socket()` checks `*socket >= 0` before shutdown/close, then sets to -1.

### Retry backoff (`retry.c`)

`retry_init(first_instant, short_count, short_delay, max_delay)`

- NTRIP servers: `retry_init(true, 5, 2000, 0)` — instant first, then 5× 2s, then exponential up to 1h
- WiFi reconnect: `retry_init(true, 5, 2000, 60000)` — caps at 60s

---

## HTTP endpoints

| Method | Path         | Description                                  |
|--------|--------------|----------------------------------------------|
| GET    | /            | Redirects to /index.html                     |
| GET    | /status      | JSON: WiFi, heap, reset_reason, core_dump_available, streams |
| GET    | /log         | SSE stream of live log entries               |
| GET    | /core_dump   | Download raw core dump binary                |
| GET    | /ota         | OTA upload page (HTML)                       |
| POST   | /ota         | OTA firmware upload (binary, reboots after)  |
| GET    | /*           | Static file from SPIFFS /www, .gz preferred  |

---

## Remote diagnostics

Since the device is deployed remotely with no serial access:

1. **Live log:** `http://<ip>/log.html`
2. **Status/heap:** `http://<ip>/status` — includes `reset_reason`, `core_dump_available`, heap stats
3. **Core dump:** `curl http://<ip>/core_dump -o core_dump.bin`
   - Analyze: `idf.py coredump-info -c core_dump.bin`
   - SHA256 must match current build ELF — keep old build artifacts if needed
4. **OTA update:** `http://<ip>/ota` — upload `build\esp32-xbee.bin`

---

## Common pitfalls

- **SHA256 mismatch on coredump:** Core dump captured with old firmware, but ELF is from new build. Either use `--force` flag or rebuild from the commit that was running at crash time.
- **"11 No more processes":** errno 11 = EAGAIN in newlib. Means socket recv timed out (10s). Not a real error, just the caster not responding.
- **www file not served:** Check that both the plain file and `.gz` version exist in `www/`. SPIFFS has a 1MB limit — check total size before adding large files.
- **Stack overflow:** Any task that calls ESP_LOGX needs ≥4096 bytes stack due to `log_vprintf()` using 512B stack buffer. Never set interface task stacks below 4096.
- **Partition table change:** Requires USB flash (`idf.py flash`). Cannot be done via OTA. After changing `partitions.csv`, rebuild and physically flash once.
- **OTA binary:** Use `build\esp32-xbee.bin` only (the app binary). Never upload bootloader, partition table, or merged flash image via OTA.

---

## ERROR_ACTION macro

Used throughout for error handling with automatic file/line logging:

```c
ERROR_ACTION(TAG, condition, action, format, ...)
// Example:
ERROR_ACTION(TAG, sock < 0, goto _error, "Could not connect: %d", errno);
// Expands to: if (condition) { ESP_LOGE(...); action; }
```

---

## Hardware notes

- **Power supply:** Shared USB-C 5V rail between ESP32 and UM980. Known issue: voltage sag during WiFi TX + GNSS reacquisition spikes. Fix: 470µF + 100nF capacitors across 5V rail at UM980 power pins (16V rated).
- **GNSS receiver:** Unicore UM980 — must be in BASE mode (`MODE BASE` or `MODE BASE <lat> <lon> <alt>`) for RTCM output. Uses `SAVECONFIG` to persist settings.
- **GPIO0:** Long press (>5s) triggers factory reset (config wipe + restart).
