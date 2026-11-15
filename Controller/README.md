# Features
- BMD ATEM
- OBS WebSocket
- vMix TCP Tally
- supports 64 tallies (even with ATEM)
- since largest ATEM has 40 sources, camIds 41-64 can be used for sum tallies
- ESP-NOW long range mode (WIFI_PROTOCOL_LR)

# Hardware
- WT32-ETH01 (recommended)
- ESP32-POE-ISO
- Seeed Studio XIAO W5500 Ethernet Adapter (XIAO ESP32-S3 Plus, PoE)

# Build & Flash

```bash
cd Controller
pio run -e wt32-eth01     # WT32-ETH01 / ESP32-POE-ISO build & flash as before
pio run -e xiao-w5500      # build the XIAO W5500 firmware
pio run -e xiao-w5500 -t upload     # flash over USB (USB-Serial/JTAG, auto-reset)
```

## XIAO W5500 notes
- The board is an ESP32-S3 Plus with a WIZnet W5500 on SPI, so it needs the
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform
  (Arduino core 3.x): the stock PlatformIO `espressif32` platform ships core 2.x,
  whose `ETH` class has no W5500/SPI support. The env already pins the platform URL.
- Flashing over USB-C works out of the box via the S3's USB-Serial/JTAG unit
  (shows up as `/dev/cu.usbmodem*`, VID:PID `303A:1001`). If esptool cannot
  connect, hold BOOT, tap RESET, then release BOOT to enter download mode.
- Serial monitor at 115200 baud (same as the other boards).
- If you alternate builds between `xiao-w5500` and the WT32/POE envs, PlatformIO
  re-downloads the respective Arduino core on the switch (the two platforms share
  one framework package slot by design). Expect a one-time multi-minute delay
  after switching; keep building one env at a time to avoid it.

# Pure ESP-IDF port (experimental)

`[env:xiao-w5500-idf]` is a from-scratch ESP-IDF implementation of the same XIAO
W5500 controller, built on the stock `espressif32` platform so it shares the
Receiver's IDF 6.0.1 toolchain. Sources live in `src/idf/` and are excluded from
the Arduino environments via `build_src_filter`.

```bash
pio run -e xiao-w5500-idf              # build
pio run -e xiao-w5500-idf -t upload    # flash over USB-Serial/JTAG
```

Status by slice:

- [x] W5500 bring-up (SPI2, polling, DHCP), HTTP server, embedded UI pages
- [x] WebSocket tally feed on port 81 (2 s keepalive, same frame format)
- [x] OSC UDP on port 57110
- [x] ESP-NOW tally pump (WiFi STA brought up alongside ETH, channel 1, LR)
- [x] mDNS advertisement (`tally.local`, `TallyBridge._http._tcp`)
- [x] NVS config persistence (`/config` stores the URI and reboots into it,
  `/power` is stored in `tally:power`; same namespace/keys as Preferences in
  the Arduino build, so a board keeps its settings across firmwares)
- [x] ATEM client (handshake, init recovery, `TlIn` tally, `/cut` and `/auto`)
- [x] vMix client (`SUBSCRIBE TALLY`, 1 s -> 30 s reconnect backoff) and the
  TCP tally server on port 8099 for vMix-protocol clients
- [x] OBS client (obs-websocket v5: scene names tagged `T<n>`, `/cut` and
  `/auto` switch to the scene tagged with the selected tally)
- [x] Task watchdog (5 s, panic on timeout) on every long-running task
- [x] Over-the-air ESP-NOW verified against a Receiver (tally burst, SLEEP,
  SET_CAMID / SIGNAL / SET_COLOR)

Notes:

- IDF 6.0 moved the SPI Ethernet drivers out of `esp_eth`; the W5500 driver is
  pulled from the component registry through `src/idf_component.yml`
  (`espressif/w5500`), so the first build downloads it.
- IDF 6.0 also moved mDNS out of the core components; `espressif/mdns` comes
  from the same manifest. The controller advertises `tally.local` and
  `TallyBridge._http._tcp` on port 80, matching the Arduino build.
