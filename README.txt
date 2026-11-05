AVERMEDIA EXTREMECAP U3 FOR OBS

An open-source OBS plugin for the AVerMedia ExtremeCap U3 (CV710).
It talks directly to the capture card over USB. It does not use
AVerMedia software, drivers or firmware.

VIDEO AND AUDIO

  Video: progressive HDMI, up to 1920x1080 at 24-60 Hz
  Audio: HDMI stereo at 32, 44.1 or 48 kHz

Interlaced video is not supported yet. The plugin has been tested
on macOS with OBS 32.2 and on Linux and macOS source computers.

INSTALL

macOS needs Xcode command line tools and Homebrew:

  brew install libusb
  make install

Linux needs OBS development headers and libusb development files:

  make install
  sudo cp 70-cv710.rules /etc/udev/rules.d/

The build targets OBS 32.2. To use another OBS version:

  make OBS_VERSION=x.y.z

USE

Restart OBS. Add a source named:

  AVerMedia ExtremeCap U3 (CV710)

To keep the picture's shape when the input changes size, choose
Transform, then Fit to screen. OBS Virtual Camera can send the
picture to apps such as Zoom.

The card's LED shows its status:

  Steady blue       Picture is live
  Steady red        Picture is live while recording or streaming
  Blinking blue     Waiting for a picture
  Blinking red      Recording or streaming without a picture
  Blinking pink     Picture format is unsupported, such as interlaced
  Blue breathing    Card is idle

TROUBLESHOOTING

Look for lines starting with [cv710] in the OBS log under
Help, Log Files.

  Card not connected or busy:
    Check the USB connection. Only one program can use the card.

  Black picture:
    Check that the source is sending video. Some computers show
    firmware and boot menus only on their built-in display.

  No picture from a Linux source:
    Check that xrandr sees the card's display, then mirror or
    extend your desktop to it.

BUILDING FROM SOURCE

macOS needs libusb (brew install libusb). Linux needs OBS
development headers and libusb development files.

  make

To run the hardware smoke test, close OBS first. Only one process
can use the card:

  make smoke
  build/smoke 10

For hardware details, see PROTOCOL.md. The investigation notes
are in research/LOG.md.
