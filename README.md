# SSTV Next

A standalone SSTV transceiver for Linux and Windows: all 25 common analog modes plus
digital SSTV (HamDRM, compatible with EasyPal and QSSTV), with rig control through Hamlib,
a template editor and an ADIF log.

The radio control (rigctld), sound card handling and ADIF log come from
[DigiPan Next](https://github.com/hg0mv), the BPSK31 program by the same author.

**Status:** development version (M0.10). Tested with recordings and simulated channels;
on-air reports are welcome.

## Features

- **Analog SSTV, 25 modes**: Robot, Scottie, Martin, PD, Wraase, Pasokon, FAX480.
  Automatic mode detection from the VIS header, or from the sync pulses when the VIS was
  missed. Robust against QRM on the header, slant correction, QRM speck filter.
- **Weak-signal reception**: the VIS header and the sync pulses are also detected with
  narrow-band (matched) filters, not only with the FM discriminator, so reception starts
  and holds down to about 0 dB SNR (2.5 kHz bandwidth), well below where the picture
  becomes readable - if you can hear it, it starts.
- **Digital SSTV (HamDRM)**: own implementation from the DRM standard, bit-identical
  signal to QSSTV. Robustness modes A/B/E, 4/16/64-QAM, Reed-Solomon protected files
  (`.rs1`-`.rs4`), repeat requests (BSR), JPEG and JPEG 2000 pictures.
- **Automatic reception**: analog and digital decoding run side by side, nothing to switch.
- **FSK ID**: the other station's callsign (sent after the picture, MMSSTV / QSSTV style,
  or the digital station label) is filled into the log; your own callsign is sent after
  your pictures.
- **Template editor** (similar to MMSSTV): text, shapes and pictures with macros
  (`%M` my call, `%C` his call, `%R` RST, `%T` time, `%D` date, `%F` frequency).
- **Rig control** via Hamlib rigctld (CAT frequency, PTT), ADIF log shared with DigiPan Next.

## Building on Linux

Needs `libx11-dev`, `libasound2-dev` (Debian/Ubuntu package names), gcc and make.
The stb libraries for PNG/JPEG and TrueType fonts are in `third_party/` (public domain).
Fonts come from `/usr/share/fonts` (Ubuntu, DejaVu or Liberation); another UI font can be
set with `SSTV_UI_FONT=/path/to/font.ttf`.

    make

This builds five programs:

| program     | purpose |
|-------------|---------|
| `sstv_next` | the X11 transceiver |
| `sstv_dec`  | command line decoder: pictures from a WAV recording |
| `sstv_enc`  | command line encoder: picture to WAV (for tests, with simulated errors) |
| `drm_dec`   | digital (HamDRM) decoder: received files from a WAV recording |
| `drm_enc`   | digital (HamDRM) encoder: file to WAV |

JPEG 2000 pictures (QSSTV sends these by default) are shown with the system `libopenjp2`
library, loaded at run time (Ubuntu package `libopenjp2-7`, normally installed). Without it
the received file is still saved, just not displayed.

Rig control needs Hamlib (`sudo apt install libhamlib-utils`).

## Windows

Windows releases are a zip file: unpack and run `sstv_next.exe` (Windows 10/11, 64-bit).
Hamlib, OpenJPEG and fallback fonts are included. See `win/README-Windows.txt`.

The Windows version is cross-compiled on Linux with MinGW-w64 / llvm-mingw:

    make win WINCC=x86_64-w64-mingw32-gcc WINRES=x86_64-w64-mingw32-windres