- The console runs on the S3's USB Serial/JTAG (see `sdkconfig.defaults`), not
  UART0, because the XIAO has no USB-UART bridge.
- The HTML pages under `src/idf/web/` are symlinks into `data/`, so both builds
  serve the same files. `src/CMakeLists.txt` embeds them at CMake configure
  time, which PlatformIO does not re-run after a page edit, so
  `scripts/embed_pages.py` refreshes the generated assembly before every build;
  without it an edited page would stay stale until the build directory was
  cleaned.
- `sdkconfig.defaults` only seeds a *new* generated `sdkconfig.<env>`; ESP-IDF
  keeps whatever value is already set in an existing one. After changing the
  defaults (e.g. the socket pool), delete `Controller/sdkconfig.xiao-w5500-idf`
  (git-ignored) and rebuild.
- The app partition is `partitions_singleapp_large.csv` (1.5 MB factory). The
  stock 1 MB layout no longer fits; PlatformIO always regenerates
  `partitions.bin` from `board_build.partitions`, ignoring
  `CONFIG_PARTITION_TABLE_*`.
- The W5500 has no factory MAC, so the IDF build seeds it from the ESP32-S3
  eFuse (`esp_read_mac(..., ESP_MAC_ETH)`); without that DHCP went out with
  `00:00:00:00:00:00`.
- The input paths are event-driven rather than polled: the W5500 raises its
  INTn interrupt, the ATEM and vMix clients block in `select()` and are woken
  by the socket itself (the ATEM client also has a loopback wake pair for
  `/cut`), the vMix server blocks in `select()` and is poked over a loopback
  socket when the tally changes, and the ESP-NOW burst pump sleeps on a task
  notification until its next deadline. Only the watchdog feed uses a timeout
  (1 s in the ATEM and vMix clients), so a `/cut` goes out immediately and the
  idle tasks stay asleep. The Arduino build drives all of this from a `loop()`
  on a 20 ms `delay()`, which adds up to 20 ms of latency to every input.
- The ESP-NOW pump sends the full `burst_ms` burst on a tally change (five
  packets with the default 100 ms burst / 20 ms gap), then a single packet per
  keepalive window (2 s floor) so an idle controller does not re-broadcast an
  unchanged tally. A SLEEP command follows the keepalive when `sleep_ms > 0`,
  so the receiver duty cycle keeps running.
- The app is built with `CONFIG_COMPILER_OPTIMIZATION_SIZE` (the IDF default is
  `-Og`; `-Os` is ~9% smaller here) and silent assertions. The station WiFi
  buffers are trimmed too, since that interface only carries ESP-NOW.
- One task owns every output. A tally change only latches the new state and
  wakes the pump task, which then pushes the WebSocket frame and the vMix TCP
  update and runs the ESP-NOW burst. Protocol tasks therefore never do sink
  work, and rapid changes coalesce into one push instead of one per
  intermediate state (before this only the radio burst coalesced, which is why
  the WebSocket path used to show ~75 ms outliers).
- Every long-running task subscribes to the ESP-IDF task watchdog
  (`watchdog.c`, 5 s, panic on timeout) and bounds its waits so it can feed: a
  hang anywhere now reboots the controller instead of silently dropping a
  source. That is what `enableLoopWDT()` does for `loop()` in the Arduino
  build. The `esp_http_server` tasks are the exception -- they belong to the
  component and are not subscribed.
- That interrupt line is only wired on the W5500 Ethernet Adapter from **V1.2**
  on (Seeed's "INT pin wiring fix, D16 -> W5500 INTn", 2025/12/01; D16 is
  GPIO10 on the XIAO ESP32-S3 Plus). On an "Early Build" adapter INTn floats,
  so interrupt mode would stall RX completely -- build those with
  `-D XIAO_W5500_POLLING=1` to fall back to the 10 ms poll. The boot log states
  which mode is active.
- The IDF OSC parser follows OSC 1.0 (the type tag starts after the address's
  NUL terminator, at `align4(addr_len + 1)`, and int32 args are big-endian)
  and accepts `0` digits in addresses, so tally ids 10-64 work. The Arduino
  implementation scans one byte late, uses `align4(addr_len)` (which skips the
  type tag entirely for 8-char addresses like `/program` and `/preview`),
  skips the byte swap and treats `0` as a separator; this port intentionally
  does not copy those bugs.
