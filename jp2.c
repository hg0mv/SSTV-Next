/* jp2.c - JPEG 2000 decoding through the system OpenJPEG library
 *
 * libopenjp2 is loaded at run time (dlopen), so it is not a build
 * dependency; without it JPEG 2000 pictures are only saved, not shown.
 * Only the small, stable part of the OpenJPEG 2.x API is used.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jp2.h"

typedef struct {
	uint32_t dx, dy, w, h, x0, y0, prec, bpp, sgnd, resno_decoded, factor;
	int32_t *data;
	uint16_t alpha;
} opj_comp;

typedef struct {
	uint32_t x0, y0, x1, y1, numcomps;
	int color_space;
	opj_comp *comps;
	uint8_t *icc;
	uint32_t icclen;
} opj_img;

static void *lib;
static int tried;
static void *(*p_stream_file)(const char *, size_t, int);
static void  (*p_stream_destroy)(void *);
static void *(*p_create_decompress)(int);
static void  (*p_default_params)(void *);
static int   (*p_setup)(void *, void *);
static int   (*p_read_header)(void *, void *, opj_img **);
static int   (*p_decode)(void *, void *, opj_img *);
static int   (*p_end)(void *, void *);
static void  (*p_destroy_codec)(void *);
static void  (*p_image_destroy)(opj_img *);

static int load(void)
{
	if (tried) return lib != NULL;
	tried = 1;
#ifdef _WIN32
	const char *names[] = {"openjp2.dll", "libopenjp2-7.dll", "libopenjp2.dll", NULL};   /* next to the exe */
#else
	const char *names[] = {"libopenjp2.so.7", "libopenjp2.so", NULL};
#endif
	for (int i = 0; names[i] && !lib; i++) lib = dlopen(names[i], RTLD_NOW);
	if (!lib) return 0;
#define SYM(v, n) if (!(*(void **)&v = dlsym(lib, n))) { dlclose(lib); lib = NULL; return 0; }
	SYM(p_stream_file, "opj_stream_create_default_file_stream");
	SYM(p_stream_destroy, "opj_stream_destroy");
	SYM(p_create_decompress, "opj_create_decompress");
	SYM(p_default_params, "opj_set_default_decoder_parameters");
	SYM(p_setup, "opj_setup_decoder");
	SYM(p_read_header, "opj_read_header");
	SYM(p_decode, "opj_decode");
	SYM(p_end, "opj_end_decompress");
	SYM(p_destroy_codec, "opj_destroy_codec");
	SYM(p_image_destroy, "opj_image_destroy");
	return 1;
}

int jp2_available(void) { return load(); }

image *jp2_load(const char *path)
{
	if (!load()) return NULL;
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	unsigned char sig[12] = {0};
	size_t n = fread(sig, 1, 12, f);
	fclose(f);
	int fmt;
	if (n >= 4 && sig[0] == 0xff && sig[1] == 0x4f && sig[2] == 0xff && sig[3] == 0x51) fmt = 0;   /* J2K codestream */
	else if (n >= 12 && !memcmp(sig + 4, "jP  ", 4)) fmt = 2;                                    /* JP2 file */
	else return NULL;
	void *st = p_stream_file(path, 1 << 20, 1);
	if (!st) return NULL;
	void *codec = p_create_decompress(fmt);
	void *params = calloc(1, 1 << 16);       /* opj_dparameters_t, generously sized */
	opj_img *im = NULL;
	image *out = NULL;
	p_default_params(params);
	if (codec && p_setup(codec, params) && p_read_header(st, codec, &im) && p_decode(codec, st, im) && p_end(codec, st)) {
		int nc = (int)im->numcomps;
		if (nc >= 1 && im->comps[0].w > 0 && im->comps[0].h > 0 && im->comps[0].w < 10000 && im->comps[0].h < 10000) {
			int w = (int)im->comps[0].w, h = (int)im->comps[0].h;
			out = image_new(w, h);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					for (int ch = 0; ch < 3; ch++) {
						opj_comp *c = &im->comps[nc >= 3 ? ch : 0];
						int cx = (int)(x * im->comps[0].dx / (c->dx ? c->dx : 1));
						int cy = (int)(y * im->comps[0].dy / (c->dy ? c->dy : 1));
						if (cx >= (int)c->w) cx = (int)c->w - 1;
						if (cy >= (int)c->h) cy = (int)c->h - 1;
						int v = c->data[(size_t)cy * c->w + cx];
						if (c->sgnd) v += 1 << (c->prec - 1);
						if (c->prec > 8) v >>= c->prec - 8;
						else if (c->prec < 8) v <<= 8 - c->prec;
						out->px[((size_t)y * w + x) * 3 + ch] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
					}
			/* sYCC (colour space 3) -> RGB */
			if (im->color_space == 3 && nc >= 3)
				for (int i = 0; i < w * h; i++) {
					unsigned char *p = out->px + i * 3;
					double Y = p[0], cb = p[1] - 128.0, cr = p[2] - 128.0;
					double r = Y + 1.402 * cr, g = Y - 0.344136 * cb - 0.714136 * cr, b = Y + 1.772 * cb;
					p[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
					p[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
					p[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
				}
		}
	}
	if (im) p_image_destroy(im);
	if (codec) p_destroy_codec(codec);
	p_stream_destroy(st);
	free(params);
	return out;
}
