CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE
LDLIBS  = -lm

CORE = modes.o wav.o image.o demod.o tones.o rx.o tx.o
DRM  = drm_common.o drm_tx.o drm_rx.o mot.o rsfile.o
APP  = $(CORE) $(DRM) fskid.o jp2.o imgio.o combine.o live.o audio.o rig.o adif.o config.o tpl.o ui.o

all: sstv_next sstv_enc sstv_dec sstv_combine drm_enc drm_dec

# the X11 transceiver
sstv_next: sstv_next.o $(APP)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lX11 -lasound -lpthread -ldl $(LDLIBS)

# command line tools (no X11/ALSA needed)
sstv_enc: sstv_enc.o $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

sstv_dec: sstv_dec.o $(CORE) imgio.o combine.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

sstv_combine: sstv_combine.o image.o imgio.o combine.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

drm_enc: drm_enc.o $(DRM) wav.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

drm_dec: drm_dec.o $(DRM) wav.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c *.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o sstv_next sstv_enc sstv_dec sstv_combine drm_enc drm_dec

.PHONY: all clean

# ---- Windows build (cross compile with MinGW-w64 / llvm-mingw) ----------------
#   make win                 -> win/dist/SSTVNext/sstv_next.exe (+ command line tools)
WINCC   ?= x86_64-w64-mingw32-gcc
WINRES  ?= x86_64-w64-mingw32-windres
WINFLAGS = -O2 -std=c11 -D_DEFAULT_SOURCE -include win/compat.h -Iwin/include -Iwin -I.
WINOBJ   = win/obj
WINAPP   = $(addprefix $(WINOBJ)/,$(APP:.o=.w.o)) $(WINOBJ)/winx.w.o $(WINOBJ)/glob.w.o
WINCORE  = $(addprefix $(WINOBJ)/,$(CORE:.o=.w.o))
WINDRM   = $(addprefix $(WINOBJ)/,$(DRM:.o=.w.o)) $(WINOBJ)/wav.w.o
WINDIST  = win/dist/SSTVNext
WINLIBS  = -static -lwinmm -lgdi32 -luser32 -lws2_32 -ladvapi32 -lshell32 -lpthread -lm

win: $(WINDIST)/sstv_next.exe $(WINDIST)/sstv_dec.exe $(WINDIST)/sstv_enc.exe $(WINDIST)/sstv_combine.exe $(WINDIST)/drm_dec.exe $(WINDIST)/drm_enc.exe
	@mkdir -p $(WINDIST)/fonts
	cp win/fonts/*.ttf win/fonts/LICENSE-DejaVu.txt $(WINDIST)/fonts/
	if [ -d win/bundle ]; then cp -r win/bundle/* $(WINDIST)/; fi
	cp README.md LICENSE win/README-Windows.txt $(WINDIST)/

$(WINOBJ)/%.w.o: %.c *.h win/*.h
	@mkdir -p $(WINOBJ)
	$(WINCC) $(WINFLAGS) -c $< -o $@

$(WINOBJ)/%.w.o: win/%.c win/*.h
	@mkdir -p $(WINOBJ)
	$(WINCC) $(WINFLAGS) -c $< -o $@

$(WINOBJ)/sstvnext.res.o: win/sstvnext.rc win/sstvnext.ico win/sstvnext.manifest
	@mkdir -p $(WINOBJ)
	cd win && $(WINRES) sstvnext.rc -O coff -o obj/sstvnext.res.o

$(WINDIST)/sstv_next.exe: $(WINOBJ)/sstv_next.w.o $(WINAPP) $(WINOBJ)/sstvnext.res.o
	@mkdir -p $(WINDIST)
	$(WINCC) -mwindows -o $@ $^ $(WINLIBS)

$(WINDIST)/sstv_dec.exe: $(WINOBJ)/sstv_dec.w.o $(WINCORE) $(WINOBJ)/imgio.w.o $(WINOBJ)/combine.w.o
	@mkdir -p $(WINDIST)
	$(WINCC) -o $@ $^ $(WINLIBS)

$(WINDIST)/sstv_enc.exe: $(WINOBJ)/sstv_enc.w.o $(WINCORE)
	@mkdir -p $(WINDIST)
	$(WINCC) -o $@ $^ $(WINLIBS)

$(WINDIST)/sstv_combine.exe: $(WINOBJ)/sstv_combine.w.o $(WINOBJ)/image.w.o $(WINOBJ)/imgio.w.o $(WINOBJ)/combine.w.o
	@mkdir -p $(WINDIST)
	$(WINCC) -o $@ $^ $(WINLIBS)

$(WINDIST)/drm_dec.exe: $(WINOBJ)/drm_dec.w.o $(WINDRM)
	@mkdir -p $(WINDIST)
	$(WINCC) -o $@ $^ $(WINLIBS)

$(WINDIST)/drm_enc.exe: $(WINOBJ)/drm_enc.w.o $(WINDRM)
	@mkdir -p $(WINDIST)
	$(WINCC) -o $@ $^ $(WINLIBS)

winclean:
	rm -rf win/obj win/dist

.PHONY: win winclean