- The ATEM client reuses the wire-format layer in `lib/ATEMmini` but runs on
  BSD nonblocking UDP and `esp_timer`, and `/cut`/`/auto` only leave a pending
  switch request for the client task to send, so all UDP I/O stays in one
  task. `src/idf/atem.cpp` is C++ (ATEMmini is namespaced), which makes
  PlatformIO feed the merged C/C++ flag set to the whole `src/` component;
  `build_unflags = -fuse-cxa-atexit` in `[env:xiao-w5500-idf]` removes the
  C++-only option that would otherwise break every C file there.
- The vMix client and the port-8099 tally server each run in their own task,
  where the Arduino build polls both from `loop()`. Nothing blocks between
  them: connect() is bounded by a 3 s timeout and every read/write is
  nonblocking, so a dead or slow host cannot stall the ESP-NOW burst pump. The
  server reports an input on program *and* preview as `3` (the Arduino server
  only emits `1` there) and always sends 64 state characters, including before
  the first update (the Arduino server sends `TALLY OK 0`).
- The OBS client keeps the same scene-name tagging as the Arduino build
  (`T3 Cam` == input 3, `T1,T2` == inputs 1 and 2, a downstream-keyer overlay
  ORs its tags onto program) but updates only the half of the tally it owns, so
  a scene event cannot clobber an OSC/ATEM/vMix update to the other bus; only
  a transition (program |= preview) touches both. It reassembles fragmented
  messages before parsing and builds its requests with cJSON, so scene names
  containing quotes or backslashes are escaped correctly. Like the Arduino
  build there is no authentication: the OBS websocket server must accept the
  connection without a password.
- Small deliberate divergences from the Arduino build: the switcher port comes
  from the config URI instead of the hardcoded 9910, a config URI with an
  invalid address or a port outside 1..65535 is ignored rather than stored,
  `PUT /power` persists the canonical (clamped) string so a partial body cannot
  silently reset the other tunables on the next boot, and `PUT /config`
  replies before restarting (50 ms timer) instead of blocking in the handler.

# Protocol Selection
Access the web UI at the device's IP address. Select the switcher protocol:
- ATEM: Blackmagic Design ATEM switcher (default)
- OBS: OBS WebSocket (port 4455)
- vMix: vMix via TCP (default port 8099)

# vMix TCP Tally
The controller can both receive tally from vMix and provide tally data to receivers via TCP:

## Receiving from vMix
Connect to vMix computer on port 8099. Sends `SUBSCRIBE TALLY` command. Receives tally updates in format:
```
TALLY OK 000...032...
```
Where each character is: 0=idle, 1=program, 2=preview, 3=program+preview

## Serving to Receivers
The controller runs a TCP server on port 8099 that receivers can connect to. Clients can subscribe with `SUBSCRIBE TALLY` to receive real-time tally updates.

# HTTP API

## GET /tally
Serves the Web Tally page (HTML)

## GET /tally.json
Returns current tally state as JSON:
```json
{"program":4,"preview":8}
```
Bitmasks: bit 0 = tally 1, bit 1 = tally 2, etc.

## POST /set
Configure tallies. Query parameters (can combine multiple):

### Color
`/set?color=RRGGBB&i=N` - Set tally N color (hex)
- `/set?color=FF0000&i=1` - Set tally 1 to red

### Brightness
`/set?brightness=0-255&i=N` - Set tally N brightness
- `/set?brightness=128&i=1` - Set tally 1 to 50% brightness

### Camera ID
`/set?camid=1-64&i=N` - Map tally N to respond to switcher source
- `/set?camid=7&i=8` - Tally 8 responds to camera 7

### Signal (manual tally)
`/set?signal=12-23&i=N` - Send signal to tally N
- 12 = off, 13 = left, 14 = down, 15 = up, 16 = right
- 17 = freeze, 18 = brightness+, 19 = brightness-
- 20 = zoom+, 21 = intercom+, 22 = intercom-, 23 = check

