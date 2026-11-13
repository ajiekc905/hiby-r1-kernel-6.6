// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 ajiekc905
/*
 * r1screen.c — the firmware's own full-screen message: shown by r1boot.sh when
 * the player on the card keeps crashing, or there is no player on the card.
 *
 * Lives in the firmware (bundle card-player-boot), so it must work with no
 * card, no fonts, no libraries: static binary, glyphs baked in (font.h, made
 * by mkfont.py), draws straight into /dev/fb0.
 *
 *   r1screen -t <title> [-m <message>] [-l <log file>] [-w <path>]
 *            [-i <idle s>] [-o <out.ppm> -g <W>x<H>]
 *
 * Exit: 0 the user asked to try again (tap / play / enter)
 *       3 switch off (Vol- pressed, or idle for -i seconds, default 600)
 *       4 the -w path appeared (e.g. a player copied to the card)
 *       2 no framebuffer
 * The last lines of -l that fit are shown, with the wlan0 address
 * and the battery level, refreshed every second. The
 * backlight goes off after 60 s without input; the next input only wakes it.
 * -o renders one frame to a PPM instead (tests), no input, exit 0.
 *
 * Build (static, any mipsel musl GCC; ../../build.sh does it for PROD=1):
 *   mipsel-linux-musl-gcc -static -O2 -Wall -Wextra -o r1screen r1screen.c
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font.h"

#define BG 0x0b3a8cu
#define FG 0xffffffu
#define DIM 0xb4c6e6u
#define BACKLIGHT "/sys/class/backlight/backlight_pwm0/brightness"
#define MAXDEV 16

static int W, H;
static uint32_t *cv;

static void fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
            cv[j * W + i] = c;
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned k) {
    unsigned r = (((a >> 16) & 255) * (15 - k) + ((b >> 16) & 255) * k) / 15;
    unsigned g = (((a >> 8) & 255) * (15 - k) + ((b >> 8) & 255) * k) / 15;
    unsigned bl = ((a & 255) * (15 - k) + (b & 255) * k) / 15;
    return r << 16 | g << 8 | bl;
}

static unsigned utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && !n) { *s = (const char *)p; return '?'; }
    if (n) c &= 0x3f >> n;
    while (n--) {
        if ((*p & 0xc0) != 0x80) { *s = (const char *)p; return '?'; }
        c = c << 6 | (*p++ & 0x3f);
    }
    *s = (const char *)p;
    return c;
}

static const rg_glyph *find(const rg_font *f, unsigned cp) {
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f->g[m].cp == cp) return &f->g[m];
        if (f->g[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return cp == '?' ? NULL : find(f, '?');
}

static int glyph(const rg_font *f, int x, int y, unsigned cp, uint32_t c) {
    if (cp == '\t') cp = ' ';
    const rg_glyph *g = find(f, cp);
    if (!g) return 0;
    for (int j = 0; j < g->h; j++)
        for (int i = 0; i < g->w; i++) {
            int k = j * g->w + i;
            unsigned a = (f->bits[g->off + (k >> 1)] >> (k & 1 ? 4 : 0)) & 15;
            int px = x + g->x + i, py = y + g->y + j;
            if (a && px >= 0 && px < W && py >= 0 && py < H)
                cv[py * W + px] = mix(cv[py * W + px], c, a);
        }
    return g->adv;
}

static int width(const rg_font *f, const char *s, int n) {
    int w = 0;
    const char *e = s + n;
    while (s < e && *s) {
        const rg_glyph *g = find(f, utf8(&s));
        if (g) w += g->adv;
    }
    return w;
}

static void text(const rg_font *f, int x, int y, const char *s, int n, uint32_t c) {
    const char *e = s + n;
    while (s < e && *s) x += glyph(f, x, y, utf8(&s), c);
}

/* one line of s that fits in w (breaks at the last space when there is one) */
static int fit(const rg_font *f, const char *s, int w) {
    int n = 0, sp = -1, x = 0;
    while (s[n] && s[n] != '\n') {
        const char *p = s + n;
        const rg_glyph *g = find(f, utf8(&p));
        int adv = g ? g->adv : 0;
        if (x + adv > w && n) return sp > 0 ? sp : n;
        if (s[n] == ' ') sp = n;
        x += adv;
        n = (int)(p - s);
    }
    return n;
}

/* wrapped paragraph, returns the y below it */
static int para(const rg_font *f, int x, int y, int w, const char *s, uint32_t c, int maxy) {
    while (*s && y + f->line <= maxy) {
        int n = fit(f, s, w);
        text(f, x, y, s, n, c);
        y += f->line;
        s += n;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return y;
}

static void wlan_ip(char *out, size_t n) {
    struct ifreq r;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    out[0] = 0;
    if (s < 0) return;
    memset(&r, 0, sizeof r);
    strncpy(r.ifr_name, "wlan0", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &r) == 0)
        snprintf(out, n, "%s", inet_ntoa(((struct sockaddr_in *)&r.ifr_addr)->sin_addr));
    close(s);
}

