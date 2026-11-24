/* mot.c - MOT object transfer over DRM packet mode
 *
 * Picture -> MOT header + body segments -> MSC data groups (with segment
 * field, transport id and CRC) -> one packet per DRM frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mot.h"
#include "drm.h"
#include "rsfile.h"

typedef struct { uint8_t *b; int bit; } bitw;

static void wbits(bitw *w, unsigned v, int n)
{
	for (int i = n - 1; i >= 0; i--) {
		int byte = w->bit >> 3, sh = 7 - (w->bit & 7);
		if ((v >> i) & 1) w->b[byte] |= (uint8_t)(1 << sh);
		else w->b[byte] &= (uint8_t)~(1 << sh);
		w->bit++;
	}
}

unsigned mot_transport_id(const char *name)
{
	int n = (int)strlen(name);
	if (n > 80) n = 80;
	uint8_t x = 0, a = 0;
	for (int k = 0; k < n; k++) {
		x ^= (uint8_t)name[k];
		a += (uint8_t)name[k];
		a ^= (uint8_t)k;
	}
	unsigned id = 256u * a + x;
	if (id <= 2) id += (unsigned)n;
	return id;
}

static void list_add(mot_tx *m, int v)
{
	m->list = realloc(m->list, sizeof(int) * (m->nlist + 1));
	m->list[m->nlist++] = v;
}

static void add_headers(mot_tx *m)
{
	for (int k = 0; k < m->nhseg; k++) list_add(m, -1 - k);
	list_add(m, m->nseg - 1);
}

static void run(mot_tx *m, int len)
{
	for (int cnt = 0; cnt < len;) {
		add_headers(m); cnt++;
		for (int i = 0; i < m->nseg && cnt < len; i++) list_add(m, i), cnt++;
	}
}

int mot_tx_init(mot_tx *m, const uint8_t *data, int len, const char *name, int nbits,
                const int *fix, int nfix)
{
	memset(m, 0, sizeof *m);
	int nl = (int)strlen(name);
	if (nl > 80) nl = 80;
	m->body = data;
	m->bodylen = len;
	m->nbits = nbits;
	m->nbytes = nbits / 8;
	m->paclen = m->nbytes - 3;
	static unsigned bsr_id;
	if (!strcmp(name, "bsr.bin")) m->tid = bsr_id++ % 3;      /* requests use ids 0..2 */
	else m->tid = mot_transport_id(name);

	/* header: core (body size, header size, content type 2 / subtype 1) +
	   TriggerTime "now" + VersionNumber 0 + ContentName */
	m->hdrlen = 7 + 5 + 2 + 3 + nl;
	m->hdr = calloc(m->hdrlen, 1);
	bitw w = {m->hdr, 0};
	wbits(&w, (unsigned)len, 28);
	wbits(&w, (unsigned)m->hdrlen, 13);
	wbits(&w, 2, 6);
	wbits(&w, 1, 9);
	wbits(&w, 2, 2); wbits(&w, 5, 6); wbits(&w, 0, 32);
	wbits(&w, 1, 2); wbits(&w, 6, 6); wbits(&w, 0, 8);
	wbits(&w, 3, 2); wbits(&w, 12, 6); wbits(&w, 0, 1); wbits(&w, (unsigned)(1 + nl), 7);
	wbits(&w, 0, 8);
	for (int i = 0; i < nl; i++) wbits(&w, (uint8_t)name[i], 8);

	m->segsize = m->nbytes - 14;
	m->nseg = (len + m->segsize - 1) / m->segsize;
	m->nhseg = (m->hdrlen + MOT_HDR_SEG - 1) / MOT_HDR_SEG;

	run(m, MOT_RUN_IN);
	if (!fix || nfix == 0) {
		for (int i = 0; i < m->nseg; i++) {
			if (i % 50 == 0) add_headers(m);
			list_add(m, i);
		}
	} else {
		int k = 0;
		for (int i = 0; i < nfix; i++) {
			if (fix[i] < 0 || fix[i] >= m->nseg) continue;   /* a request from the air: only real segments */
			if (k++ % 30 == 0) add_headers(m);
			list_add(m, fix[i]);
		}
	}
	run(m, MOT_RUN_OUT);
	return m->nlist;
}

