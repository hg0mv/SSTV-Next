/* winx.c - Win32 implementation of the Xlib subset in winx.h */
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <string.h>
#include "winx.h"

Visual winx_visual = { TrueColor, 0xff0000, 0x00ff00, 0x0000ff };

static Display the_display;
static HWND hwnd;
static int min_w = 200, min_h = 150;
static int buttons;                       /* X button mask held */

/* event queue */
#define QN 512
static XEvent q[QN];
static int qh, qt;
static int qcount(void) { return (qt - qh + QN) % QN; }
static void push(const XEvent *e)
{
	if (qcount() == QN - 1) qh = (qh + 1) % QN;      /* full: drop the oldest */
	q[qt] = *e;
	qt = (qt + 1) % QN;
}

/* framebuffer: a DIB section, drawn into directly by ui.c */
static HDC memdc;
static HBITMAP dib, olddib;
static uint32_t *bits;
static int fbw, fbh;

uint32_t *winx_framebuffer(int w, int h)
{
	if (memdc && dib) { SelectObject(memdc, olddib); DeleteObject(dib); dib = NULL; }
	if (!memdc) memdc = CreateCompatibleDC(NULL);
	BITMAPINFO bi;
	memset(&bi, 0, sizeof bi);
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;                       /* top-down */
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *p = NULL;
	dib = CreateDIBSection(memdc, &bi, DIB_RGB_COLORS, &p, NULL, 0);
	if (!dib) { bits = NULL; return NULL; }
	olddib = SelectObject(memdc, dib);
	bits = p;
	fbw = w; fbh = h;
	memset(bits, 0, (size_t)w * h * 4);
	return bits;
}

void winx_blit(int x, int y, int w, int h)
{
	if (!hwnd || !dib) return;
	GdiFlush();
	HDC dc = GetDC(hwnd);
	BitBlt(dc, x, y, w, h, memdc, x, y, SRCCOPY);
	ReleaseDC(hwnd, dc);
}

/* ---- keyboard ---------------------------------------------------------- */

static int utf8(unsigned cp, char *o)
{
	if (cp < 0x80) { o[0] = (char)cp; return 1; }
	if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
	o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63));
	return 3;
}

static unsigned mods(void)
{
	unsigned s = (unsigned)buttons;
	if (GetKeyState(VK_SHIFT) & 0x8000) s |= ShiftMask;
	if (GetKeyState(VK_CONTROL) & 0x8000) s |= ControlMask;
	return s;
}

static KeySym special_key(WPARAM vk, LPARAM lp)
{
	switch (vk) {
	case VK_BACK: return XK_BackSpace;
	case VK_TAB: return (GetKeyState(VK_SHIFT) & 0x8000) ? XK_ISO_Left_Tab : XK_Tab;
	case VK_RETURN: return (lp & (1 << 24)) ? XK_KP_Enter : XK_Return;
	case VK_ESCAPE: return XK_Escape;
	case VK_DELETE: return XK_Delete;
	case VK_LEFT: return XK_Left;
	case VK_UP: return XK_Up;
	case VK_RIGHT: return XK_Right;
	case VK_DOWN: return XK_Down;
	case VK_PRIOR: return XK_Prior;
	case VK_NEXT: return XK_Next;
	case VK_HOME: return XK_Home;
	case VK_END: return XK_End;
	case VK_F1: return XK_F1;
	default: return NoSymbol;
	}
}

static void button_event(int type, unsigned b, LPARAM lp)
{
	XEvent e;
	memset(&e, 0, sizeof e);
	e.type = type;
	e.xbutton.x = GET_X_LPARAM(lp);
	e.xbutton.y = GET_Y_LPARAM(lp);
	e.xbutton.button = b;
	e.xbutton.state = mods();
	push(&e);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	XEvent e;
	memset(&e, 0, sizeof e);
	switch (m) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		if (dib) {
			GdiFlush();
			BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
			       ps.rcPaint.bottom - ps.rcPaint.top, memdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
		}
		EndPaint(h, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_SIZE:
		if (wp != SIZE_MINIMIZED && LOWORD(lp) > 0 && HIWORD(lp) > 0) {
			e.type = ConfigureNotify;
			e.xconfigure.width = LOWORD(lp);
			e.xconfigure.height = HIWORD(lp);
			push(&e);
		}
		return 0;
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mi = (MINMAXINFO *)lp;
		RECT r = { 0, 0, min_w, min_h };
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
		mi->ptMinTrackSize.x = r.right - r.left;
		mi->ptMinTrackSize.y = r.bottom - r.top;
		return 0;
	}
	case WM_CLOSE:
		e.type = ClientMessage;
		e.xclient.data.l[0] = 1;                       /* the WM_DELETE_WINDOW atom */
		push(&e);
		return 0;
	case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: {
		unsigned b = m == WM_LBUTTONDOWN ? 1 : m == WM_MBUTTONDOWN ? 2 : 3;
		SetCapture(h);
		button_event(ButtonPress, b, lp);
		buttons |= Button1Mask << (b - 1);
		return 0;
	}
	case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: {
		unsigned b = m == WM_LBUTTONUP ? 1 : m == WM_MBUTTONUP ? 2 : 3;
		buttons &= ~(Button1Mask << (b - 1));
		if (!buttons) ReleaseCapture();
		button_event(ButtonRelease, b, lp);
		return 0;
	}
	case WM_MOUSEWHEEL: {
		POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(h, &p);
		unsigned b = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 4 : 5;
		LPARAM cl = MAKELPARAM(p.x, p.y);
		button_event(ButtonPress, b, cl);
		button_event(ButtonRelease, b, cl);
		return 0;
	}
	case WM_MOUSEMOVE:
		if (buttons) {
			e.type = MotionNotify;
			e.xmotion.x = GET_X_LPARAM(lp);
			e.xmotion.y = GET_Y_LPARAM(lp);
			e.xmotion.state = mods();
			push(&e);
		}
		return 0;
	case WM_KEYDOWN: {
		KeySym ks = special_key(wp, lp);
		if (ks != NoSymbol) {
			e.type = KeyPress;
			e.xkey.ks = ks;
			e.xkey.state = mods();
			push(&e);
			return 0;
		}
		break;                                         /* characters come with WM_CHAR */
	}
	case WM_CHAR: {
		unsigned cp = (unsigned)wp;
		if (cp < 32 || cp == 127 || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
		e.type = KeyPress;
		e.xkey.ks = cp < 0x100 ? cp : 0x01000000 + cp;
		e.xkey.len = utf8(cp, e.xkey.text);
		e.xkey.state = mods();
		push(&e);
		return 0;
	}
	}
	return DefWindowProcW(h, m, wp, lp);
}

