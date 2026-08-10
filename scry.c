#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#if defined(__linux__)
#  include <pty.h>
#elif defined(__APPLE__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__)
#  include <util.h>
#endif

#include <stdint.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

/* ---------------- sigils: edit, then recompile ---------------- */
#define FONTNAME  "Iosevka"   /* Base Xft font family or pattern */
#define COLS0     80
#define ROWS0     24
#define FG        0xa6ff9eUL
#define BG        0x0a0014UL
#define CURSORCLR 0xff66ffUL
#define SHELL     "/bin/sh"
#define BORDERPX  2
/* ---------------------------------------------------------------- */

static unsigned long palette[256];

static void init_palette(void) {
    unsigned long sys16[16] = {
        0x0a0014,0xff5f5f,0x7dffb0,0xffe66d,0x9d7dff,0xff7dff,0x7dfaff,0xd8d8e6,
        0x2a1a3d,0xff8f8f,0xa6ffca,0xfff0a0,0xc2a6ff,0xffb0ff,0xa6faff,0xffffff,
    };
    memcpy(palette, sys16, sizeof(sys16));

    for (int i = 0; i < 216; i++) {
        int r = i / 36, g = (i / 6) % 6, b = i % 6;
        palette[16 + i] = ((r * 51) << 16) | ((g * 51) << 8) | (b * 51);
    }

    for (int i = 0; i < 24; i++) {
        int v = i * 10 + 8;
        palette[232 + i] = (v << 16) | (v << 8) | v;
    }
}

typedef struct { uint32_t code; unsigned char fg, bg, bold, rev; } Cell;
static Cell *grid = NULL;
static Cell *alt_grid = NULL;
static int rows = ROWS0, cols = COLS0;
static int cx = 0, cy = 0, savx = 0, savy = 0;
static int alt_savx = 0, alt_savy = 0;
static int curfg = -1, curbg = -1, curbold = 0, currev = 0;
static int cursor_visible = 1;
static int in_alt_screen = 0;

static int ptyfd;
static pid_t childpid;
static volatile sig_atomic_t running = 1;

static Display *dpy;
static Window win;
static XftDraw *xftdraw;
static XftFont *font;
static Atom wmDelete;
static int cw, ch;
static int font_size = 12;
static char font_pattern[256] = FONTNAME;

static void draw(void);
static void spawn_shell(void);
static void switch_alt(int on);

static void on_sigchld(int s){ (void)s; running = 0; }

/* Converts 24-bit RGB hex (0xRRGGBB) to 16-bit XRenderColor components */
static void make_xcolor(unsigned long hex, XRenderColor *c) {
    c->red   = ((hex >> 16) & 0xff) * 0x0101;
    c->green = ((hex >> 8)  & 0xff) * 0x0101;
    c->blue  = (hex         & 0xff) * 0x0101;
    c->alpha = 0xffff;
}

static int utf8_decode(const unsigned char *src, size_t len, uint32_t *out, size_t *consumed) {
    if (len == 0) return 0;
    unsigned char c = src[0];
    if (c < 0x80) {
        *out = c;
        *consumed = 1;
        return 1;
    }
    int need = 0;
    if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else {
        *out = c;
        *consumed = 1;
        return 1;
    }

    if (len < (size_t)need) return 0;

    uint32_t cp = 0;
    if (need == 2) cp = c & 0x1F;
    else if (need == 3) cp = c & 0x0F;
    else if (need == 4) cp = c & 0x07;

    for (int i = 1; i < need; i++) {
        if ((src[i] & 0xC0) != 0x80) {
            *out = c;
            *consumed = 1;
            return 1;
        }
        cp = (cp << 6) | (src[i] & 0x3F);
    }
    *out = cp;
    *consumed = (size_t)need;
    return 1;
}

static void spawn_shell(void) {
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    childpid = forkpty(&ptyfd, NULL, NULL, &ws);
    if (childpid < 0) { perror("forkpty"); exit(1); }
    if (childpid == 0) {
        setenv("TERM", "xterm-256color", 1);
        execl(SHELL, SHELL, NULL);
        perror("execl");
        _exit(1);
    }
}

static void clearcell(int y, int x) {
    grid[y*cols + x] = (Cell){ .code = ' ', .fg = 255, .bg = 255, .bold = 0, .rev = 0 };
}

