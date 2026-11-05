# How the CV710 works

Verified on 07ca:0710 (bcdDevice 0205). `src/cv710.c` is the executable version
of this page; the discovery history is in `research/LOG.md`.

## USB

One vendor interface. **EP 0x01/0x81**: command/reply. **EP 0x83**: the stream
(video and audio multiplexed; read with ~32 queued 64 KiB async transfers,
synchronous reads lose data). EP 0x82 never delivered data.

EP1 commands (one at a time; read the reply before the next command):

| Bytes | Meaning |
|---|---|
| `01 addr 01 reg data…` | I2C write (main bus) |
| `02 addr 01 n reg` → n bytes | I2C read (main bus) |
| `03…` / `04…` | same, on the second ("GPIO") I2C bus |
| `14` | reset stream DMA |

`addr` is 7-bit. **Never send more than 32 data bytes per command**: a larger one
hung the firmware until the card was replugged.

## Chips (7-bit I2C addresses)

| Addr | Chip | Notes |
|---|---|---|
| 0x10 | FPGA ("LFE") | reg5 bit1 = streaming; reg6 1 = YUY2; reg8 1 = HDMI input |
| 0x74 | PCA9539 GPIO expander | reg2/3 outputs, reg6/7 directions. **Single-register access only**: block reads wrap inside a register pair |
| 0x20 (+maps) | ADV7604-class HDMI receiver | maps: CP 0x22, AFE 0x26, REP 0x32, HDMI 0x34 |
| 0x18 main bus | VX8897 scaler | bypassed |
| 0x18 GPIO bus | TI audio codec | audio clocks, configured at bring-up as the original driver does |

PCA port 0 bits: 0/1 source routing, **2 HDMI hotplug (inverted: 1 = unplugged)**,
3 video path (0 = direct), 4 scaler reset, 5 pulsed at bring-up (purpose unknown),
**6 receiver reset (active low)**. Never access the receiver while bit 6 is low: it stalls the firmware.

**EDID:** the board has its own EDID EEPROM on the HDMI DDC lines ("AVERMEDIA HD":
1080p60 preferred, 720p, 1080i, SD, VGA modes, stereo LPCM). The receiver's
internal EDID must stay **off** (REP 0x77 bits 3:0 = 0); if both answer, sources
read the bitwise AND of the two and Linux rejects it.

## Bring-up

Board reset pulses → audio clocks → receiver maps and HDMI port A → YCbCr 4:2:2
output → EDID off → assert hotplug → scaler bypass (reset, VX 0x8a|=2, 0x0d|=0x80,
PCA bit 3 = 0). Wait until the measured timing (HDMI map 0x07–0x0b size/interlace,
0x1e/0x26 totals, 0x06/0x3b TMDS clock) is stable for 1 s. Then: FPGA idle, reg6=1,
reg1=0x0c, reg2=0, reg8=1, DMA reset, queue transfers, FPGA work bit. Exact values
are in `receiver_init()`.

## Stream

```
00 ff ff c0  seq meta flags sum        sum = c0 + seq + meta + flags
             width × height × 2 bytes YUY2 (Y0 U Y1 V), limited range
00 ff ff c1  seq m1 lines_hi lines_lo sum
00 ff ff 58  u32 big-endian length     between frames
             big-endian int16 stereo PCM, one record per video frame
```

Every progressive mode arrives at its native size (tested 640×480 to 1920×1080,
24–60 Hz) with the same FPGA settings. `00 ff` cannot occur in limited-range video,
so markers are unambiguous outside audio records. Audio runs at the source's
32/44.1/48 kHz; with no source audio the card sends zero-filled records at a
mode-dependent size, which the core ignores. Interlaced modes are not yet handled.