static int read_int(const char *path) {
    char b[32];
    int fd = open(path, O_RDONLY), n;
    if (fd < 0) return -1;
    n = (int)read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    return atoi(b);
}

static void write_int(const char *path, int v) {
    char b[16];
    int fd = open(path, O_WRONLY), n = snprintf(b, sizeof b, "%d\n", v);
    if (fd < 0) return;
    if (write(fd, b, n) < 0) {}
    close(fd);
}

/* the last lines of the log that fit, each cut to the width */
static void log_tail(const rg_font *f, const char *path, int x, int y, int w, int maxy) {
    static char buf[16384];
    int fd = open(path, O_RDONLY), n = 0;
    if (fd < 0) return;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end > 0) {
        off_t at = end > (off_t)sizeof buf - 1 ? end - (off_t)sizeof buf + 1 : 0;
        lseek(fd, at, SEEK_SET);
        n = (int)read(fd, buf, sizeof buf - 1);
    }
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    int rows = (maxy - y) / f->line;
    if (rows <= 0) return;
    const char *lines[64];
    int lens[64], cnt = 0;
    char *e = buf + n;
    while (e > buf && (e[-1] == '\n' || e[-1] == '\r')) e--;
    while (e > buf && cnt < rows && cnt < 64) {
        char *b = e;
        while (b > buf && b[-1] != '\n') b--;
        if (b == buf && end > n) break;      /* a cut-off first line: skip */
        lines[cnt] = b;
        lens[cnt] = (int)(e - b);
        cnt++;
        e = b > buf ? b - 1 : b;
        if (e > buf && e[-1] == '\r') e--;
    }
    for (int i = cnt - 1; i >= 0; i--) {
        int len = lens[i];
        while (len > 0 && width(f, lines[i], len) > w) {
            len--;
            while (len > 0 && (lines[i][len] & 0xc0) == 0x80) len--;
        }
        text(f, x, y, lines[i], len, DIM);
        y += f->line;
    }
}

static void draw(const char *title, const char *msg, const char *logf, int asleep) {
    const rg_font *b = &rg_body, *t = &rg_title;
    int m = W / 12, cw = W - 2 * m, y = H / 14;
    char ip[32], line[96];
    fill(0, 0, W, H, BG);
    if (asleep) return;
    glyph(&rg_face, m, y, ':', FG);
    glyph(&rg_face, m + rg_face.g[1].adv, y, '(', FG);
    y += rg_face.line + t->line / 2;
    y = para(t, m, y, cw, title, FG, H);
    y += b->line / 2;
    if (msg && *msg) y = para(b, m, y, cw, msg, FG, H) + b->line / 2;

    int foot = H - H / 20 - 3 * b->line;
    if (logf) {
        fill(m, y, cw, 1, DIM);
        log_tail(&rg_small, logf, m, y + b->line / 2, cw, foot - b->line / 2);
    }
    fill(m, foot - b->line / 3, cw, 1, DIM);
    para(b, m, foot, cw, "Tap or press Play to try again", FG, H);
    para(b, m, foot + b->line, cw, "Vol\xe2\x80\x93: switch off", FG, H);
    wlan_ip(ip, sizeof ip);
    int bat = read_int("/sys/class/power_supply/battery/capacity");
    snprintf(line, sizeof line, "%s%s%s", ip[0] ? "Wi-Fi " : "Wi-Fi: no address", ip, "");
    if (bat >= 0) {
        size_t l = strlen(line);
        snprintf(line + l, sizeof line - l, "   \xc2\xb7   battery %d%%", bat);
    }
    para(b, m, foot + 2 * b->line, cw, line, DIM, H);
}

/* fb: 16 or 32 bpp, channel layout from the var info; SLCD panels need the pan */
static int fbfd = -1;
static uint8_t *fbm;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;

static int fb_open(void) {
    fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) || ioctl(fbfd, FBIOGET_FSCREENINFO, &fi))
        return -1;
    if (vi.bits_per_pixel != 16 && vi.bits_per_pixel != 32) return -1;
    fbm = mmap(NULL, fi.line_length * vi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbm == MAP_FAILED) return -1;
    W = (int)vi.xres;
    H = (int)vi.yres;
    ioctl(fbfd, FBIOBLANK, FB_BLANK_UNBLANK);
    return 0;
}