static void erase_display(int mode) {
    if (mode == 2) { for (int y=0;y<rows;y++) for (int x=0;x<cols;x++) clearcell(y,x); return; }
    if (mode == 0) {
        for (int x=cx;x<cols;x++) clearcell(cy,x);
        for (int y=cy+1;y<rows;y++) for (int x=0;x<cols;x++) clearcell(y,x);
    } else if (mode == 1) {
        for (int x=0;x<=cx;x++) clearcell(cy,x);
        for (int y=0;y<cy;y++) for (int x=0;x<cols;x++) clearcell(y,x);
    }
}

static void erase_line(int mode) {
    if (mode == 2) { for (int x=0;x<cols;x++) clearcell(cy,x); return; }
    if (mode == 0) for (int x=cx;x<cols;x++) clearcell(cy,x);
    else if (mode == 1) for (int x=0;x<=cx;x++) clearcell(cy,x);
}

static void scroll1(void) {
    memmove(grid, grid + cols, sizeof(Cell) * cols * (rows-1));
    for (int x=0;x<cols;x++) clearcell(rows-1,x);
}

static void newline(void) { cy++; if (cy >= rows) { scroll1(); cy = rows-1; } }

static void put_codepoint(uint32_t code) {
    if (code == '\n') { newline(); return; }
    if (code == '\r') { cx = 0; return; }
    if (code == '\b') { if (cx > 0) cx--; return; }
    if (code == '\t') { cx = (cx/8+1)*8; if (cx >= cols) cx = cols-1; return; }
    if (code < 0x20) return;
    if (cx >= cols) { cx = 0; newline(); }
    grid[cy*cols + cx] = (Cell){ .code = code,
        .fg = curfg<0?255:(unsigned char)curfg,
        .bg = curbg<0?255:(unsigned char)curbg,
        .bold = (unsigned char)curbold,
        .rev = (unsigned char)currev };
    cx++;
}

static int params[16], nparam = 0, curparam = -1;
static int pstate = 0;

static void sgr(void) {
    if (nparam == 0) { curfg = curbg = -1; curbold = currev = 0; return; }
    for (int i=0;i<nparam;i++) {
        int p = params[i];
        if (p == 0) { curfg = curbg = -1; curbold = currev = 0; }
        else if (p == 1) curbold = 1;
        else if (p == 22) curbold = 0;
        else if (p == 7) currev = 1;
        else if (p == 27) currev = 0;
        else if (p >= 30 && p <= 37) curfg = p - 30;
        else if (p == 39) curfg = -1;
        else if (p >= 40 && p <= 47) curbg = p - 40;
        else if (p == 49) curbg = -1;
        else if (p >= 90 && p <= 97) curfg = p - 90 + 8;
        else if (p >= 100 && p <= 107) curbg = p - 100 + 8;
        else if (p == 38 || p == 48) {
            int isfg = (p == 38);
            if (i+2 < nparam && params[i+1] == 5) {
                int idx = params[i+2];
                if (isfg) curfg = idx; else curbg = idx;
                i += 2;
            } else if (i+1 < nparam && params[i+1] == 2) i += 4;
        }
    }
}

static void csi_dispatch(char final) {
    int n = (nparam > 0 && params[0]) ? params[0] : 1;
    switch (final) {
        case 'A': cy -= n; if (cy < 0) cy = 0; break;
        case 'B': cy += n; if (cy >= rows) cy = rows-1; break;
        case 'C': cx += n; if (cx >= cols) cx = cols-1; break;
        case 'D': cx -= n; if (cx < 0) cx = 0; break;
        case 'H': case 'f':
            cy = (nparam > 0 ? params[0] : 1) - 1;
            cx = (nparam > 1 ? params[1] : 1) - 1;
            if (cy < 0) cy = 0; if (cy >= rows) cy = rows-1;
            if (cx < 0) cx = 0; if (cx >= cols) cx = cols-1;
            break;
        case 'J': erase_display(nparam > 0 ? params[0] : 0); break;
        case 'K': erase_line(nparam > 0 ? params[0] : 0); break;
        case 's': if (in_alt_screen) { alt_savx = cx; alt_savy = cy; } else { savx = cx; savy = cy; } break;
        case 'u': if (in_alt_screen) { cx = alt_savx; cy = alt_savy; } else { cx = savx; cy = savy; } break;
        case 'm': sgr(); break;
        case 'n':
            if (nparam == 1 && params[0] == 6) {
                char buf[32];
                snprintf(buf, sizeof buf, "\x1b[%d;%dR", cy+1, cx+1);
                write(ptyfd, buf, strlen(buf));
            }
            break;
        case 'h':
            if (nparam >= 1 && params[0] == 25) cursor_visible = 1;
            if (nparam >= 1 && params[0] == 1049) switch_alt(1);
            break;
        case 'l':
            if (nparam >= 1 && params[0] == 25) cursor_visible = 0;
            if (nparam >= 1 && params[0] == 1049) switch_alt(0);
            break;
        default: break;
    }
}

