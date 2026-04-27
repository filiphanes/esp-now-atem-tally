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

# Configuration
Configure via web UI:
- protocol: ATEM, OBS, or vMix
- ip: Switcher IP address
- port: Connection port (for OBS/vMix)
- group: ESP-NOW group (1-9, broadcasts to all if empty)
- bg: Background color in hex (e.g., 001E1E for dark grey)

# OSC Protocol
UDP server on port 8000. Supports:

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