void mot_tx_free(mot_tx *m)
{
	free(m->hdr);
	free(m->list);
	memset(m, 0, sizeof *m);
}

/* MSC data group for list entry e */
static void data_group(mot_tx *m, int e)
{
	int hdr = e < 0, seg = hdr ? -1 - e : e;
	const uint8_t *src; int slen, last;
	if (hdr) {
		src = m->hdr + seg * MOT_HDR_SEG;
		slen = m->hdrlen - seg * MOT_HDR_SEG;
		if (slen > MOT_HDR_SEG) slen = MOT_HDR_SEG;
		last = seg + 1 == m->nhseg;
	} else {
		src = m->body + seg * m->segsize;
		slen = m->bodylen - seg * m->segsize;
		if (slen > m->segsize) slen = m->segsize;
		last = seg + 1 == m->nseg;
	}
	uint8_t *d = m->du;
	memset(d, 0, sizeof m->du);
	bitw w = {d, 0};
	wbits(&w, 0, 1);                 /* extension flag */
	wbits(&w, 1, 1);                 /* CRC flag */
	wbits(&w, 1, 1);                 /* segment flag */
	wbits(&w, 1, 1);                 /* user access flag */
	wbits(&w, hdr ? 3 : 4, 4);       /* data group type */
	if (hdr) { wbits(&w, (unsigned)m->ci_hdr, 4); m->ci_hdr = (m->ci_hdr + 1) & 15; }
	else     { wbits(&w, (unsigned)m->ci_body, 4); m->ci_body = (m->ci_body + 1) & 15; }
	wbits(&w, 0, 4);                 /* repetition index */
	wbits(&w, (unsigned)last, 1);
	wbits(&w, (unsigned)seg, 15);
	wbits(&w, 0, 3);                 /* rfa */
	wbits(&w, 1, 1);                 /* transport id flag */
	wbits(&w, 2, 4);                 /* length indicator */
	wbits(&w, m->tid, 16);
	wbits(&w, 0, 3);                 /* segment header: repetition count */
	wbits(&w, (unsigned)slen, 13);
	int n = w.bit / 8;
	memcpy(d + n, src, slen);
	n += slen;
	unsigned crc = drm_crc(d, n, 16);
	d[n++] = (uint8_t)(crc >> 8);
	d[n++] = (uint8_t)crc;
	m->dulen = n;
	m->dupos = 0;
}

void mot_tx_frame(mot_tx *m, uint8_t *bits)
{
	memset(bits, 0, m->nbits);
	if (m->dulen == 0) {
		data_group(m, m->list[m->idx]);
		if (++m->idx == m->nlist) m->idx = 0;
	}
	uint8_t pk[512];
	memset(pk, 0, sizeof pk);
	int rem = m->dulen - m->dupos, L = m->paclen;
	int first = m->dupos == 0, lastp = rem <= L;
	pk[0] = (uint8_t)(first << 7 | lastp << 6 | 0 << 4 | (rem < L) << 3 | m->ci_pkt);
	m->ci_pkt = (m->ci_pkt + 1) & 7;
	if (rem >= L) {
		memcpy(pk + 1, m->du + m->dupos, L);
		m->dupos += L;
	} else {
		pk[1] = (uint8_t)rem;
		memcpy(pk + 2, m->du + m->dupos, rem);
		m->dupos += rem;
	}
	unsigned crc = drm_crc(pk, L + 1, 16);
	pk[L + 1] = (uint8_t)(crc >> 8);
	pk[L + 2] = (uint8_t)crc;
	for (int i = 0; i < (L + 3) * 8; i++) bits[i] = (pk[i >> 3] >> (7 - (i & 7))) & 1;
	if (lastp) {
		m->dulen = 0;
		/* prepare the next data unit right away, like the reference does */
	}
}