static void feed_byte(unsigned char c) {
    switch (pstate) {
        case 0:
            if (c == 0x1b) pstate = 1;
            else put_codepoint(c);
            return;
        case 1:
            if (c == '[') { pstate = 3; nparam = 0; curparam = -1; memset(params,0,sizeof params); }
            else if (c == ']') pstate = 4;
            else if (c >= 0x28 && c <= 0x2f) pstate = 2;
            else pstate = 0;
            return;
        case 2: pstate = 0; return;
        case 4:
            if (c == 0x07) pstate = 0;
            else if (c == 0x1b) pstate = 5;
            return;
        case 5: pstate = 0; return;
        case 3:
            if (c == '?' || c == '>' || c == '=') return;
            if (c >= '0' && c <= '9') { curparam = (curparam < 0 ? 0 : curparam)*10 + (c-'0'); return; }
            if (c == ';') { if (nparam < 16) params[nparam++] = curparam < 0 ? 0 : curparam; curparam = -1; return; }
            if (c < 0x40 || c > 0x7e) return;
            if (nparam < 16) params[nparam++] = curparam < 0 ? 0 : curparam;
            csi_dispatch(c);
            pstate = 0;
            return;
    }
}

static void switch_alt(int on) {
    if (on && !in_alt_screen) {
        savx = cx; savy = cy;
        if (!alt_grid) alt_grid = calloc(rows * cols, sizeof(Cell));
        else {
            for (int i = 0; i < rows*cols; i++)
                alt_grid[i] = (Cell){ .code=' ', .fg=255, .bg=255, .bold=0, .rev=0 };
        }
        Cell *tmp = grid; grid = alt_grid; alt_grid = tmp;
        cx = 0; cy = 0;
        in_alt_screen = 1;
    } else if (!on && in_alt_screen) {
        Cell *tmp = grid; grid = alt_grid; alt_grid = tmp;
        cx = savx; cy = savy;
        in_alt_screen = 0;
    }
}

static void process_codepoint(uint32_t cp) {
    if (cp <= 0x7F) {
        feed_byte((unsigned char)cp);
    } else {
        put_codepoint(cp);
    }
}

static void update_font_metrics(void) {
    XGlyphInfo ext;
    XftTextExtents8(dpy, font, (const FcChar8 *)"M", 1, &ext);
    cw = ext.xOff;
    if (cw <= 0) cw = font->max_advance_width;
    if (cw <= 0) cw = 8;
    ch = font->ascent + font->descent;
    if (ch <= 0) ch = 16;
}

static void xinit(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "scry: cannot open display\n"); exit(1); }
    int scr = DefaultScreen(dpy);

    const char *env_font = getenv("SCRY_FONT");
    if (env_font && env_font[0] != '\0') {
        strncpy(font_pattern, env_font, sizeof(font_pattern) - 1);
        font_pattern[sizeof(font_pattern) - 1] = '\0';
    }

    char pat[256];
    snprintf(pat, sizeof pat, "%s:size=%d", font_pattern, font_size);
    font = XftFontOpenName(dpy, scr, pat);
    if (!font) {
        fprintf(stderr, "scry: cannot load font '%s'\n", pat);
        exit(1);
    }
    update_font_metrics();

    win = XCreateSimpleWindow(dpy, RootWindow(dpy,scr), 0, 0,
        cols * cw, rows * ch, BORDERPX, BlackPixel(dpy,scr), BG);
    XStoreName(dpy, win, "scry");
    wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);
    xftdraw = XftDrawCreate(dpy, win, DefaultVisual(dpy,scr), DefaultColormap(dpy,scr));
    grid = calloc(rows * cols, sizeof(Cell));
    for (int i=0;i<rows*cols;i++) clearcell(i/cols, i%cols);
}

static void destroy_font(void) {
    if (font) XftFontClose(dpy, font);
    font = NULL;
}