static void fb_show(void) {
    for (int y = 0; y < H; y++) {
        uint8_t *row = fbm + (size_t)y * fi.line_length;
        for (int x = 0; x < W; x++) {
            uint32_t c = cv[y * W + x];
            unsigned r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
            uint32_t v = (r >> (8 - vi.red.length)) << vi.red.offset |
                         (g >> (8 - vi.green.length)) << vi.green.offset |
                         (b >> (8 - vi.blue.length)) << vi.blue.offset;
            if (vi.transp.length) v |= ((1u << vi.transp.length) - 1) << vi.transp.offset;
            if (vi.bits_per_pixel == 16) ((uint16_t *)row)[x] = (uint16_t)v;
            else ((uint32_t *)row)[x] = v;
        }
    }
    vi.yoffset = 0;
    ioctl(fbfd, FBIOPAN_DISPLAY, &vi);
}

static int ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 2;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char p[3] = { cv[i] >> 16 & 255, cv[i] >> 8 & 255, cv[i] & 255 };
        fwrite(p, 1, 3, f);
    }
    return fclose(f) ? 2 : 0;
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

int main(int argc, char **argv) {
    const char *title = "Something went wrong", *msg = NULL, *logf = NULL, *watch = NULL, *out = NULL;
    int idle = 600, c;
    while ((c = getopt(argc, argv, "t:m:l:w:i:o:g:")) != -1) {
        switch (c) {
        case 't': title = optarg; break;
        case 'm': msg = optarg; break;
        case 'l': logf = optarg; break;
        case 'w': watch = optarg; break;
        case 'i': idle = atoi(optarg); break;
        case 'o': out = optarg; break;
        case 'g': if (sscanf(optarg, "%dx%d", &W, &H) != 2) W = H = 0; break;
        default:
            fprintf(stderr, "usage: r1screen -t title [-m msg] [-l log] [-w path] [-i idle_s] [-o out.ppm -g WxH]\n");
            return 2;
        }
    }
    if (out) {
        if (W < 120 || H < 160) return 2;
        cv = calloc((size_t)W * H, 4);
        if (!cv) return 2;
        draw(title, msg, logf, 0);
        return ppm(out);
    }
    if (fb_open()) {
        fprintf(stderr, "r1screen: no usable /dev/fb0\n");
        return 2;
    }
    cv = calloc((size_t)W * H, 4);
    if (!cv) return 2;

    struct pollfd pf[MAXDEV];
    int nd = 0;
    for (int i = 0; i < 32 && nd < MAXDEV; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) { pf[nd].fd = fd; pf[nd].events = POLLIN; nd++; }
    }
    int bl = read_int(BACKLIGHT);
    if (bl <= 0) { bl = 28; write_int(BACKLIGHT, bl); }

    long start = now_ms(), last_in = start;
    int asleep = 0, rc = -1;
    while (rc < 0) {
        draw(title, msg, logf, asleep);
        fb_show();
        long until = now_ms() + 1000;
        while (rc < 0) {
            long left = until - now_ms();
            if (left <= 0) break;
            if (poll(pf, (nfds_t)nd, (int)left) <= 0) continue;
            for (int i = 0; i < nd; i++) {
                struct input_event ev[16];
                ssize_t r;
                if (!(pf[i].revents & POLLIN)) continue;
                while ((r = read(pf[i].fd, ev, sizeof ev)) > 0) {
                    for (int k = 0; k < (int)(r / (ssize_t)sizeof ev[0]); k++) {
                        int press = (ev[k].type == EV_KEY && ev[k].value == 1) ||
                                    (ev[k].type == EV_ABS && ev[k].code == ABS_MT_TRACKING_ID && ev[k].value >= 0);
                        if (!press || now_ms() - start < 1000) continue;
                        last_in = now_ms();
                        if (asleep) {
                            asleep = 0;
                            write_int(BACKLIGHT, bl);
                            until = 0;
                            continue;
                        }
                        if (ev[k].type == EV_KEY && ev[k].code == KEY_VOLUMEDOWN) rc = 3;
                        else if (ev[k].type == EV_KEY && ev[k].code == KEY_VOLUMEUP) continue;
                        else rc = 0;
                    }
                }
            }
        }
        if (rc >= 0) break;
        struct stat st;
        if (watch && stat(watch, &st) == 0) rc = 4;
        else if (idle > 0 && now_ms() - last_in > idle * 1000L) rc = 3;
        else if (!asleep && now_ms() - last_in > 60000) {
            asleep = 1;
            write_int(BACKLIGHT, 0);
        }
    }
    if (asleep) write_int(BACKLIGHT, bl);
    if (rc == 0) {
        fill(0, 0, W, H, BG);
        para(&rg_title, W / 12, H / 2 - rg_title.line, W - W / 6, "Starting\xe2\x80\xa6", FG, H);
        fb_show();
    }
    return rc;
}