/* ======================================================================== */
/* receiver                                                                 */

typedef struct { const uint8_t *b; int bit, nbits; } bitr;

static unsigned rbits(bitr *r, int n)
{
	unsigned v = 0;
	for (int i = 0; i < n; i++, r->bit++) {
		int bb = r->bit < r->nbits ? (r->b[r->bit >> 3] >> (7 - (r->bit & 7))) & 1 : 0;
		v = v << 1 | (unsigned)bb;
	}
	return v;
}

void mot_rx_init(mot_rx *r, mot_done_fn done, void *user)
{
	memset(r, 0, sizeof *r);
	r->done = done;
	r->user = user;
	r->cur = -1;
}

static void obj_free(mot_obj *o)
{
	free(o->hdr);
	if (o->seg) for (int i = 0; i < o->nseg; i++) free(o->seg[i]);
	free(o->seg);
	free(o->seglen);
	memset(o, 0, sizeof *o);
}

void mot_rx_free(mot_rx *r)
{
	for (int i = 0; i < MOT_RX_OBJ; i++) obj_free(&r->obj[i]);
}

static long obj_prefix(const mot_obj *o, uint8_t **data)
{
	long tot = 0;
	int n = 0;
	while (n < o->nseg && o->seg[n]) tot += o->seglen[n++];
	if (!tot) return 0;
	*data = malloc(tot);
	long p = 0;
	for (int i = 0; i < n; i++) { memcpy(*data + p, o->seg[i], o->seglen[i]); p += o->seglen[i]; }
	return tot;
}

/* .rs1 .. .rs4: Reed-Solomon protected file -> type 1..4, else 0 */
static int rs_type(const char *name)
{
	const char *e = strrchr(name, '.');
	if (!e || strlen(e) != 4 || e[1] != 'r' || e[2] != 's' || e[3] < '1' || e[3] > '4') return 0;
	return e[3] - '0';
}

/* total body size, -1 if not known yet */
static long body_total(const mot_obj *o)
{
	if (o->bodysize > 0) return o->bodysize;
	if (o->last >= 0 && o->segsize && o->seg[o->last]) return (long)o->last * o->segsize + o->seglen[o->last];
	return -1;
}

/* try to rebuild a Reed-Solomon protected object with the missing segments as erasures */
static int rs_try(mot_rx *r, mot_obj *o, int force)
{
	int type = rs_type(o->name);
	long tot = body_total(o);
	if (!type || o->done || tot <= 0 || !o->segsize) return 0;
	int nseg = (int)((tot + o->segsize - 1) / o->segsize);
	/* enough segments for the code at all? (parity share of the file) */
	int need = (int)((long)nseg * rs_kk(type) / 255);
	if (o->nhave < need || (!force && o->nhave == o->rs_tried)) return 0;
	o->rs_tried = o->nhave;
	uint8_t *buf = calloc(tot, 1), *lost = calloc(tot, 1);
	for (int i = 0; i < nseg; i++) {
		long a = (long)i * o->segsize, b = a + o->segsize > tot ? tot : a + o->segsize;
		if (i < o->nseg && o->seg[i] && o->seglen[i] == b - a) memcpy(buf + a, o->seg[i], b - a);
		else memset(lost + a, 1, b - a);
	}
	long olen; char ext[4]; int fixed = 0;
	uint8_t *out = rs_file_decode(buf, tot, type, lost, &olen, ext, &fixed);
	free(buf); free(lost);
	if (!out) return 0;
	char name[128];
	snprintf(name, sizeof name, "%.*s.%s", (int)(strrchr(o->name, '.') - o->name), o->name, ext[0] ? ext : "jpg");
	o->done = 1;
	r->rs_fixed = fixed;
	if (r->done) r->done(r->user, name, out, olen, 0);
	free(out);
	return 1;
}