static void set_font_size(int new_size) {
    if (new_size < 6) new_size = 6;
    if (new_size > 72) new_size = 72;
    font_size = new_size;
    char pat[256];
    snprintf(pat, sizeof pat, "%s:size=%d", font_pattern, font_size);
    int scr = DefaultScreen(dpy);
    XftFont *newfont = XftFontOpenName(dpy, scr, pat);
    if (!newfont) {
        fprintf(stderr, "scry: cannot load font '%s'\n", pat);
        return;
    }
    destroy_font();
    font = newfont;
    update_font_metrics();
    XResizeWindow(dpy, win, cols * cw, rows * ch);
    draw();
}

static Cell *create_resized_grid(Cell *old_grid, int old_cols, int old_rows, int new_cols, int new_rows) {
    Cell *new_grid = calloc(new_rows * new_cols, sizeof(Cell));
    for (int i = 0; i < new_rows * new_cols; i++) {
        new_grid[i] = (Cell){ .code=' ', .fg=255, .bg=255, .bold=0, .rev=0 };
    }
    int copy_rows = (old_rows < new_rows) ? old_rows : new_rows;
    int copy_cols = (old_cols < new_cols) ? old_cols : new_cols;
    if (old_grid) {
        for (int y = 0; y < copy_rows; y++) {
            for (int x = 0; x < copy_cols; x++) {
                new_grid[y * new_cols + x] = old_grid[y * old_cols + x];
            }
        }
        free(old_grid);
    }
    return new_grid;
}

static void resize_term(int new_cols, int new_rows) {
    if (new_cols == cols && new_rows == rows) return;

    grid = create_resized_grid(grid, cols, rows, new_cols, new_rows);
    if (alt_grid) {
        alt_grid = create_resized_grid(alt_grid, cols, rows, new_cols, new_rows);
    }
    cols = new_cols;
    rows = new_rows;

    if (cx >= cols) cx = cols-1;
    if (cy >= rows) cy = rows-1;
    if (savx >= cols) savx = cols-1;
    if (savy >= rows) savy = rows-1;
    if (alt_savx >= cols) alt_savx = cols-1;
    if (alt_savy >= rows) alt_savy = rows-1;

    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(ptyfd, TIOCSWINSZ, &ws);
    kill(childpid, SIGWINCH);
}

static void draw(void) {
    XftColor fgcol, bgcol, cursorcol;
    XRenderColor rfg, rbg, rcursor;
    
    make_xcolor(FG, &rfg);
    make_xcolor(BG, &rbg);
    make_xcolor(CURSORCLR, &rcursor);

    XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                       DefaultColormap(dpy, DefaultScreen(dpy)), &rfg, &fgcol);
    XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                       DefaultColormap(dpy, DefaultScreen(dpy)), &rbg, &bgcol);
    XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                       DefaultColormap(dpy, DefaultScreen(dpy)), &rcursor, &cursorcol);

    for (int y=0; y<rows; y++) {
        int x=0;
        while (x < cols) {
            Cell c0 = grid[y*cols + x];
            int start = x;
            uint32_t run[cols];
            int run_len = 0;
            while (x < cols && run_len < cols) {
                Cell c = grid[y*cols + x];
                if (c.fg != c0.fg || c.bg != c0.bg || c.bold != c0.bold || c.rev != c0.rev)
                    break;
                run[run_len++] = c.code ? c.code : ' ';
                x++;
            }
            unsigned long fg = c0.fg == 255 ? FG :
                palette[(c0.bold && c0.fg < 8) ? c0.fg + 8 : c0.fg];
            unsigned long bg = c0.bg == 255 ? BG : palette[c0.bg];
            if (c0.rev) { unsigned long t = fg; fg = bg; bg = t; }
            
            XRenderColor rf, rb;
            make_xcolor(fg, &rf);
            make_xcolor(bg, &rb);

            XftColor fcol, bcol;
            XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                               DefaultColormap(dpy, DefaultScreen(dpy)), &rf, &fcol);
            XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                               DefaultColormap(dpy, DefaultScreen(dpy)), &rb, &bcol);
            XftDrawRect(xftdraw, &bcol, start * cw, y * ch, run_len * cw, ch);
            char utf8_buf[run_len * 4 + 1];
            int off = 0;
            for (int i=0; i<run_len; i++) {
                uint32_t cp = run[i];
                if (cp < 0x80) utf8_buf[off++] = (char)cp;
                else if (cp < 0x800) { utf8_buf[off++] = 0xC0 | (cp >> 6); utf8_buf[off++] = 0x80 | (cp & 0x3F); }
                else if (cp < 0x10000) { utf8_buf[off++] = 0xE0 | (cp >> 12); utf8_buf[off++] = 0x80 | ((cp >> 6) & 0x3F); utf8_buf[off++] = 0x80 | (cp & 0x3F); }
                else { utf8_buf[off++] = 0xF0 | (cp >> 18); utf8_buf[off++] = 0x80 | ((cp >> 12) & 0x3F); utf8_buf[off++] = 0x80 | ((cp >> 6) & 0x3F); utf8_buf[off++] = 0x80 | (cp & 0x3F); }
            }
            utf8_buf[off] = '\0';
            XftDrawStringUtf8(xftdraw, &fcol, font, start * cw, y * ch + font->ascent,
                              (const FcChar8*)utf8_buf, off);
            XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                         DefaultColormap(dpy, DefaultScreen(dpy)), &fcol);
            XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                         DefaultColormap(dpy, DefaultScreen(dpy)), &bcol);
        }
    }
    if (cursor_visible) {
        XftDrawRect(xftdraw, &cursorcol, cx * cw, cy * ch, cw, ch);
    }
    XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                 DefaultColormap(dpy, DefaultScreen(dpy)), &fgcol);
    XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                 DefaultColormap(dpy, DefaultScreen(dpy)), &bgcol);
    XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                 DefaultColormap(dpy, DefaultScreen(dpy)), &cursorcol);
    XFlush(dpy);
}

