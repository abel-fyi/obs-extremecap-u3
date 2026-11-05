# Research log

How the facts in PROTOCOL.md were established, in order. New experiments go at
the end: what was tried, what happened, what it proves.

## Command channel and stream (2026-09)

- Blind EP0/EP1 probing got no replies. Static analysis of AverMedia's own
  macOS and Windows drivers (never installed or run) recovered the EP1 command
  format; `02 10 01 01 05` returned the FPGA mode register.
- Enabling the FPGA's internal test pattern produced a stream on EP 0x83 (not
  0x82). Synchronous reads lost data; 32 queued async 64 KiB transfers gave
  intact frames: C0/C1-framed 1920×1080 YUY2 plus `58` audio records.
- The HDMI receiver only answers after its reset (PCA bit 6) is released.
  Reading it while held in reset, and one 260-byte EDID write, both wedged the
  firmware until a USB replug. The original driver splits I2C into 32-byte
  pieces; so does ours.

## First HDMI video

- The receiver locked to 1080p60, but frames arrived truncated until the VX8897
  scaler was bypassed (reset, direct output mode, PCA bit 3 low), as the
  original driver does. With the bypass: intact frames of the source's desktop.
- PCA bit 2 drives HDMI hotplug inverted (1 = unplugged). Leaving it high made
  sources drop the link after a few frames.
- A newly written driver (no vendor code) then ran for 7+ minutes without a
  rejected frame.

## OBS, recovery, EDID

- Stream restarts are cheap: on signal loss or a mode change, idle the FPGA,
  wait for a stable measured timing (1 s), reset DMA, stream again. Simulated
  hotplugs recovered in about 4 s (mostly the source re-detecting the display);
  pulling the USB cable recovered as soon as it was plugged back in.
- Linux sources reported the card as disconnected: "EDID checksum invalid".
  Reading the DDC bus from the source showed a bit-subset of our EDID. With the
  receiver's EDID disabled the source still read a complete, valid EDID: the
  board has its own EEPROM on DDC, and both were answering (wired-AND). macOS
  had tolerated the corrupt result. Fix: leave the receiver's EDID off. Linux
  and Mac sources then worked, including a machine's pre-boot LUKS prompt.

## Other resolutions and audio

- Stepping a source through its modes with xrandr: the receiver measured every
  mode correctly, and with unchanged FPGA settings every progressive mode
  arrived as complete native-size frames (640×480 to 1920×1080, 24–60 Hz). The
  C1 trailer carries the line count. The scaler chip is not needed.
- A known tone and music played over HDMI identified the audio records as
  big-endian int16 stereo at the source rate, one record per video frame. With
  no source audio the card sends zero-filled records at a mode-dependent size;
  those are dropped by measuring the rate.

## C core and OBS plugin

- The validated Python prototype was ported to C with the same register order.
  One difference wedged the card: saving the PCA9539 with a block read. Its
  pointer wraps within a register pair (`31 4e 31 4e…` instead of
  `31 4e 31 cf 00 00 00 89`), so garbage was written to the pin directions on
  exit. Single-register access fixed it.
- The OBS source delivers YUY2 video and 16-bit stereo audio directly; verified
  with OBS screenshots, audio meters, HDMI loss and mode changes.

## Status LED

- The original driver has `LEDMode00`–`18` functions writing FPGA registers
  0x80–0x86. Replaying them on the card with the owner watching: colour 1 = red,
  2 = blue, 3 = both (pink); styles steady, blink, fade; multi-step patterns
  with per-step durations. Reading back after a two-step pattern only returns
  the last step, so the plugin writes a complete idle pattern (blue breathing)
  on exit instead of "restoring" a half-readable one.

## Open questions

- Interlaced modes (1080i/480i/576i): fields not yet examined.
- Colour matrix: frames are treated as BT.601 limited range; HD sources may be
  BT.709. Needs a colour-bar test.
- Linux as the capture host: builds, untested.
- EP 0x82 and PCA bit 5: purpose unknown.