static void flush_obj(mot_rx *r, mot_obj *o)
{
	if (rs_type(o->name) && rs_try(r, o, 1)) return;
	if (!o->used || o->done || o->nhave == 0 || o->flushed >= o->nhave) return;
	uint8_t *d = NULL;
	long n = obj_prefix(o, &d);
	if (n > 0 && r->done) r->done(r->user, o->name[0] ? o->name : "drm_image.jpg", d, n, 1);
	free(d);
	o->flushed = o->nhave;
}

long mot_rx_prefix(const mot_rx *r, uint8_t **data)
{
	if (r->cur < 0) return 0;
	return obj_prefix(&r->obj[r->cur], data);
}

void mot_rx_flush(mot_rx *r)
{
	if (r->cur >= 0) flush_obj(r, &r->obj[r->cur]);
}

static mot_obj *get_obj(mot_rx *r, unsigned tid)
{
	int best = 0;
	for (int i = 0; i < MOT_RX_OBJ; i++)
		if (r->obj[i].used && r->obj[i].tid == tid) {
			if (r->cur >= 0 && r->cur != i) flush_obj(r, &r->obj[r->cur]);
			r->obj[i].age = ++r->clock; r->cur = i; return &r->obj[i];
		}
	if (r->cur >= 0) flush_obj(r, &r->obj[r->cur]);
	for (int i = 0; i < MOT_RX_OBJ; i++) {
		if (!r->obj[i].used) { best = i; break; }
		if (r->obj[i].age < r->obj[best].age) best = i;
	}
	mot_obj *o = &r->obj[best];
	obj_free(o);
	o->used = 1;
	o->tid = tid;
	o->age = ++r->clock;
	o->last = -1;
	r->cur = best;
	return o;
}

static void parse_header(mot_obj *o)
{
	bitr b = {o->hdr, 0, o->hdrlen * 8};
	if (o->hdrlen < 7) return;
	o->bodysize = (long)rbits(&b, 28);
	int hsize = (int)rbits(&b, 13);
	rbits(&b, 6); rbits(&b, 9);
	if (hsize > o->hdrlen) hsize = o->hdrlen;
	while (b.bit + 8 <= hsize * 8) {
		int pli = (int)rbits(&b, 2), id = (int)rbits(&b, 6), len = 0;
		if (pli == 1) len = 1;
		else if (pli == 2) len = 4;
		else if (pli == 3) { int ext = (int)rbits(&b, 1); len = (int)rbits(&b, ext ? 15 : 7); }
		int start = b.bit / 8;
		if (start + len > hsize) break;
		if (id == 12 && len > 1) {
			int n = len - 1;
			if (n > (int)sizeof o->name - 1) n = (int)sizeof o->name - 1;
			memcpy(o->name, o->hdr + start + 1, n);
			o->name[n] = 0;
			for (int i = 0; i < n; i++)    /* keep it a plain file name */
				if (o->name[i] == '/' || o->name[i] == '\\' || (unsigned char)o->name[i] < 32) o->name[i] = '_';
		}
		b.bit = (start + len) * 8;
	}
	o->hdrok = 1;
}

static void check_done(mot_rx *r, mot_obj *o)
{
	if (o->done) return;
	if (o->hdrok && rs_type(o->name)) { rs_try(r, o, o->last >= 0 && o->nhave >= o->last + 1); return; }
	if (!o->hdrok || o->last < 0 || o->nhave < o->last + 1) return;
	long tot = 0;
	for (int i = 0; i <= o->last; i++) tot += o->seglen[i];
	if (o->bodysize && tot != o->bodysize) return;
	uint8_t *buf = malloc(tot ? tot : 1);
	long p = 0;
	for (int i = 0; i <= o->last; i++) { memcpy(buf + p, o->seg[i], o->seglen[i]); p += o->seglen[i]; }
	o->done = 1;
	if (r->done) r->done(r->user, o->name[0] ? o->name : "drm_image.jpg", buf, tot, 0);
	free(buf);
}