## GET /seen
Returns list of connected tallies and their last-seen time:
```json
{"tallies":[{"id":1,"seen":5},{"id":2,"seen":120}]}
```

## GET /config, PUT /config
Reads or writes the switcher URI (the first line is what counts, see
`Protocol Selection` below). `PUT` stores it and restarts the controller so the
new target is picked up; the response is sent before the restart.

## GET /power, PUT /power
ESP-NOW power-save tunables as `ws=<sleep_ms>&wb=<burst_ms>&wg=<gap_ms>`.
The defaults are `ws=0&wb=100&wg=20`, i.e. five packets per tally change.
`PUT` clamps (gap <= burst), applies and persists them.

## POST /cut, POST /auto
Switch the first tally named in `?i=` (same CSV syntax as `/set`): ATEM puts it
straight on program (`/auto` runs an AUTO transition), OBS switches to the
scene tagged `T<n>` (`/auto` sets the preview scene and triggers a transition,
which needs OBS Studio Mode). vMix has no switch action, same as the Arduino
build.

# Configuration
Configure via web UI:
- protocol: ATEM, OBS, or vMix
- ip: Switcher IP address
- port: Connection port (for OBS/vMix)
- group: ESP-NOW group (1-9, broadcasts to all if empty)
- bg: Background color in hex (e.g., 001E1E for dark grey)

# OSC Protocol
UDP server on port 57110. Supports:

Type tags may be omitted when every argument is int32: `/program 2` and
`/program ,i 2` are equivalent.

## /tally[program,preview]
Set both program and preview tallies. First arg = program, second arg = preview. Values are tally numbers.
- `/tally ii(2)(4)` - Tally 2 program, tally 4 preview
- `/tally ii(3)(0)` - Tally 3 program only
- `/tally ii(0)(5)` - Tally 5 preview only

## /tally,N
Same as `/tally ii(N)(0)` - Set tally N to program

## /program,N
Set tally N to program only. Can accept multiple integers.
- `/program 2` - Tally 2 program
- `/program ii(2)(4)(6)` - Tallies 2, 4, 6 program

## /preview,N
Set tally N to preview only. Can accept multiple integers.
- `/preview 4` - Tally 4 preview
- `/preview ii(1)(3)(5)` - Tallies 1, 3, 5 preview

## /color,N
Set tally N color to red
- `/color,4` - Set tally 4 to red

## /brightness,N
Set tally N brightness to full
- `/brightness,7` - Set tally 7 to full brightness

## /id,N
Set tally N camera ID
- `/id,8` - Set tally 8 camera ID

# WebSocket API
Server on port 81. Broadcasts real-time updates:

## TALLY
`TALLY 000...032...` - Each char: 0=idle, 1=program, 2=preview, 3=both

## Settings Broadcast
When settings change via HTTP API, broadcasts are sent:
- `CAMID 7 8` - Camera ID 7 mapped to tally 8
- `BRIGHTNESS 128 8` - Brightness set to 128 for tally 8
- `COLOR FF0000 8` - Color set to red for tally 8
- `SIGNAL 12 8` - Signal sent to tally 8

# Web Interfaces

## Director Page (`/director`)
Web interface for directors to control tallies:
- Signal buttons: ← ↓ ↑ → (nav), × (off), F (freeze), B (brightness), Z (zoom), I+/I- (intercom), ✓ (check)
- Color buttons: red, green, blue, yellow, white
- Tally status display (red=program, green=preview)
- Select tallies by clicking, then press button to send command

## Web Tally (`/tally`)
Self-contained tally display page:
- Shows program (red) and preview (green) status
- Updates via WebSocket
- Useful as a browser-based tally

# Configuration

## URI Scheme
Configure via URI string (save with PUT /config):
- `atem://192.168.88.240` - ATEM switcher
- `obs://192.168.88.50:4455` - OBS WebSocket
- `vmix://192.168.88.10:8099` - vMix TCP
- `atem://192.168.88.240?bg=1E1E1E` - With background color

Supports multiple presets (only first line is active, rest are saved as presets).

## mDNS
Access the controller via `http://tally.local` (hostname: "tally", instance: "TallyBridge")