/* ---- Xlib calls ---------------------------------------------------------- */

Display *XOpenDisplay(const char *name)
{
	(void)name;
	static int reg;
	if (!reg) {
		WNDCLASSEXW wc;
		memset(&wc, 0, sizeof wc);
		wc.cbSize = sizeof wc;
		wc.style = CS_DBLCLKS;
		wc.lpfnWndProc = wndproc;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
		wc.hIconSm = wc.hIcon;
		wc.lpszClassName = L"SSTVNext";
		if (!RegisterClassExW(&wc)) return NULL;
		reg = 1;
	}
	return &the_display;
}

Window XCreateSimpleWindow(Display *d, Window parent, int x, int y, unsigned w, unsigned h,
                           unsigned bw, unsigned long border, unsigned long bg)
{
	(void)d; (void)parent; (void)x; (void)y; (void)bw; (void)border; (void)bg;
	RECT r = { 0, 0, (LONG)w, (LONG)h };
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	int ww = r.right - r.left, wh = r.bottom - r.top;
	/* keep it on the screen (small laptop displays) */
	RECT wa;
	if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
		if (ww > wa.right - wa.left) ww = wa.right - wa.left;
		if (wh > wa.bottom - wa.top) wh = wa.bottom - wa.top;
	}
	hwnd = CreateWindowExW(0, L"SSTVNext", L"SSTV Next", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                       ww, wh, NULL, NULL, GetModuleHandleW(NULL), NULL);
	return hwnd;
}

int XStoreName(Display *d, Window w, const char *name)
{
	(void)d;
	wchar_t t[256];
	MultiByteToWideChar(CP_UTF8, 0, name, -1, t, 256);
	SetWindowTextW(w, t);
	return 1;
}

int XSelectInput(Display *d, Window w, long mask) { (void)d; (void)w; (void)mask; return 1; }

void XSetWMNormalHints(Display *d, Window w, XSizeHints *h)
{
	(void)d; (void)w;
	if (h->flags & PMinSize) { min_w = h->min_width; min_h = h->min_height; }
}

XIOErrorHandler XSetIOErrorHandler(XIOErrorHandler h) { (void)h; return NULL; }
Atom XInternAtom(Display *d, const char *name, Bool only) { (void)d; (void)name; (void)only; return 1; }
int  XSetWMProtocols(Display *d, Window w, Atom *a, int n) { (void)d; (void)w; (void)a; (void)n; return 1; }
GC   XCreateGC(Display *d, Window w, unsigned long mask, void *values) { (void)d; (void)w; (void)mask; (void)values; return NULL; }
int  XFreeGC(Display *d, GC gc) { (void)d; (void)gc; return 1; }

int XMapWindow(Display *d, Window w)
{
	(void)d;
	ShowWindow(w, SW_SHOWNORMAL);
	UpdateWindow(w);
	XEvent e;
	memset(&e, 0, sizeof e);
	e.type = Expose;
	push(&e);
	return 1;
}

static void pump(void)
{
	MSG msg;
	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

int XPending(Display *d)
{
	(void)d;
	pump();
	return qcount();
}

int XNextEvent(Display *d, XEvent *e)
{
	(void)d;
	while (!qcount()) {
		MSG msg;
		if (GetMessageW(&msg, NULL, 0, 0) <= 0) {
			memset(e, 0, sizeof *e);
			e->type = ClientMessage;
			e->xclient.data.l[0] = 1;
			return 0;
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	*e = q[qh];
	qh = (qh + 1) % QN;
	return 0;
}

Bool XCheckTypedWindowEvent(Display *d, Window w, int type, XEvent *e)
{
	(void)d; (void)w;
	for (int i = qh; i != qt; i = (i + 1) % QN)
		if (q[i].type == type) {
			*e = q[i];
			for (int j = i; (j + 1) % QN != qt; j = (j + 1) % QN) q[j] = q[(j + 1) % QN];   /* close the gap */
			qt = (qt - 1 + QN) % QN;
			return True;
		}
	return False;
}

int XLookupString(XKeyEvent *e, char *buf, int n, KeySym *ks, XComposeStatus *st)
{
	(void)st;
	if (ks) *ks = e->ks;
	int len = e->len < n ? e->len : 0;
	if (len) memcpy(buf, e->text, len);
	if (len < n) buf[len] = 0;
	return len;
}

int XFlush(Display *d) { (void)d; GdiFlush(); return 1; }
int XDestroyWindow(Display *d, Window w) { (void)d; DestroyWindow(w); hwnd = NULL; return 1; }
int XCloseDisplay(Display *d) { (void)d; return 1; }