static void data_group_rx(mot_rx *r, const uint8_t *d, int n)
{
	if (n < 4) { r->dg_bad++; return; }
	bitr b = {d, 0, n * 8};
	int ext = (int)rbits(&b, 1), crcf = (int)rbits(&b, 1), segf = (int)rbits(&b, 1), uaf = (int)rbits(&b, 1);
	int type = (int)rbits(&b, 4);
	rbits(&b, 8);
	if (crcf) {
		unsigned c = (unsigned)d[n - 2] << 8 | d[n - 1];
		if (drm_crc(d, n - 2, 16) != c) { r->dg_bad++; return; }
		n -= 2;
		b.nbits = n * 8;
	}
	r->dg_ok++;
	if (ext) rbits(&b, 16);
	int last = 0, segnum = 0;
	if (segf) { last = (int)rbits(&b, 1); segnum = (int)rbits(&b, 15); }
	unsigned tid = 0; int havetid = 0;
	if (uaf) {
		rbits(&b, 3);
		havetid = (int)rbits(&b, 1);
		int len = (int)rbits(&b, 4);
		if (havetid) { tid = rbits(&b, 16); len -= 2; }
		if (len > 0) b.bit += len * 8;
	}
	if (!havetid || !segf || (type != 3 && type != 4)) return;
	rbits(&b, 3);
	int ssize = (int)rbits(&b, 13);
	int start = b.bit / 8;
	if (start + ssize > n) return;
	mot_obj *o = get_obj(r, tid);
	if (type == 3) {
		if (o->hdrok && o->done && segnum == 0 && (ssize != o->hdrlen || memcmp(o->hdr, d + start, ssize))) {
			unsigned t2 = o->tid;                       /* same id, new object (e.g. the next bsr.bin) */
			obj_free(o);
			o->used = 1; o->tid = t2; o->age = ++r->clock; o->last = -1;
		}
		if (o->hdrok) return;
		/* headers are small: keep segments in order */
		if (segnum == 0) { free(o->hdr); o->hdr = NULL; o->hdrlen = 0; }
		if (segnum * MOT_HDR_SEG != o->hdrlen) return;
		o->hdr = realloc(o->hdr, o->hdrlen + ssize);
		memcpy(o->hdr + o->hdrlen, d + start, ssize);
		o->hdrlen += ssize;
		if (last) parse_header(o);
	} else {
		if (segnum >= 32768) return;
		if (segnum >= o->nseg) {
			int nn = segnum + 64;
			o->seg = realloc(o->seg, sizeof(uint8_t *) * nn);
			o->seglen = realloc(o->seglen, sizeof(int) * nn);
			for (int i = o->nseg; i < nn; i++) o->seg[i] = NULL, o->seglen[i] = 0;
			o->nseg = nn;
		}
		if (last) o->last = segnum;
		else if (!o->segsize) o->segsize = ssize;
		if (!o->seg[segnum]) {
			o->seg[segnum] = malloc(ssize ? ssize : 1);
			memcpy(o->seg[segnum], d + start, ssize);
			o->seglen[segnum] = ssize;
			o->nhave++;
		}
	}
	check_done(r, o);
}

void mot_rx_packet(mot_rx *r, const uint8_t *pk, int nb)
{
	int L = nb - 3;
	unsigned c = (unsigned)pk[nb - 2] << 8 | pk[nb - 1];
	if (L < 2 || drm_crc(pk, nb - 2, 16) != c) { r->pk_bad++; r->duok = 0; return; }
	r->pk_ok++;
	int first = pk[0] >> 7 & 1, last = pk[0] >> 6 & 1, ppi = pk[0] >> 3 & 1;
	const uint8_t *d = pk + 1;
	int n = L;
	if (ppi) { n = pk[1]; d = pk + 2; if (n > L - 1) n = L - 1; }
	if (first) { r->dulen = 0; r->duok = 1; }
	if (!r->duok) return;
	if (r->dulen + n > (int)sizeof r->du) { r->duok = 0; return; }
	memcpy(r->du + r->dulen, d, n);
	r->dulen += n;
	if (last) {
		data_group_rx(r, r->du, r->dulen);
		r->duok = 0;
		r->dulen = 0;
	}
}

