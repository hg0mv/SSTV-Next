/* winx.h - the small part of Xlib that SSTV Next uses, on top of Win32
 *
 * The program draws everything into its own framebuffer (ui.c), so only a
 * window, its events and a blit are needed. Event and key codes follow X11,
 * which keeps the main program identical on Linux and Windows.
 */
#ifndef WINX_H
#define WINX_H

#include <windows.h>

typedef struct { int unused; } Display;
typedef HWND Window;
typedef void *GC;
typedef unsigned long Atom, KeySym, XID;
typedef int Bool;
typedef struct { int class; unsigned long red_mask, green_mask, blue_mask; } Visual;
typedef struct { long flags; int x, y, width, height, min_width, min_height; } XSizeHints;
typedef struct { int dummy; } XComposeStatus;
typedef struct { int dummy; } XImage;

#ifndef False
#define False 0
#define True 1
#endif
#define TrueColor 4
#define NoSymbol 0L
#define PMinSize (1L << 4)

#define ExposureMask        (1L << 15)
#define KeyPressMask        (1L << 0)
#define ButtonPressMask     (1L << 2)
#define ButtonReleaseMask   (1L << 3)
#define ButtonMotionMask    (1L << 13)
#define StructureNotifyMask (1L << 17)

#define KeyPress        2
#define KeyRelease      3
#define ButtonPress     4
#define ButtonRelease   5
#define MotionNotify    6
#define Expose          12
#define ConfigureNotify 22
#define ClientMessage   33

#define ShiftMask   (1 << 0)
#define ControlMask (1 << 2)
#define Button1Mask (1 << 8)

typedef struct { int type; int count; } XExposeEvent;
typedef struct { int type; int width, height; } XConfigureEvent;
typedef struct { int type; unsigned state; KeySym ks; char text[8]; int len; } XKeyEvent;
typedef struct { int type; int x, y; unsigned button, state; } XButtonEvent;
typedef struct { int type; int x, y; unsigned state; } XMotionEvent;
typedef struct { int type; union { long l[5]; } data; } XClientMessageEvent;
typedef union {
	int type;
	XExposeEvent xexpose;
	XConfigureEvent xconfigure;
	XKeyEvent xkey;
	XButtonEvent xbutton;
	XMotionEvent xmotion;
	XClientMessageEvent xclient;
	long pad[8];
} XEvent;

/* key symbols (X11 values) */
#define XK_BackSpace     0xff08
#define XK_Tab           0xff09
#define XK_Return        0xff0d
#define XK_Escape        0xff1b
#define XK_Delete        0xffff
#define XK_Home          0xff50
#define XK_Left          0xff51
#define XK_Up            0xff52
#define XK_Right         0xff53
#define XK_Down          0xff54
#define XK_Prior         0xff55
#define XK_Next          0xff56
#define XK_End           0xff57
#define XK_KP_Enter      0xff8d
#define XK_KP_Home       0xff95
#define XK_KP_Left       0xff96
#define XK_KP_Right      0xff98
#define XK_KP_End        0xff9c
#define XK_KP_Delete     0xff9f
#define XK_F1            0xffbe
#define XK_ISO_Left_Tab  0xfe20
#define XK_q             0x0071

#define DefaultScreen(d)    0
#define DefaultVisual(d, s) (&winx_visual)
#define DefaultDepth(d, s)  24
#define RootWindow(d, s)    ((Window)0)
#define BlackPixel(d, s)    0UL
#define WhitePixel(d, s)    0xffffffUL
#define AllPlanes           (~0UL)

extern Visual winx_visual;

Display *XOpenDisplay(const char *name);
Window   XCreateSimpleWindow(Display *d, Window parent, int x, int y, unsigned w, unsigned h,
                             unsigned bw, unsigned long border, unsigned long bg);
int  XStoreName(Display *d, Window w, const char *name);
int  XSelectInput(Display *d, Window w, long mask);
void XSetWMNormalHints(Display *d, Window w, XSizeHints *h);
typedef int (*XIOErrorHandler)(Display *);
XIOErrorHandler XSetIOErrorHandler(XIOErrorHandler h);
Atom XInternAtom(Display *d, const char *name, Bool only);
int  XSetWMProtocols(Display *d, Window w, Atom *a, int n);
GC   XCreateGC(Display *d, Window w, unsigned long mask, void *values);
int  XFreeGC(Display *d, GC gc);
int  XMapWindow(Display *d, Window w);
int  XPending(Display *d);
int  XNextEvent(Display *d, XEvent *e);
Bool XCheckTypedWindowEvent(Display *d, Window w, int type, XEvent *e);
int  XLookupString(XKeyEvent *e, char *buf, int n, KeySym *ks, XComposeStatus *st);
int  XFlush(Display *d);
int  XDestroyWindow(Display *d, Window w);
int  XCloseDisplay(Display *d);

/* for ui.c: the window's 32 bit framebuffer (0x00RRGGBB, top-down) */
uint32_t *winx_framebuffer(int w, int h);
void      winx_blit(int x, int y, int w, int h);

#endif