static void send_key(XKeyEvent *ev) {
    char buf[32]; KeySym ks;
    int n = XLookupString(ev, buf, sizeof buf, &ks, NULL);
    const char *seq = NULL;
    switch (ks) {
        case XK_Up: seq="\x1b[A"; break;
        case XK_Down: seq="\x1b[B"; break;
        case XK_Right: seq="\x1b[C"; break;
        case XK_Left: seq="\x1b[D"; break;
        case XK_Home: seq="\x1b[H"; break;
        case XK_End: seq="\x1b[F"; break;
        case XK_Delete: seq="\x1b[3~"; break;
        case XK_BackSpace: buf[0]=0x7f; n=1; break;
        case XK_plus:
        case XK_equal:
            if (ev->state & ControlMask) { set_font_size(font_size+1); return; }
            break;
        case XK_minus:
            if (ev->state & ControlMask) { set_font_size(font_size-1); return; }
            break;
        case XK_0:
            if (ev->state & ControlMask) { set_font_size(12); return; }
            break;
        default: break;
    }
    if (seq) write(ptyfd, seq, strlen(seq));
    else if (n > 0) write(ptyfd, buf, n);
}

int main(void) {
    init_palette();
    signal(SIGCHLD, on_sigchld);
    spawn_shell();
    xinit();
    draw(); /* Draw terminal window immediately upon launch */

    int xfd = ConnectionNumber(dpy);
    unsigned char readbuf[4096];
    while (running) {
        fd_set rf; FD_ZERO(&rf); FD_SET(xfd,&rf); FD_SET(ptyfd,&rf);
        int maxfd = xfd>ptyfd?xfd:ptyfd;
        struct timeval tv = {0, 30000};
        int r = select(maxfd+1, &rf, NULL, NULL, &tv);
        if (r<0) { if (errno==EINTR) continue; break; }
        if (r>0 && FD_ISSET(ptyfd,&rf)) {
            ssize_t n = read(ptyfd, readbuf, sizeof readbuf);
            if (n <= 0) break;
            size_t off = 0;
            while (off < (size_t)n) {
                uint32_t cp;
                size_t consumed = 0;
                int ok = utf8_decode(readbuf + off, (size_t)n - off, &cp, &consumed);
                if (!ok || consumed == 0) {
                    cp = readbuf[off];
                    consumed = 1;
                }
                process_codepoint(cp);
                off += consumed;
            }
            draw();
        }
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy,&ev);
            if (ev.type == Expose) draw();
            else if (ev.type == KeyPress) send_key(&ev.xkey);
            else if (ev.type == ConfigureNotify) {
                int new_cols = ev.xconfigure.width / cw;
                int new_rows = ev.xconfigure.height / ch;
                if (new_cols > 0 && new_rows > 0) resize_term(new_cols, new_rows);
            } else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wmDelete) {
                running = 0; break;
            }
        }
    }
    if (childpid > 0) kill(childpid, SIGHUP);
    XftDrawDestroy(xftdraw);
    destroy_font();
    XCloseDisplay(dpy);
    if (grid) free(grid);
    if (alt_grid) free(alt_grid);
    return 0;
}