void mot_rx_progress(const mot_rx *r, int *have, int *total, const char **name)
{
	*have = 0; *total = 0; *name = "";
	if (r->cur < 0) return;
	const mot_obj *o = &r->obj[r->cur];
	*have = o->nhave;
	if (o->last >= 0) *total = o->last + 1;
	else if (o->bodysize && o->segsize) *total = (int)((o->bodysize + o->segsize - 1) / o->segsize);
	*name = o->name;
}

int mot_rx_missing(const mot_rx *r, int *list, int max)
{
	if (r->cur < 0) return 0;
	const mot_obj *o = &r->obj[r->cur];
	int tot = o->last >= 0 ? o->last + 1 : (o->bodysize && o->segsize ? (int)((o->bodysize + o->segsize - 1) / o->segsize) : 0);
	int n = 0;
	for (int i = 0; i < tot && n < max; i++)
		if (i >= o->nseg || !o->seg[i]) list[n++] = i;
	return n;
}

/* ---- broadcast segment request (BSR), EasyPal / QSSTV compatible ---------- */

int mot_rx_bsr(const mot_rx *r, char *out, int max)
{
	if (r->cur < 0) return 0;
	const mot_obj *o = &r->obj[r->cur];
	if (o->done || !o->nhave) return 0;
	long tot = body_total(o);
	int nseg = tot > 0 && o->segsize ? (int)((tot + o->segsize - 1) / o->segsize) : (o->last >= 0 ? o->last + 1 : 0);
	if (!nseg) { for (int i = 0; i < o->nseg; i++) if (o->seg[i]) nseg = i + 1; }
	if (max < 64) return 0;
	int n = 0, first = 1, prev = -10, run = 0, last_missing = -1;
	max -= 16;                                /* room for the closing lines, always */
	n += snprintf(out + n, max - n, "%u\nH_OK\n%d\n", o->tid, o->segsize ? o->segsize : 1);
	for (int i = 0; i < nseg && n < max - 20; i++) {
		if (i < o->nseg && o->seg[i]) continue;
		if (first) { n += snprintf(out + n, max - n, "%d\n", i); first = 0; }
		else if (i == prev + 1) run = 1;
		else {
			if (run) { n += snprintf(out + n, max - n, "-1\n%d\n", prev); run = 0; }
			n += snprintf(out + n, max - n, "%d\n", i);
		}
		prev = i; last_missing = i;
	}
	if (first) return 0;                      /* nothing missing */
	max += 16;
	if (n > max - 16) n = max - 16;
	if (run) n += snprintf(out + n, max - n, "-1\n%d\n", last_missing);
	if (n > max - 5) n = max - 5;
	n += snprintf(out + n, max - n, "-99\n");
	return n;
}

int mot_bsr_parse(const char *txt, long len, unsigned *tid, int *list, int max)
{
	char buf[8192];
	if (len >= (long)sizeof buf) len = sizeof buf - 1;
	memcpy(buf, txt, len);
	buf[len] = 0;
	char *save = NULL, *line[4];
	int nl = 0;
	char *t = strtok_r(buf, "\r\n", &save);
	while (t && nl < 4) { line[nl++] = t; if (nl < 4) t = strtok_r(NULL, "\r\n", &save); }
	if (nl < 4 || strcmp(line[1], "H_OK")) return -1;
	*tid = (unsigned)strtoul(line[0], NULL, 10);
	int n = 0, lastb = atoi(line[3]), series = 0;
	if (n < max) list[n++] = lastb;
	lastb++;
	while ((t = strtok_r(NULL, "\r\n", &save))) {
		int b = atoi(t);
		if (b == -99) break;
		if (b < 0) { series = 1; continue; }
		if (series) { for (int j = lastb; j < b && n < max; j++) list[n++] = j; series = 0; }
		if (n < max) list[n++] = b;
		lastb = b + 1;
	}
	return n;
}