The result goes to `win/dist/SSTVNext`. Windows-specific code is in `win/` (`winx.c`: the
small part of Xlib the program uses, on top of Win32; `glob.c`; replacement headers), the
sound (`audio.c`: WinMM) and rig control (`rig.c`: Winsock, starting the bundled
`rigctld.exe`) are in `#ifdef _WIN32` branches. The third-party files of the package (Hamlib
Windows release, OpenJPEG `openjp2.dll` with `vcruntime140.dll`) are copied from
`win/bundle`, which is not part of the repository: download them from the official
[Hamlib](https://github.com/Hamlib/Hamlib/releases) and
[OpenJPEG](https://github.com/uclouvain/openjpeg/releases) releases.

## Usage

    ./sstv_next

- **Audio input**: choose the sound card connected to the radio (`plughw:N,0` or `default`).
  The program works at 11025 Hz; plughw converts on any card.
- **Radio**: rig model, serial port and speed. On the first start the callsign and rig
  settings are taken from DigiPan Next. The program runs its own rigctld on port 14533,
  so it can run together with DigiPan Next.
- **Reception** is automatic. The mode comes from the VIS code; if the VIS was missed
  (weak signal, QSB, switched on in the middle of a picture), from the timing of the sync
  pulses after about 10 lines, with the mistuning measured from the sync pulses as well.
  The picture is drawn line by line, the slant is corrected on the fly, and the result is
  saved as PNG (`~/sstv/rx`). If the signal is lost for 8 seconds, or a new VIS arrives
  (another station starts), the partial picture is saved marked `_part`.
  - **RX mode**: "Auto (all modes)" by default; a fixed mode can be chosen (click: next,
    right click / wheel: previous).
  - **Save now**: closes and saves the picture being received.
  - **QRM filter**: replaces the coloured specks caused by voice QRM (SSB) from the
    neighbouring lines; thin lines and text are left alone. On by default.
  - **QRM on the header**: if voice partly covers the VIS, the program still finds the start
    of the picture from the header frame and decides the mode from the sync pulses of the
    first ~6 seconds ("VIS damaged by QRM ..."). A correct-looking VIS is also checked
    against the sync pulses, and if two bits flipped, the right mode is used.
  - **Tuning**: tune the 1900 Hz leader of the VIS header onto the yellow line.
  - Click a picture in the gallery to view it large; **Live** goes back.
- **Transmission**: choose a background from the pictures in `~/sstv/tx` (`<` `>` or the
  arrow keys), PNG/JPEG/BMP/PPM. The picture is cropped to the mode's size (from the centre).
  **Mode**: click for the next mode, right click / wheel for the previous one. **Templ**
  selects the template put on the picture. **Send / F1** starts, **Esc** stops. PTT handling
  and the safety cut-offs are the same as in DigiPan Next.
- **Template editor** (Templ → **Edit**), similar to MMSSTV templates:
  - Objects: **text** (TrueType fonts, outline, shadow), **rectangle**, **ellipse**,
    **line**, **picture**. Clicking a received picture in the gallery inserts it (for a reply
    picture); the *Picture* button inserts the latest one.
  - Mouse: click = select, drag = move, drag the blue corner = resize (font size for text),
    the two end points for a line. Keys: Del delete, arrows move by 1 pixel, Esc leaves.
  - Toolbar: text, font (`<` `>`), outline/shadow/fill, fill and line colour (Fill/Line),
    size, line width, opacity, layer order (Up/Down), copy, delete.
  - Macros in the text, filled in at transmission: `%M` my callsign, `%C` the other station
    (from the Call field of the log bar), `%R` RST (S field), `%T` UTC time, `%D` date,
    `%F` frequency.
  - Templates: name + **Save** (saving under a new name makes a copy), **New**,
    **Delete tpl**, `<` `>` to switch. **Done** saves and returns. Three templates are
    created on the first start: *Call*, *CQ* and *QSO reply*. They are stored in
    `~/.config/sstvnext/templates/*.tpl` (text files).
  - Reception keeps running in the background while editing; its state is shown below the picture.
- **FSK ID** (callsign after the picture, MMSSTV / QSSTV standard):
  - On reception the program listens for the FSK ID after a picture (2100/1900 Hz,
    45.45 baud) and shows it ("FSK ID: HA5XYZ"). Digital stations send their callsign in
    the signal; it is recognised as well. If the **Call** field of the log is empty (and
    you are not typing in it), the callsign is filled in and the QSO time starts. A callsign
    already typed is never overwritten.
  - On transmission (analog modes) it is switched with the **FSK ID on/off** button below
    the Mode row (on by default): your callsign is sent in about 2 seconds after the picture.
  - Works down to about 8 dB SNR and ±100 Hz mistuning.
- **Log**: Call, RST (595 by default), MHz (read from the radio) → **Log QSO**. By default it
  writes the same `~/digipan_log.adi` file as DigiPan Next, MODE=SSTV, with the exact mode in
  the COMMENT field.

## Digital SSTV (HamDRM)

The digital mode used by EasyPal, QSSTV and WinDRM, in an own implementation (from the DRM
standard, with a signal bit-identical to QSSTV's).

- **Reception** is automatic and runs in parallel with the analog decoder. When a digital
  signal is found, a `DIGITAL` badge, the mode (A/B/E, QAM, protection, bandwidth) and the
  sender's callsign are shown; below the picture the received segments, the SNR and the
  frequency offset. The picture builds up during reception (JPEG). Mistuning up to
  ±150 Hz, sound card clock errors of several hundred ppm and HF fading (multipath) are
  handled. The received file is saved as it was sent in `~/sstv/rx/digital`, and the
  picture as PNG in the gallery. If the signal goes and some segments are missing, the part
  received is saved marked `_part`.
- **Transmission**: after the 25 analog modes, the **Mode** button offers **Digital DRM**,
  with two extra rows:
  - `DRM`: robustness mode (**A** for good signals, **B** general, **E** weak / fading
    signals), modulation (**4/16/64-QAM**), protection (**High**/**Norm**), bandwidth
    (**2.3k**/**2.5k**). Click: next value, right click / wheel: previous.
  - `Pic`: picture size (320x240 ... 800x600) and the JPEG size limit (6 ... 64 kB); the
    best JPEG quality that fits is chosen. The label next to Mode shows the expected
    transmission time and the actual file size.
  - **RS** (end of the `Pic` row): Reed-Solomon protection (**no RS**, **RS1** ... **RS4**).
    The file gets longer (RS1 +14 %, RS2 +33 %, RS3 +60 %, RS4 +100 %), but the receiver
    can rebuild the whole picture even if some segments were lost (with RS4 up to half).
    `.rs1`-`.rs4` files from QSSTV and EasyPal are received as well.
  - Templates are put on the picture just as in analog mode. Esc stops the transmission.
  - Tip: the most common setting is **B / 16-QAM / Norm / 2.5k**; with poor propagation
    **E / 4-QAM / High**.
  - Keep the ALC low (OFDM signal): set the level so that the ALC just does not move.
- **Repeat requests (BSR)**, as in EasyPal / QSSTV:
  - If segments of a received digital picture are missing, an **Ask repeat** button appears
    below the picture. It sends the list of missing segments (`bsr.bin`) with the current
    DRM settings. When the sender answers, the missing parts are added to the picture.
  - When someone asks for missing parts of *your* picture, the message line shows it
    ("BSR from ...") and a **Fix N** button replaces **Rescan** (N = segments asked for).
    It resends only those segments, with the original settings. The last 6 pictures sent
    are remembered (until the program is restarted).

### Testing without a radio

    ./sstv_next -f recording.wav -s 10    # feed a WAV recording to the receiver, 10x speed
    ./sstv_next -x tx.wav                 # transmission goes to a WAV file instead of the sound card
    ./sstv_dec tx.wav                     # decode for checking (-r: without the QRM filter)
    ./drm_enc -m B -q 16 -c HG0MV pic.jpg drm.wav    # digital transmission to WAV
    ./drm_dec -v -o /tmp drm.wav                      # digital reception from WAV
    ./drm_enc -r 2 pic.jpg drm.wav                    # with Reed-Solomon protection (pic.rs2)
    ./drm_dec -b bsr.txt drm.wav                      # repeat request for the missing segments
    ./drm_enc -F bsr.txt pic.jpg fix.wav              # answer to a repeat request

## Supported analog modes

| family | modes |
|--------|-------|
| Robot | Robot 36, Robot 72 |
| Scottie | S1, S2, S3, S4, DX |
| Martin | M1, M2, M3, M4 |
| PD | PD50, PD90, PD120, PD160, PD180, PD240, PD290 |
| Wraase | SC2-60, SC2-120, SC2-180 |
| Pasokon | P3, P5, P7 |
| other | FAX480 (black and white) |

Without a VIS, S3/S4 and M3/M4 cannot be told apart from S1/S2 and M1/M2 (same timing);
the full-height variant is shown then. The MMSSTV-specific modes (MP, MR, ML, extended
16-bit VIS) are not supported yet.

## Source files

| file | contents |
|------|----------|
| `modes.c` | mode table (a new mode is one line) |
| `demod.c` | FM discriminator (instantaneous frequency) |
| `tones.c` | sliding DFT tone powers (weak-signal VIS detection) |
| `rx.c` | VIS search, sync tracking, robust slant fit, picture building |
| `tx.c` | encoder (phase-continuous FM, VIS header) |
| `tpl.c` | templates: objects, TrueType text (stb_truetype), drawing, saving |
| `live.c` | live receiver engine (own thread), analog + digital |
| `drm_common.c` | HamDRM tables, cell map, convolutional code, Viterbi, CRC |
| `drm_tx.c` / `drm_rx.c` | digital transmitter (OFDM) / receiver (sync, channel estimation, decoding) |
| `mot.c` | picture transfer in packets (MOT objects, segments, BSR) |
| `rsfile.c` | Reed-Solomon protected files (.rs1-.rs4): encoder, error-correcting decoder |
| `fskid.c` | FSK ID (callsign after the picture): decoder and encoder |
| `jp2.c` | JPEG 2000 loading with the system OpenJPEG library |
| `audio.c`, `rig.c`, `adif.c` | sound card, rigctld, ADIF log (from DigiPan Next) |
| `config.c` | settings (`~/.config/sstvnext/config`) |
| `ui.c` | drawing layer: framebuffer, anti-aliased shapes, TrueType text |
| `sstv_next.c` | the user interface |
| `win/` | Windows port |

## License

Copyright (C) 2026 HG0MV

This program is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either version 3
of the License, or (at your option) any later version. See [LICENSE](LICENSE).

Third-party code: the stb libraries in `third_party/` (public domain / MIT). The Windows
package also contains Hamlib (LGPL/GPL), OpenJPEG (BSD 2-clause) and the DejaVu fonts, each
with its own license file.

73 de HG0MV
