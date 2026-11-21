SSTV Next - Windows version
===========================

Analog (25 modes) and digital (HamDRM, EasyPal / QSSTV compatible) SSTV transceiver.

Installation
------------
No installation needed. Unpack the zip into any folder (e.g. C:\SSTVNext) and start
sstv_next.exe. Windows 10 or 11, 64-bit.

On the first start Windows SmartScreen may warn ("unknown publisher") because the program
is not digitally signed: "More info" -> "Run anyway".

Setup
-----
1. Audio input: choose the sound card of the radio (for the FT-891 usually the input called
   "USB Audio CODEC"). For transmission the program uses the output of the same sound card
   (matched by the part of the name in brackets).
2. Radio button: rig model (e.g. Yaesu FT-891), COM port, speed. The COM port number is in
   Device Manager -> Ports (COM & LPT) ("Silicon Labs CP210x ..."). The program starts the
   included Hamlib rigctld.exe in the background (hamlib folder).
3. In the Windows sound settings set the input level of the sound card so that the "level"
   in the TUNING panel shows about 20-50 %, and the output level so that the ALC just does
   not move.

Where the files are
-------------------
  received pictures:     C:\Users\<name>\sstv\rx      (digital files: ...\rx\digital)
  pictures to send:      C:\Users\<name>\sstv\tx
  settings:              C:\Users\<name>\.config\sstvnext   (templates: ...\templates)
  log:                   C:\Users\<name>\digipan_log.adi    (shared with DigiPan Next)

Using the program is the same as on Linux (see README.md).

Contents
--------
  sstv_next.exe            the program
  sstv_dec.exe, sstv_enc.exe, sstv_combine.exe,
  drm_dec.exe, drm_enc.exe                              command line tools
  fonts\                   DejaVu fonts (fallback if Segoe UI is missing) - free license
  hamlib\                  Hamlib 4.6.5 (rigctld, rigctl) - LGPL/GPL,
                           source: https://github.com/Hamlib/Hamlib
  openjp2.dll, vcruntime140.dll   OpenJPEG 2.5.3 (BSD license) for JPEG 2000 pictures

Known limitation: the Windows version was tested under Wine (reception from WAV,
transmission to WAV, rig control with the Hamlib dummy rig, digital and analog decoding);
it has not yet run on real Windows hardware with a real sound card.

SSTV Next is free software under the GNU General Public License v3 (see LICENSE).

73 de HG0MV
