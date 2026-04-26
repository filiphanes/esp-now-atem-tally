# Features
- BMD ATEM
- OBS WebSocket
- vMix TCP Tally
- supports 64 tallies (even with ATEM)
- since largest ATEM has 40 sources, camIds 41-64 can be used for sum tallies

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
`/set?signal=1-25&i=N` - Send signal to tally N
- Signal 12 = off, 13 = left, 14 = down, 15 = up, 16 = right
- Signal 17 = freeze, 18 = brightness+, 19 = brightness-, 20 = zoom+, 21 = zoom-

### Program/Preview override
`/set?program=1&preview=1&i=N` - Manually set program/preview state

## GET /seen
Returns list of connected tallies and their last-seen time:
```json
{"tallies":[{"id":1,"seen":5},{"id":2,"seen":120}]}
```

# Configuration
Configure via web UI:
- Protocol: ATEM, OBS, or vMix
- IP: Switcher IP address
- Port: Connection port (for OBS/vMix)
- Group: ESP-NOW group (1-9, broadcasts to all if empty)