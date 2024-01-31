# Features
- BMD ATEM
- supports 64 tallies (even with ATEM)
- since largest ATEM has 40 sources, camIds 41-64 can be used for sum tallies

# TODO
- multiple alternative configurations
- sum tally can react to multiple tallies (ONAIR)
- ATEM auto discover
- list of connected tallies via web UI

# OSC protocol

## Examples:

`/tally,ii<2><4>`: Set tally 2 to program and tally 3 to preview

`/1/tally,TF`: Set tally 1 to program and not preview

`/4/rgb,r<0x0000FF>`: Set tally 4 to blue

`/{4,5,6}/rgb,r<0x0000FFFF>`: Set tallies 4, 5 and 6 to blue
`/{4,5,6}/rgb,i<0x0000FF>`: Set tallies 4, 5 and 6 to blue

`/5/signal,ii<12><5000>`: Set tally 5 to signal 12 for 5 seconds

`/7/brightness,i<255>`: Set tally 7 to full brightness

`/8/id,i<7>`: Set tally 8 to id=7 so it will react on tally 7