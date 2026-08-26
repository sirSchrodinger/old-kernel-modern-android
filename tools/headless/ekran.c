/*
 * ekran - the handset's status panel, drawn without a framework.
 *
 * This device is not used as a phone but as a box that sits on a network and
 * does work.  For that job the Android framework is pure load: measured, with
 * zygote + system_server + surfaceflinger running, 30-50 MB of 825 MB stays
 * free and the device visibly stalls; with the framework stopped, 705 MB free
 * and load ~0.
 *
 * So whatever draws the screen cannot depend on the framework either.  This
 * program writes straight to the framebuffer - the same thing Android's own
 * `charger` mode does.  No surfaceflinger, no SurfaceView, no font engine:
 * the glyphs are baked into the binary (font.h), the data comes from sysfs
 * and from plain-text files.
 *
 * Four things this cost, none of which produce an error:
 *
 *   MCDE composites alpha.  A 32bpp pixel with alpha 0 is fully transparent:
 *   the write succeeds, every ioctl returns 0, and the panel shows nothing.
 *
 *   There are three framebuffers (yres_virtual 2400 / yres 800).  Draw into
 *   the one that is not visible, then FBIOPAN_DISPLAY.  Never FBIOBLANK per
 *   frame - that is visible flicker.
 *
 *   The touchscreen (mxt224s) registers early_suspend and only enables its
 *   IRQ from mxt_resume(), reachable only via late_resume.  With no framework
 *   that transition never happens and touch stays silent forever.  One
 *   suspend->resume cycle fixes it; unbind/bind does not.
 *
 *   The single-instance lock must be O_CLOEXEC.  This program forks for
 *   scans and jobs, children inherit open descriptors, and one child that
 *   wedges in an uninterruptible kernel path holds that lock forever - the
 *   panel then never starts again and nothing says why.  A lock that leaks
 *   into your own children is not a lock.
 *
 * Build: arm-linux-gnueabihf-gcc -O2 -static -o ekran ekran.c
 * Use:   ekran [-d /dev/graphics/fb0] [-s seconds] [-1] [-o out.ppm]
 *              [-p page] [-k source] [-t]
 *
 * -o matters: a wrong status screen is silently wrong - font, layout and
 * non-ASCII glyphs break without any error - so the output has to be
 * inspectable without going to the device.  -p/-k pick page and data source
 * and work against live data without disturbing a running panel.
 *
 * NOTE: the inline commentary below is in Turkish.  The design rationale is
 * summarised in English in the repository README.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <signal.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/wireless.h>

#include "font.h"

static const char *ISLER = "/data/sirsch/isler.txt";
static const char *PANEL_DIZIN = "/data/sirsch/panel";

/* Battery percentage, taken seriously.
 *
 * The instantaneous reading is not a state of charge, it is a state of load.
 * Measured from nod's ring on 26 August: with the current pinned at -171 mA the
 * voltage still walked 3597..3648 mV, 51 mV of jitter, and feeding that
 * straight through a curve made the screen show 30%, then 40%, then 18% inside
 * a few minutes.  A cell cannot do that.
 *
 * Two corrections, both cheap:
 *   1. Median of the last samples, not the last sample.  The median throws
 *      away transients instead of averaging them in.
 *   2. Open-circuit voltage: V_oc = V + I*R.  Under 171 mA of draw a cell with
 *      200 mOhm of internal resistance reads 34 mV low, which is most of the
 *      error at this end of the curve.
 *
 * R starts at a provisional 200 mOhm and is meant to be replaced by a fitted
 * value: sirsch-pil-arsiv.sh accumulates voltage/current pairs across days and
 * load levels, and R is the slope of V against I at constant charge.  Put the
 * fitted number in /data/sirsch/pil-r-mohm.
 */

/* Percent from voltage, not from the fuel gauge.
 *
 * This handset's gauge is wrong often enough to be useless: measured on
 * 26 August it reported 2% while the cell sat at 3552 mV, which is about 12%.
 * The same chip is the one that reported a temperature of -1066830336 - the
 * raw bit pattern of a float printed through an integer formatter - and sent
 * the framework into a shutdown loop.  So the number on the screen is derived
 * from voltage, and the gauge is only used to flag disagreement.
 * Same curve as nod, deliberately: two instruments that disagree about the
 * same cell would be worse than one. */
static int yuzde_gerilimden(int mv)
{
    static const int e[][2] = {
        {4200, 100}, {4100, 92}, {4000, 82}, {3950, 74}, {3900, 66},
        {3850, 57},  {3800, 48}, {3750, 40}, {3700, 32}, {3650, 24},
        {3600, 16},  {3500, 8},  {3400, 3},  {3300, 0},
    };
    int n = (int)(sizeof e / sizeof e[0]), i;
    if (mv >= e[0][0]) return 100;
    if (mv <= e[n - 1][0]) return 0;
    for (i = 0; i < n - 1; i++)
        if (mv <= e[i][0] && mv > e[i + 1][0]) {
            int dv = e[i][0] - e[i + 1][0], dp = e[i][1] - e[i + 1][1];
            return e[i + 1][1] + (mv - e[i + 1][0]) * dp / dv;
        }
    return 0;
}

/* ---------- framebuffer ---------- */

static unsigned char *fb;
static size_t fb_boy;
static int fb_g, fb_y, fb_bpp, fb_satir;
static int r_off, g_off, b_off, r_len, g_len, b_len, a_off, a_len;
static int fbd = -1;
static struct fb_var_screeninfo var;
static int tampon_sayisi = 1, aktif_tampon = 0;
static size_t kare_boy;

/* Alpha is not optional here.  This mcde layer composites, so a 32bpp pixel
   written with alpha 0 is fully transparent: the write succeeds, the pixel is
   in the mapping, and the panel shows nothing.  That is exactly what happened
   on 26 August - the framebuffer read back 000a0e10, the right colour with an
   empty alpha, while the screen stayed black and every ioctl returned 0. */
static unsigned long renk(int r, int g, int b)
{
    unsigned long v =
           ((unsigned long)(r >> (8 - r_len)) << r_off)
         | ((unsigned long)(g >> (8 - g_len)) << g_off)
         | ((unsigned long)(b >> (8 - b_len)) << b_off);
    if (a_len) v |= ((1UL << a_len) - 1) << a_off;
    return v;
}

/* Draw into the buffer that is NOT on screen.  Clearing and repainting the
   live buffer is what produced the black flash every refresh: for a few
   milliseconds the panel was scanning out a background-coloured page with no
   text on it yet.  The panel already has three buffers (yres_virtual 2400 for
   an 800-row screen); using them is free. */
static unsigned char *cizim_yuzeyi(void)
{
    return fb + (size_t)((aktif_tampon + 1) % tampon_sayisi) * kare_boy;
}

static void nokta(int x, int y, unsigned long c)
{
    unsigned char *p;
    if (x < 0 || y < 0 || x >= fb_g || y >= fb_y) return;
    p = cizim_yuzeyi() + (size_t)y * fb_satir + (size_t)x * (fb_bpp / 8);
    if (fb_bpp == 16)      *(unsigned short *)p = (unsigned short)c;
    else if (fb_bpp == 32) *(unsigned int *)p = (unsigned int)c;
    else if (fb_bpp == 24) { p[0] = c & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = (c >> 16) & 0xFF; }
}

static void kutu(int x, int y, int g, int h, unsigned long c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < g; i++)
            nokta(x + i, y + j, c);
}

/* Render into plain memory instead of a panel.  A status screen that is wrong
   is wrong silently - the font, the layout and the Turkish glyphs all fail
   without an error - so there has to be a way to look at the output before it
   ever reaches the device. */
static int fb_bellek(int g, int y)
{
    fb_g = g; fb_y = y; fb_bpp = 32; fb_satir = g * 4;
    tampon_sayisi = 1; aktif_tampon = 0; kare_boy = (size_t)fb_satir * fb_y;
    r_off = 16; r_len = 8; g_off = 8; g_len = 8; b_off = 0; b_len = 8; a_off = 24; a_len = 8;
    fb_boy = (size_t)fb_satir * fb_y;
    fb = calloc(1, fb_boy);
    return fb ? 0 : -1;
}

static int ppm_yaz(const char *yol)
{
    FILE *f = fopen(yol, "wb");
    int x, y;
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", fb_g, fb_y);
    for (y = 0; y < fb_y; y++)
        for (x = 0; x < fb_g; x++) {
            unsigned int v = *(unsigned int *)(cizim_yuzeyi() + (size_t)y * fb_satir + x * 4);
            unsigned char p[3];
            p[0] = (v >> r_off) & 0xFF;
            p[1] = (v >> g_off) & 0xFF;
            p[2] = (v >> b_off) & 0xFF;
            fwrite(p, 1, 3, f);
        }
    fclose(f);
    return 0;
}

static int fb_ac(const char *yol)
{
    struct fb_fix_screeninfo sabit;

    fbd = open(yol, O_RDWR);
    if (fbd < 0) return -1;
    if (ioctl(fbd, FBIOGET_VSCREENINFO, &var) < 0) return -1;
    if (ioctl(fbd, FBIOGET_FSCREENINFO, &sabit) < 0) return -1;

    fb_g = var.xres; fb_y = var.yres; fb_bpp = var.bits_per_pixel;
    fb_satir = sabit.line_length;
    r_off = var.red.offset;   r_len = var.red.length;
    g_off = var.green.offset; g_len = var.green.length;
    b_off = var.blue.offset;  b_len = var.blue.length;
    a_off = var.transp.offset; a_len = var.transp.length;
    /* Some mcde panels report zero-length channels before the first mode set;
       fall back to the layout that matches the reported depth. */
    if (r_len == 0 || g_len == 0 || b_len == 0) {
        if (fb_bpp == 16) { r_off=11; r_len=5; g_off=5; g_len=6; b_off=0; b_len=5; a_len=0; }
        else              { r_off=16; r_len=8; g_off=8; g_len=8; b_off=0; b_len=8; a_off=24; a_len=8; }
    }
    kare_boy = (size_t)fb_satir * fb_y;
    tampon_sayisi = fb_y ? (int)(var.yres_virtual / fb_y) : 1;
    if (tampon_sayisi < 1) tampon_sayisi = 1;
    aktif_tampon = fb_y ? (int)(var.yoffset / fb_y) : 0;
    fb_boy = (size_t)fb_satir * var.yres_virtual;
    fb = mmap(NULL, fb_boy, PROT_READ | PROT_WRITE, MAP_SHARED, fbd, 0);
    if (fb == MAP_FAILED) { fb = NULL; return -1; }
    /* Stopping the framework takes surfaceflinger down with it, and the panel
       goes to FB_BLANK_POWERDOWN behind it.  Writing into the mapping then
       succeeds and shows nothing: the pixels are there, the panel is off.
       Measured exactly that on 26 August - "fb 480x800 32bpp" printed while
       the screen stayed black. */
    ioctl(fbd, FBIOBLANK, FB_BLANK_UNBLANK);
    return 0;
}

static void fb_bas(void)
{
    int yeni;

    if (fbd < 0) return;   /* bellek kipi */
    /* FBIOBLANK BURADA YOK.
       Zaten acik bir paneli her karede yeniden "unblank" etmek, mcde'ye
       kendini yeniden kurdurup gorunur bir titreme uretiyor - 26 Agustos:
       cift tamponlama titremeyi kaldirmisti, bu satiri ekleyince geri geldi.
       Panel yalnizca uyandirilirken acilir (isik_ac). */

    yeni = (aktif_tampon + 1) % tampon_sayisi;
    var.xoffset = 0;
    var.yoffset = (unsigned)yeni * fb_y;
    var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (ioctl(fbd, FBIOPAN_DISPLAY, &var) == 0)
        aktif_tampon = yeni;
    else {
        /* Panning refused: fall back to drawing where the panel is already
           looking, which flickers but is better than a frozen screen. */
        ioctl(fbd, FBIOPUT_VSCREENINFO, &var);
        aktif_tampon = yeni;
    }
}

/* ---------- yazi ---------- */

static int glif(unsigned int kod)
{
    int i;
    if (kod >= 32 && kod < 127) return (int)kod - 32;
    for (i = 95; i < FONT_N; i++) if (FONT_KOD[i] == kod) return i;
    return '?' - 32;
}

/* UTF-8 -> codepoint; ilerletilen bayt sayisini *ilerle'ye yazar */
static unsigned int utf8(const unsigned char *s, int *ilerle)
{
    if (s[0] < 0x80)              { *ilerle = 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) { *ilerle = 2; return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        *ilerle = 3;
        return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    *ilerle = 1; return '?';
}

static void harf(int x, int y, int idx, int olcek, unsigned long c)
{
    int sy, sx;
    for (sy = 0; sy < FONT_Y; sy++) {
        unsigned int satir = (FONT[idx][sy * 2] << 8) | FONT[idx][sy * 2 + 1];
        for (sx = 0; sx < FONT_G; sx++)
            if (satir & (1u << (FONT_G - 1 - sx)))
                kutu(x + sx * olcek, y + sy * olcek, olcek, olcek, c);
    }
}

static int metin_sinir(int x, int y, const char *s, int olcek, unsigned long c, int sag)
{
    const unsigned char *p = (const unsigned char *)s;
    int adim;
    if (sag > fb_g - 8) sag = fb_g - 8;
    while (*p) {
        unsigned int kod = utf8(p, &adim);
        p += adim;
        if (x + FONT_G * olcek > sag) break;
        harf(x, y, glif(kod), olcek, c);
        x += FONT_G * olcek;
    }
    return x;
}

static int metin(int x, int y, const char *s, int olcek, unsigned long c)
{
    const unsigned char *p = (const unsigned char *)s;
    int adim;
    while (*p) {
        unsigned int kod = utf8(p, &adim);
        p += adim;
        if (x + FONT_G * olcek > fb_g - 8) break;   /* panelden tasma */
        harf(x, y, glif(kod), olcek, c);
        x += FONT_G * olcek;
    }
    return x;
}

/* ---------- veri toplama ---------- */

static int oku_sayi(const char *yol, long *out)
{
    char tampon[64];
    int f = open(yol, O_RDONLY), n;
    if (f < 0) return -1;
    n = read(f, tampon, sizeof(tampon) - 1);
    close(f);
    if (n <= 0) return -1;
    tampon[n] = 0;
    *out = strtol(tampon, NULL, 10);
    return 0;
}

static int oku_metin(const char *yol, char *out, size_t boy)
{
    int f = open(yol, O_RDONLY), n;
    if (f < 0) { out[0] = 0; return -1; }
    n = read(f, out, boy - 1);
    close(f);
    if (n < 0) n = 0;
    out[n] = 0;
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ')) out[--n] = 0;
    return 0;
}

#define ORNEK 30          /* halkadan okunacak son kayit sayisi (~5 dakika) */

struct durum {
    long mv, ma, yuzde, olcer, mv_oc;
    long kalan_dk;      /* + : bosalmaya kalan, - : dolmaya kalan */
    char sarj[24];
    long khz0, khz1;
    double up, yuk;
    long bos_mb, toplam_mb;
    char ssid[64], ip[40], arayuz[16];
    long sinyal;
};


/* qsort karsilastiricisi.  Adi hicbir yerde "kucuk_mu(" seklinde gecmiyor -
   qsort'a islevci olarak veriliyor; olu kod tarayicisi bu yuzden onu olu
   sandi ve silindi, derleme kirildi.  Bir arac yalnizca aradigi kalibi
   goruyor. */
static int kucuk_mu(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static long medyan(long *v, int n)
{
    if (n <= 0) return 0;
    qsort(v, n, sizeof(long), kucuk_mu);
    return v[n / 2];
}

/* nod's ring: fixed 128-byte records, slot 0 is the write index.
   "epoch uptime mV mA capacity temp cpu0 cpu1 load freeram net" */
#define HALKA_KAYIT 128
static int halka_oku(long *mv, long *ma, int tavan)
{
    static const char *yol = "/data/sirsch/gecmis.ring";
    char kayit[HALKA_KAYIT + 1];
    long indis, toplam;
    struct stat st;
    int f = open(yol, O_RDONLY), n = 0, i;

    if (f < 0) return 0;
    if (fstat(f, &st) < 0 || st.st_size < 2 * HALKA_KAYIT) { close(f); return 0; }
    toplam = st.st_size / HALKA_KAYIT - 1;
    if (pread(f, kayit, HALKA_KAYIT, 0) != HALKA_KAYIT) { close(f); return 0; }
    kayit[HALKA_KAYIT] = 0;
    indis = strtol(kayit, NULL, 10);
    if (indis < 0 || indis >= toplam) indis = 0;

    /* Walk backwards from the newest record. */
    for (i = 0; i < tavan; i++) {
        long slot = indis - i;
        long z, u, v, a;
        while (slot < 1) slot += toplam;
        if (pread(f, kayit, HALKA_KAYIT, slot * HALKA_KAYIT) != HALKA_KAYIT) break;
        kayit[HALKA_KAYIT] = 0;
        if (sscanf(kayit, "%ld %ld %ld %ld", &z, &u, &v, &a) != 4) continue;
        if (v < 2500 || v > 4400) continue;          /* uydurma okuma */
        mv[n] = v; ma[n] = a; n++;
    }
    close(f);
    return n;
}

/* Cell capacity, mAh.  1500 is the GT-I8190 nameplate; a fourteen-year-old
   cell holds less, and the real number can be measured later by counting
   charge between two known states.  Until then it is a stated assumption, not
   a hidden one - override in /data/sirsch/pil-mah. */
static long pil_mah(void)
{
    char b[32];
    int f = open("/data/sirsch/pil-mah", O_RDONLY), n;
    long v = 1500;
    if (f < 0) return v;
    n = read(f, b, sizeof(b) - 1);
    close(f);
    if (n > 0) { b[n] = 0; v = strtol(b, NULL, 10); }
    return (v > 100 && v < 6000) ? v : 1500;
}

static long ic_direnc_mohm(void)
{
    char b[32];
    int f = open("/data/sirsch/pil-r-mohm", O_RDONLY), n;
    long r = 200;                                     /* gecici varsayilan */
    if (f < 0) return r;
    n = read(f, b, sizeof(b) - 1);
    close(f);
    if (n > 0) { b[n] = 0; r = strtol(b, NULL, 10); }
    return (r > 0 && r < 2000) ? r : 200;
}

static void wifi_bilgi(struct durum *d)
{
    FILE *f;
    char satir[256];

    d->ssid[0] = 0; d->sinyal = 0;
    /* /proc/net/wireless gives the level without asking the supplicant, so the
       screen keeps working even when nothing else does. */
    f = fopen("/proc/net/wireless", "r");
    if (f) {
        while (fgets(satir, sizeof(satir), f)) {
            char ad[32]; int durum2; float kalite, seviye;
            if (sscanf(satir, " %31[^:]: %d %f %f", ad, &durum2, &kalite, &seviye) == 4) {
                d->sinyal = (long)seviye;
                break;
            }
        }
        fclose(f);
    }
    /* SSID'yi SURUCUYE sor.
     *
     * Ilk surum /data/sirsch/ssid diye bir dosyadan okuyordu ve o dosyayi
     * hicbir sey yazmiyordu: ekran "wifi not connected" gosterirken cihaz
     * o anda ayni wifi uzerinden konusuyordu.  Var olmayan bir kaynaktan
     * okumak, sessizce "yok" demek anlamina geliyor.
     *
     * SIOCGIWESSID fork gerektirmiyor - `iw` ya da `agci` cagirmak her
     * yenilemede bir surec demek olurdu. */
    {
        struct iwreq istek;
        char ssid[IW_ESSID_MAX_SIZE + 1];
        int sk = socket(AF_INET, SOCK_DGRAM, 0);
        d->ssid[0] = 0;
        if (sk >= 0) {
            memset(&istek, 0, sizeof(istek));
            memset(ssid, 0, sizeof(ssid));
            snprintf(istek.ifr_name, IFNAMSIZ, "wlan0");
            istek.u.essid.pointer = ssid;
            istek.u.essid.length  = IW_ESSID_MAX_SIZE;
            istek.u.essid.flags   = 0;
            if (ioctl(sk, SIOCGIWESSID, &istek) == 0 && istek.u.essid.length > 0) {
                ssid[istek.u.essid.length < (int)sizeof(ssid) - 1
                     ? istek.u.essid.length : (int)sizeof(ssid) - 1] = 0;
                snprintf(d->ssid, sizeof(d->ssid), "%s", ssid);
            }
            close(sk);
        }
    }
}

/* Interface address without netlink: SIOCGIFADDR on a throwaway socket.
   Tried in order, because which interface carries the address depends on how
   the node is attached at the time - wifi at school, rndis on the bench. */
static void ip_bul(struct durum *d)
{
    static const char *adaylar[] = { "wlan0", "rndis0", "usb0", "eth0", NULL };
    struct ifconf yap;
    struct ifreq liste[16];
    int s, i, j, n, yedek_var = 0;
    char yedek[40] = "", yedek_ay[16] = "";

    d->ip[0] = 0;
    d->arayuz[0] = 0;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;

    /* SIOCGIFADDR bir arayuzun YALNIZ ILK adresini veriyor.
     *
     * 26 Agustos: wlan0'da once bir link-local (169.254.x) vardi, sonra
     * DHCP ile gercek adres EKLENDI - link-local kacis yolu olarak
     * bilerek birakildi.  Ekran yine 169.254'u gosteriyordu, yani cihaza
     * baglanmak isteyen birine calismayan adresi veriyordu.
     * SIOCGIFCONF butun adresleri listeliyor; link-local yalnizca baska
     * hicbir sey yoksa gosteriliyor. */
    memset(&yap, 0, sizeof(yap));
    yap.ifc_len = sizeof(liste);
    yap.ifc_req = liste;
    if (ioctl(s, SIOCGIFCONF, &yap) == 0) {
        n = yap.ifc_len / (int)sizeof(struct ifreq);
        for (i = 0; adaylar[i] && !d->ip[0]; i++) {
            for (j = 0; j < n; j++) {
                struct sockaddr_in *sa = (struct sockaddr_in *)&liste[j].ifr_addr;
                char b[40];
                if (strcmp(liste[j].ifr_name, adaylar[i])) continue;
                if (sa->sin_family != AF_INET) continue;
                if (!inet_ntop(AF_INET, &sa->sin_addr, b, sizeof(b))) continue;
                if (!strncmp(b, "169.254.", 8) || !strcmp(b, "127.0.0.1")) {
                    if (!yedek_var) {
                        snprintf(yedek, sizeof(yedek), "%s", b);
                        snprintf(yedek_ay, sizeof(yedek_ay), "%s", adaylar[i]);
                        yedek_var = 1;
                    }
                    continue;
                }
                snprintf(d->ip, sizeof(d->ip), "%s", b);
                snprintf(d->arayuz, sizeof(d->arayuz), "%s", adaylar[i]);
                break;
            }
        }
    }
    if (!d->ip[0] && yedek_var) {
        snprintf(d->ip, sizeof(d->ip), "%s", yedek);
        snprintf(d->arayuz, sizeof(d->arayuz), "%s", yedek_ay);
    }
    close(s);
}

static void topla(struct durum *d)
{
    FILE *f;
    char satir[256];
    long v;

    memset(d, 0, sizeof(*d));
    oku_sayi("/sys/class/power_supply/battery/voltage_now", &d->mv);
    d->mv /= 1000;
    oku_sayi("/sys/class/power_supply/battery/current_now", &d->ma);
    oku_sayi("/sys/class/power_supply/battery/capacity", &d->olcer);
    {
        long mv[ORNEK], ma[ORNEK];
        int n = halka_oku(mv, ma, ORNEK);
        long v = d->mv, a = d->ma;
        if (n >= 5) { v = medyan(mv, n); a = medyan(ma, n); }
        /* Discharge (a<0) drags the terminal voltage down, charge (a>0) props
           it up; both are removed by the same sign convention. */
        d->mv_oc = v - (a * ic_direnc_mohm()) / 1000;
        d->yuzde = yuzde_gerilimden((int)d->mv_oc);

        /* Remaining time from the median current, not the instantaneous one:
           the load swings between 200 MHz idle and both cores at 1 GHz, and a
           figure that jumps with every sample answers nothing.  Discharging
           counts down what is left; charging counts up to full. */
        d->kalan_dk = 0;
        if (a < -5)
            d->kalan_dk = (pil_mah() * d->yuzde / 100) * 60 / (-a);
        else if (a > 5)
            d->kalan_dk = -((pil_mah() * (100 - d->yuzde) / 100) * 60 / a);
    }
    oku_metin("/sys/class/power_supply/battery/status", d->sarj, sizeof(d->sarj));
    oku_sayi("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &d->khz0);
    oku_sayi("/sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq", &d->khz1);

    f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &d->up) != 1) d->up = 0; fclose(f); }
    f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf", &d->yuk) != 1) d->yuk = 0; fclose(f); }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        long bos = 0, onbellek = 0;
        while (fgets(satir, sizeof(satir), f)) {
            if (sscanf(satir, "MemTotal: %ld", &v) == 1) d->toplam_mb = v / 1024;
            else if (sscanf(satir, "MemFree: %ld", &v) == 1) bos = v;
            else if (sscanf(satir, "Cached: %ld", &v) == 1) onbellek = v;
        }
        d->bos_mb = (bos + onbellek) / 1024;
        fclose(f);
    }
    wifi_bilgi(d);
    ip_bul(d);
}

/* Panel AMOLED (s6e63m0): siyah piksel yanmayan pikseldir, sifir guc.
 * Zemin RGB(10,14,16) iken ekranin TAMAMI hafifce yaniyordu - 480x800'un
 * her pikseli.  Saf siyah, o alani komple sondurur.
 *
 * Onplan renkleri de kasten kisik: parlaklik zaten dusuruldu ve yuksek
 * kontrast astigmatta hale yapiyor.  Okunurlugu kaybetmeden en az piksel
 * yakan degerler. */
#define ZEMIN   renk(0, 0, 0)
#define AK      renk(190, 196, 195)
#define SOLUK   renk(92, 102, 105)
#define IYI     renk(62, 155, 140)
#define KOTU    renk(190, 96, 74)
#define VURGU_ZEMIN renk(12, 26, 28)

#define SOL     24
#define ETIKET  7   /* etiket 6 sutun + 1 bosluk */                       /* etiket sutunu, karakter */
#define DEGER   (SOL + FONT_G * 2 * ETIKET)
#define SATIR   46
#define IS_SATIR 40

/* Deger sutununa sigmayan yazi KUCULUR, kirpilmaz.
 *
 * 26 Agustos: cihaz "net 192.168.0.2" gosteriyordu.  Adres 192.168.0.26 idi -
 * son hane sutunun disinda kalmisti.  Kirpilmis bir IP yanlis bir IP'dir ve
 * ekranda dogru gorunur; birine o adresi verirsen baglanamaz. */
static void satir_yaz(int y, const char *etiket, const char *deger, unsigned long c)
{
    int alan = fb_g - SOL - DEGER;
    metin_sinir(SOL, y, etiket, 2, SOLUK, DEGER - FONT_G * 2 / 2);
    if ((int)strlen(deger) * FONT_G * 2 > alan)
        metin(DEGER, y + 6, deger, 1, c);
    else
        metin(DEGER, y, deger, 2, c);
}

/* ---------- dokunma ----------
 *
 * /dev/input/event2 dogrudan okunuyor: cerceve yok, InputReader yok.  Android'in
 * InputReader'i bu ekrani zaten "kullanilamaz" ilan ediyordu ("could not query
 * the properties of its associated display") - o sikayet cerceveye aitti,
 * surucuye degil.  Protokol B: MT_SLOT / MT_TRACKING_ID / MT_POSITION_X/Y.
 * Koordinatlar panele birebir; olculdu, X 3..471 ve Y 31..642.
 *
 * Olay parmak KALKINCA uretiliyor, basarken degil: basili tutarken sayfa
 * degistirmek, parmagini surukleyen birine istemedigi seyi sectirir.
 */
static int dokunma_fd = -1;
static int tus_fd = -1;      /* gpio-keys: ses ve home */
static int guc_fd = -1;      /* AB8500 PowerOn Key */

static void dokunma_ac(void)
{
    dokunma_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    tus_fd     = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    guc_fd     = open("/dev/input/event3", O_RDONLY | O_NONBLOCK);
}

/* Donus: 0 zaman doldu, 1 dokunma (koordinatli), 2 tusa basildi. */
/* Kaydirmayi taptan ayiran esik, dokunmatik biriminde.
   Panel 480 genis, dokunmatik 3..471 veriyor - yani birim ~1:1.
   80 piksel: kazara surtunmeyi kaydirma saymayacak kadar buyuk, bir
   basparmak hareketinin rahat astigi kadar kucuk. */
#define KAYDIR_ESIK 80

static int dokunma_bekle(int bekle_ms, int *cx, int *cy)
{
    static int x = -1, y = -1, basili = 0, bx = -1, by = -1;
    struct pollfd pf[3];
    struct input_event olay;
    int kalan = bekle_ms, n = 0, i;
    int idx_dokun = -1, idx_tus = -1, idx_guc = -1;

    if (dokunma_fd >= 0) { pf[n].fd = dokunma_fd; idx_dokun = n++; }
    if (tus_fd     >= 0) { pf[n].fd = tus_fd;     idx_tus   = n++; }
    if (guc_fd     >= 0) { pf[n].fd = guc_fd;     idx_guc   = n++; }
    if (!n) {
        if (bekle_ms > 0) usleep((useconds_t)bekle_ms * 1000);
        return 0;
    }
    while (kalan > 0) {
        int dilim = kalan > 250 ? 250 : kalan;
        for (i = 0; i < n; i++) { pf[i].events = POLLIN; pf[i].revents = 0; }
        if (poll(pf, n, dilim) <= 0) { kalan -= dilim; continue; }

        if (idx_dokun >= 0 && (pf[idx_dokun].revents & POLLIN))
            while (read(dokunma_fd, &olay, sizeof(olay)) == (ssize_t)sizeof(olay)) {
                if (olay.type != EV_ABS) continue;
                /* Yeni bir dokunusun BASLANGICINI kaydet: kaydirma, biten
                   yerle baslayan yerin farki.  Surucu basista TRACKING_ID
                   gondermiyor (yalnizca birakista -1), o yuzden basi
                   "basili degilken gelen ilk koordinat" olarak aliyoruz. */
                if (olay.code == ABS_MT_POSITION_X) {
                    if (!basili) { bx = -1; by = -1; }
                    x = olay.value; basili = 1;
                    if (bx < 0) bx = x;
                } else if (olay.code == ABS_MT_POSITION_Y) {
                    if (!basili) { bx = -1; by = -1; }
                    y = olay.value; basili = 1;
                    if (by < 0) by = y;
                } else if (olay.code == ABS_MT_TRACKING_ID && olay.value < 0) {
                    if (basili && x >= 0 && y >= 0) {
                        int dx = (bx >= 0) ? x - bx : 0;
                        int dy = (by >= 0) ? y - by : 0;
                        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                        *cx = x; *cy = y; basili = 0;
                        /* Dikey hareket yataydan buyukse kaydirma sayma:
                           parmak asagi kayarken sayfa degistirmek, kullanicinin
                           istemedigi seyi yapmanin en hizli yolu. */
                        if (adx >= KAYDIR_ESIK && adx > ady) return dx < 0 ? 3 : 4;
                        return 1;
                    }
                    basili = 0;
                }
            }
        /* Tuslar: hangi tus oldugu simdilik onemli degil, "biri uyandirdi"
           yeter.  Ayrimi sayfa gezinmesine baglamak sonraki is. */
        if (idx_tus >= 0 && (pf[idx_tus].revents & POLLIN)) {
            int bas = 0;
            while (read(tus_fd, &olay, sizeof(olay)) == (ssize_t)sizeof(olay))
                if (olay.type == EV_KEY && olay.value == 1) bas = 1;
            if (bas) return 2;
        }
        if (idx_guc >= 0 && (pf[idx_guc].revents & POLLIN)) {
            int bas = 0;
            while (read(guc_fd, &olay, sizeof(olay)) == (ssize_t)sizeof(olay))
                if (olay.type == EV_KEY && olay.value == 1) bas = 1;
            if (bas) return 2;
        }
        kalan -= dilim;
    }
    return 0;
}

/* ---------- sayfalar ---------- */

/* Baslik seridinin TAMAMI dokunma alani.
   Ilk surumde 96 idi - basligin ust kenarina gore dogruydu ama insan yazinin
   ortasina basiyor ve orasi 100 pikselin altinda kalmiyordu; dokunus sekme
   arama koluna dusup hicbir sey yapmiyordu.  Icerik zaten 128'de basliyor. */
#define BASLIK_Y_SON 122

enum { S_DURUM, S_VERI, S_ISLER, S_AG, S_ADET };
static const char *SAYFA_AD[S_ADET] = { "sys", "data", "jobs", "net" };
static int sayfa = S_DURUM;
int sayfa_secim = -1;

#define SEKME_Y 706

static int sutun_say(const char *t)
{
    const unsigned char *q = (const unsigned char *)t;
    int adim, n = 0;
    while (*q) { utf8(q, &adim); q += adim; n++; }
    return n;
}

static void sekmeler_ciz(void)
{
    int g = fb_g / S_ADET, i;
    kutu(0, SEKME_Y, fb_g, 1, SOLUK);
    for (i = 0; i < S_ADET; i++) {
        int x = i * g;
        int sutun = sutun_say(SAYFA_AD[i]);
        if (i == sayfa) kutu(x + 3, SEKME_Y + 6, g - 6, fb_y - SEKME_Y - 12, VURGU_ZEMIN);
        metin(x + (g - sutun * FONT_G * 2) / 2, SEKME_Y + 28, SAYFA_AD[i], 2,
              i == sayfa ? IYI : SOLUK);
        if (i) kutu(x, SEKME_Y + 10, 1, fb_y - SEKME_Y - 20, SOLUK);
    }
}

static int sekme_bul(int x, int y)
{
    if (y < SEKME_Y) return -1;
    return x * S_ADET / fb_g;
}

/* ---------- ciz ---------- */



/* Ortak baslik.  Sayfa adi BURAYA yazilmaz: sekme zaten soyluyor, ve
   tekrarlamak 46 pikseli hicbir sey icin harciyor. */
static void baslik(void)
{
    char b[32];
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    metin(SOL, 28, "schrod", 3, AK);
    if (lt) {
        /* Duran bir saat, donmus bir ekranin tek gorunur isareti. */
        snprintf(b, sizeof(b), "%02d:%02d", lt->tm_hour, lt->tm_min);
        metin(fb_g - SOL - FONT_G * 2 * 5, 46, b, 2, SOLUK);
    }
    kutu(SOL, 96, fb_g - 2 * SOL, 1, SOLUK);
}

static void sure_yaz(char *b, size_t n, long dk)
{
    if (dk >= 60) snprintf(b, n, "%ldh %ldm", dk / 60, dk % 60);
    else          snprintf(b, n, "%ld min", dk);
}

static void ciz_durum(struct durum *d)
{
    char b[128];
    int y = 128;
    unsigned long c;

    baslik();

    {
        const char *supheli = (d->olcer >= 0 &&
                               (d->yuzde - d->olcer > 12 || d->olcer - d->yuzde > 12))
                              ? "?" : "";
        if (d->ma > 0)      snprintf(b, sizeof(b), "%ld%%%s +%ldmA", d->yuzde, supheli, d->ma);
        else if (d->ma < 0) snprintf(b, sizeof(b), "%ld%%%s %ldmA", d->yuzde, supheli, d->ma);
        else                snprintf(b, sizeof(b), "%ld%%%s full", d->yuzde, supheli);
    }
    c = d->ma > 0 ? IYI : (d->yuzde <= 15 ? KOTU : AK);
    satir_yaz(y, "batt", b, c); y += SATIR;

    if (d->kalan_dk > 0) {
        sure_yaz(b, sizeof(b), d->kalan_dk);
        satir_yaz(y, "left", b, d->kalan_dk < 30 ? KOTU : AK);
    } else if (d->kalan_dk < 0) {
        sure_yaz(b, sizeof(b), -d->kalan_dk);
        satir_yaz(y, "full", b, IYI);
    } else {
        satir_yaz(y, "left", "-", SOLUK);
    }
    y += SATIR;

    {
        long sn = (long)d->up;
        if (sn >= 86400) snprintf(b, sizeof(b), "%ldd %ldh", sn / 86400, (sn % 86400) / 3600);
        else sure_yaz(b, sizeof(b), sn / 60);
    }
    satir_yaz(y, "up", b, AK); y += SATIR;

    snprintf(b, sizeof(b), "%ld MB", d->bos_mb);
    satir_yaz(y, "free", b, d->bos_mb > 200 ? AK : KOTU); y += SATIR;

    if (d->khz0 == d->khz1) snprintf(b, sizeof(b), "%ldMHz %.2f", d->khz0 / 1000, d->yuk);
    else snprintf(b, sizeof(b), "%ld/%ld %.2f", d->khz0 / 1000, d->khz1 / 1000, d->yuk);
    satir_yaz(y, "cpu", b, d->yuk < 1.5 ? AK : KOTU); y += SATIR;

    satir_yaz(y, "net", d->ip[0] ? d->ip : "offline", d->ip[0] ? IYI : KOTU); y += SATIR;

    if (d->ssid[0]) { snprintf(b, sizeof(b), "%.11s", d->ssid); satir_yaz(y, "ssid", b, SOLUK); }
}

/* Kucuk cizgi grafik.  Bir sayi dizisinin sekli, ayni sayilari alt alta
   yazmaktan cok daha hizli okunuyor - "haftalik veriye bakabilir miyim"
   sorusunun cevabi bu. */
static void kivilcim(int x, int y, int g, int h, double *v, int n, unsigned long c)
{
    double en_az, en_cok;
    int i, onceki_x = 0, onceki_y = 0;

    if (n < 2) return;
    en_az = en_cok = v[0];
    for (i = 1; i < n; i++) { if (v[i] < en_az) en_az = v[i]; if (v[i] > en_cok) en_cok = v[i]; }
    if (en_cok - en_az < 1e-9) { en_cok = en_az + 1.0; }

    kutu(x, y + h, g, 1, SOLUK);
    for (i = 0; i < n; i++) {
        int px = x + (n == 1 ? 0 : i * (g - 1) / (n - 1));
        int py = y + h - (int)((v[i] - en_az) * h / (en_cok - en_az));
        if (i) {
            /* Duz cizgi: iki nokta arasini piksel piksel doldur. */
            int dx = px - onceki_x, dy = py - onceki_y, adim, k;
            adim = (dx > 0 ? dx : -dx) > (dy > 0 ? dy : -dy)
                 ? (dx > 0 ? dx : -dx) : (dy > 0 ? dy : -dy);
            if (adim < 1) adim = 1;
            for (k = 0; k <= adim; k++)
                kutu(onceki_x + dx * k / adim, onceki_y + dy * k / adim, 2, 2, c);
        }
        onceki_x = px; onceki_y = py;
    }
}

/* data: butun kaynaklar TEK tabloda.
 *
 * Ilk surum dosya secmeye zorluyordu ve bu yanlisti - panele bakan biri
 * "hangi dosya" diye dusunmez, ne oldugunu gormek ister.  Her kaynak kendi
 * blogunu aliyor, en yenisi ustte. */
/* ---------- data: bir kaynak, bir ekran ----------
 *
 * Ilk surum butun .pnl dosyalarini alt alta diziyordu.  Uc kaynak baglaninca
 * hicbiri okunur olmadi: 480x800'de sekiz kaynagin sigacagi yer yok, ve
 * ustteki kaynagin serisi alttakinin degerlerini disari itiyordu.
 *
 * Bir panelde ayni anda BIR sey okunur.  Icerige dokunmak sonraki kaynaga
 * gecirir; kac kaynak oldugu sag ustte yaziyor.
 */
#define VERI_TAVAN 8
static char veri_ad[VERI_TAVAN][64];
static int  veri_adet = 0;
static int  veri_secim = 0;

static void veri_tara(void)
{
    DIR *dz = opendir(PANEL_DIZIN);
    struct dirent *e;
    int i, j;
    veri_adet = 0;
    if (!dz) return;
    while ((e = readdir(dz)) && veri_adet < VERI_TAVAN) {
        size_t n = strlen(e->d_name);
        if (n < 5 || n >= sizeof(veri_ad[0]) || strcmp(e->d_name + n - 4, ".pnl")) continue;
        snprintf(veri_ad[veri_adet++], sizeof(veri_ad[0]), "%s", e->d_name);
    }
    closedir(dz);
    /* Dosya adindaki sira numarasi sayfa sirasini belirliyor; readdir sirasi
       rastgele, o yuzden burada siralaniyor - yoksa dokundukca kaynaklar
       yer degistirir. */
    for (i = 1; i < veri_adet; i++)
        for (j = i; j > 0 && strcmp(veri_ad[j - 1], veri_ad[j]) > 0; j--) {
            char t[64];
            memcpy(t, veri_ad[j - 1], sizeof(t));
            memcpy(veri_ad[j - 1], veri_ad[j], sizeof(t));
            memcpy(veri_ad[j], t, sizeof(t));
        }
    if (veri_secim >= veri_adet) veri_secim = veri_adet ? veri_adet - 1 : 0;
}

/* Yasi insanin okudugu bicimde: "12s" degil "12s ago" degil - sadece sayi ve
   birim.  Ekranda tek fazla kelime yok. */
static void yas_yaz(char *b, size_t n, long sn)
{
    if (sn < 90)         snprintf(b, n, "%lds", sn);
    else if (sn < 5400)  snprintf(b, n, "%ldm", sn / 60);
    else                 snprintf(b, n, "%ldh", sn / 3600);
}

static void ciz_veri(void)
{
    char yol[160], satir[512];
    FILE *f;
    double seri[120];
    int seri_n = 0, y = 128, deger_n = 0;
    long tazelik = -1;
    char metinler[3][64];
    int metin_n = 0;
    char bas_metin[64] = "";
    char seri_etiket[48] = "";

    baslik();
    veri_tara();
    if (!veri_adet) {
        metin(SOL, y, "no source", 3, SOLUK); y += SATIR + 14;
        metin(SOL, y, "a source is any program that", 1, SOLUK); y += 26;
        metin(SOL, y, "drops a .pnl file into", 1, SOLUK); y += 26;
        metin(SOL, y, PANEL_DIZIN, 1, SOLUK); y += 34;
        metin(SOL, y, "network, serial or sensor -", 1, SOLUK); y += 26;
        metin(SOL, y, "the screen does not care", 1, SOLUK);
        return;
    }

    snprintf(yol, sizeof(yol), "%s/%s", PANEL_DIZIN, veri_ad[veri_secim]);
    f = fopen(yol, "r");
    if (!f) { metin(SOL, y, "unreadable", 2, KOTU); return; }

    /* Kac kaynak var, hangisindeyiz - ve iki yone de gidilebilecegi.
       Oklar yazi degil, DAVET: kaydirmayi bilmeyen birinin de gidebilmesi
       icin sol/sag yariya dokunmak da calisiyor. */
    if (veri_adet > 1) {
        char sayac[24];
        /* Sayac BASLIK SATIRINDA DEGIL, altindaki tazelik satirinda.
           Baslik satirina koyunca "greenhouse" gibi on harflik bir ad ile
           carpisiyordu; asagi alinca baslik butun genisligi kullanabiliyor
           ve sayac zaten bos duran sagda oturuyor. */
        snprintf(sayac, sizeof(sayac), "< %d/%d >", veri_secim + 1, veri_adet);
        metin(fb_g - SOL - (int)strlen(sayac) * FONT_G,
              y + FONT_Y * 3 + 10 + 12, sayac, 1, SOLUK);
    }

    while (fgets(satir, sizeof(satir), f)) {
        char tip[24], ad[64], deg[64], birim[24], du[24];
        int n;
        satir[strcspn(satir, "\n")] = 0;
        if (!satir[0] || satir[0] == '#') continue;
        birim[0] = du[0] = 0;
        n = sscanf(satir, "%23s %63s %63s %23s %23s", tip, ad, deg, birim, du);
        if (n < 1) continue;

        if (!strcmp(tip, "baslik") || !strcmp(tip, "title")) {
            snprintf(bas_metin, sizeof(bas_metin), "%s", satir + strlen(tip) + 1);
        } else if (!strcmp(tip, "tazelik") || !strcmp(tip, "freshness")) {
            if (n >= 2) tazelik = atol(ad);
        } else if ((!strcmp(tip, "deger") || !strcmp(tip, "value")) && n >= 3) {
            if (!deger_n) {
                /* Basligi ve yasi ancak ilk degerden once yaz: dosyada
                   hangi sirada olduklari onemsiz olsun.
                   3 kat yazi FONT_Y*3 yuksek; SATIR (46) kadar ilerlemek
                   yasi basligin kuyruguna bindiriyordu. */
                /* Sag ustteki kaynak sayacina bindirme: "greenhouse2/4"
                   diye tek kelime gorunuyordu. */
                metin_sinir(SOL, y, bas_metin[0] ? bas_metin : veri_ad[veri_secim],
                            3, AK, fb_g - SOL);
                y += FONT_Y * 3 + 10;
                if (tazelik >= 0) {
                    char yb[32];
                    yas_yaz(yb, sizeof(yb), tazelik);
                    /* 10 dakikadan eski veri canli degildir; ekranda oyle
                       gorunmesin. */
                    metin(SOL, y, yb, 2, tazelik > 600 ? KOTU : SOLUK);
                    y += FONT_Y * 2 + 14;
                } else {
                    y += 10;
                }
            }
            if (y + SATIR < SEKME_Y - 90) {
                char sag[96];
                unsigned long c = (!strcmp(du, "hata") || !strcmp(du, "bad")) ? KOTU
                                : (!strcmp(du, "dikkat") || !strcmp(du, "warn")) ? KOTU : AK;
                if (birim[0]) snprintf(sag, sizeof(sag), "%s %s", deg, birim);
                else          snprintf(sag, sizeof(sag), "%s", deg);
                satir_yaz(y, ad, sag, c);
                y += SATIR - 6;
                deger_n++;
            }
        } else if (!strcmp(tip, "seri") || !strcmp(tip, "series")) {
            char *q = satir + strlen(tip) + 1;
            char *tok = strtok(q, " \t");
            seri_n = 0;
            if (tok) tok = strtok(NULL, " \t");     /* ilk alan seri adi */
            while (tok && seri_n < 120) { seri[seri_n++] = atof(tok); tok = strtok(NULL, " \t"); }
        } else if (!strcmp(tip, "seri-etiket") || !strcmp(tip, "series-label")) {
            snprintf(seri_etiket, sizeof(seri_etiket), "%s", satir + strlen(tip) + 1);
        } else if (!strcmp(tip, "metin") || !strcmp(tip, "text")) {
            if (metin_n < 3)
                snprintf(metinler[metin_n++], sizeof(metinler[0]), "%s", satir + strlen(tip) + 1);
        }
    }
    fclose(f);

    if (!deger_n) {
        metin_sinir(SOL, y, bas_metin[0] ? bas_metin : veri_ad[veri_secim],
                    3, AK, fb_g - SOL);
        y += FONT_Y * 3 + 10;
    }

    if (seri_n >= 2) {
        double en_az = seri[0], en_cok = seri[0];
        int i, alt, yuk;
        char b[32];
        for (i = 1; i < seri_n; i++) {
            if (seri[i] < en_az) en_az = seri[i];
            if (seri[i] > en_cok) en_cok = seri[i];
        }
        /* Grafik kalan yerin TAMAMINI alir.  Sabit 72 piksel birakinca
           ekranin alt yarisi bos duruyordu; bu panelde bos piksel, gorulmeyen
           veri demek. */
        alt = SEKME_Y - 16 - metin_n * 26 - (seri_etiket[0] ? 26 : 0);
        y += 10;
        /* En cok / en az etiketleri grafigin ICINE yazilinca cizgiyi
           kesiyordu; kendi satirlarinda duruyorlar. */
        snprintf(b, sizeof(b), "%.4g", en_cok);
        metin(SOL, y, b, 1, SOLUK);
        y += FONT_Y + 4;
        yuk = alt - y - FONT_Y - 6;
        if (yuk > 40) {
            kivilcim(SOL, y, fb_g - 2 * SOL, yuk, seri, seri_n, IYI);
            y += yuk + 4;
            snprintf(b, sizeof(b), "%.4g", en_az);
            metin(SOL, y, b, 1, SOLUK);
            snprintf(b, sizeof(b), "%d", seri_n);
            metin(fb_g - SOL - (int)strlen(b) * FONT_G, y, b, 1, SOLUK);
            y += FONT_Y + 6;
        }
        if (seri_etiket[0]) { metin(SOL, y, seri_etiket, 1, SOLUK); y += 26; }
    }

    {
        int i;
        for (i = 0; i < metin_n && y + FONT_Y < SEKME_Y - 8; i++) {
            metin(SOL, y, metinler[i], 1, SOLUK);
            y += 26;
        }
    }
}

/* ---------- jobs: cihazin kendi kendine yaptigi isler ----------
 *
 * Bu sayfa bir gunluk degil bir KUMANDA: bir ise dokunmak onu simdi
 * calistirir.  Zamanlayici (sirsch-isler.sh) her turda /data/sirsch/is-simdi
 * dosyasina bakiyor; ekran oraya adi yaziyor ve isin nasil kostugunu hic
 * bilmiyor.
 */
#define IS_BAS_Y 128
#define IS_TAVAN 10
/* Her is iki satir: ad/durum/saat, altinda ne yaptigi.
   "ok" tek basina bir sey soylemiyordu. */
#define IS_YUK  (IS_SATIR + 26)
static char is_ad[IS_TAVAN][64];
static int  is_adet = 0;
/* Dokunulan is ve ne zaman dokunuldugu.
   26 Agustos: Alperen bir ise bastı ve "hicbir sey olmadi" dedi - hakliydi.
   Is gercekten kosuyordu ama zamanlayici 30 saniyede bir bakiyordu ve ekranda
   dokunusun islendigini soyleyen HICBIR SEY yoktu.  Cevapsiz bir dugme, bozuk
   bir dugmeden ayirt edilemez. */
static int    is_secim = -1;
static time_t is_zaman = 0;

static void ciz_isler(void)
{
    FILE *f = fopen(ISLER, "r");
    int y = IS_BAS_Y, n = 0;
    int isaret_x = SOL;
    int ad_x     = SOL + FONT_G * 2 + 10;
    int saat_x   = fb_g - SOL - FONT_G * 2 * 5;
    int kalan_x  = fb_g - SOL - FONT_G * 2 * 5 - FONT_G * 6 - 12;

    baslik();
    is_adet = 0;
    if (!f) {
        metin(SOL, y, "no scheduler", 3, SOLUK); y += SATIR + 10;
        metin(SOL, y, "sirsch-isler.sh is not running", 1, SOLUK);
        return;
    }
    {
        char satir[256];
        while (fgets(satir, sizeof(satir), f) &&
               y + IS_YUK < SEKME_Y - 8 && is_adet < IS_TAVAN) {
            char ad[64] = "", du[32] = "", sa[32] = "", kal[32] = "", ms[32] = "";
            char ozet[64] = "";
            long kalan;
            satir[strcspn(satir, "\n")] = 0;
            if (sscanf(satir, "%63[^\t]\t%31[^\t]\t%31[^\t]\t%31[^\t]\t%31[^\t]\t%63[^\n]",
                       ad, du, sa, kal, ms, ozet) < 3) continue;
            {
                int iyi = strcmp(du, "ok") == 0;
                int hic = strcmp(du, "-") == 0;
                int kosuyor = (is_adet == is_secim && time(NULL) - is_zaman < 8);
                if (kosuyor)
                    metin(isaret_x, y, ">", 2, IYI);
                else
                    metin(isaret_x, y, hic ? "." : (iyi ? "+" : "!"), 2,
                          hic ? SOLUK : (iyi ? IYI : KOTU));
            }
            metin_sinir(ad_x, y, ad, 2, AK, kalan_x - 8);
            metin(saat_x, y, sa, 2, SOLUK);
            kalan = atol(kal);
            {
                char kb[24];
                int kx;
                if (kalan >= 0) yas_yaz(kb, sizeof(kb), kalan);
                else            snprintf(kb, sizeof(kb), "manual");
                /* Saga hizala: degisken uzunlukta bir alan sola hizali
                   olunca sutun her turda kayiyor ve goz takip edemiyor. */
                kx = saat_x - 12 - (int)strlen(kb) * FONT_G;
                metin(kx, y + 8, kb, 1, SOLUK);
            }
            snprintf(is_ad[is_adet++], sizeof(is_ad[0]), "%s", ad);
            /* Ozet, adin ALTINDA.  Ilk denemede 12 piksel yukaridaydi ve
               iki kat cizilen adin kuyruguna biniyordu. */
            y += IS_SATIR - 4;
            if (ozet[0]) metin_sinir(ad_x, y, ozet, 1, SOLUK, fb_g - SOL);
            y += 30; n++;
        }
    }
    fclose(f);
    if (!n) { metin(SOL, y, "no jobs", 3, SOLUK); return; }
    y += 12;
    if (is_secim >= 0 && time(NULL) - is_zaman < 8) {
        char b[96];
        snprintf(b, sizeof(b), "running %s...", is_ad[is_secim]);
        metin(SOL, y, b, 1, IYI);
    } else {
        metin(SOL, y, "tap a job to run it now", 1, SOLUK);
    }
}

/* Bir ise dokunma: adi dosyaya yaz, zamanlayici alsin. */
static int is_dokunma(int x, int y)
{
    int i;
    (void)x;
    for (i = 0; i < is_adet; i++) {
        int ust = IS_BAS_Y + i * IS_YUK - 6;
        if (y >= ust && y < ust + IS_YUK) {
            pid_t p;
            int f = open("/data/sirsch/is-simdi", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (f >= 0) { if (write(f, is_ad[i], strlen(is_ad[i])) < 0) {} close(f); }
            is_secim = i;
            is_zaman = time(NULL);
            /* Zamanlayicinin sirasini bekleme: bir tur SIMDI kostur.
               Aksi halde dokunusla is arasinda 30 saniyeye kadar bosluk
               oluyor ve dugme calismiyor gibi duruyor. */
            while (waitpid(-1, NULL, WNOHANG) > 0) { }
            p = fork();
            if (p == 0) {
                execl("/system/bin/sh", "sh", "/system/bin/sirsch-isler.sh",
                      "tur", (char *)NULL);
                _exit(127);
            }
            return 1;
        }
    }
    return 0;
}

/* Tarama YALNIZCA net sayfasi acikken.
 *
 * Surekli taramak bir panelin yapacagi son sey: her tarama radyoyu kanal
 * kanal gezdirir, mevcut baglantiyi kesintiye ugratir ve pil yer.  Kullanici
 * o sayfaya bakmiyorsa taranacak bir sey de yoktur.
 *
 * Cocuk surecte: tarama bes saniye suruyor ve cizim dongusu o sure boyunca
 * dokunmaya cevap veremezdi.
 */
static void tarama_baslat(void)
{
    pid_t p;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }   /* zombi biriktirme */
    p = fork();
    if (p == 0) {
        execl("/system/bin/sh", "sh", "/system/bin/sirsch-ag-tara.sh", (char *)NULL);
        _exit(127);
    }
}

/* rfkill'in state=1'i "acik" degil "ENGELLENMEMIS" demek: radyoya izin var,
   calisiyor demek degil.  Gercek olcut bir HCI aygitinin var olmasi.
   26 Agustos'ta ekran "bt on" yaziyordu, oysa /sys/class/bluetooth bostu ve
   hicbir bluetooth yigini calismiyordu. */
static int bt_acik(void)
{
    DIR *d = opendir("/sys/class/bluetooth");
    struct dirent *e;
    int var = 0;
    if (!d) return 0;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') { var = 1; break; }
    closedir(d);
    return var;
}

static int arayuz_var(const char *ad)
{
    char yol[128], b[16];
    int f, n;
    snprintf(yol, sizeof(yol), "/sys/class/net/%s/operstate", ad);
    f = open(yol, O_RDONLY);
    if (f < 0) return 0;
    n = read(f, b, sizeof(b) - 1);
    close(f);
    if (n <= 0) return 0;
    b[n] = 0;
    return strncmp(b, "up", 2) == 0;
}

/* net: baglanabilecegin seyler, ve onlara DOKUNULUR.
 *
 * Kendiliginden tarama yok.  Her tarama radyoyu kanal kanal gezdiriyor,
 * mevcut baglantiyi kesintiye ugratiyor ve pil yiyor; kullanici istemeden
 * bunu yapmanin gerekcesi yok.  wifi satirina dokunursan tarar, listeyi
 * altta gosterir.
 */
#define AG_WIFI_Y   128
#define AG_USB_Y    (AG_WIFI_Y + SATIR + 24)
#define AG_BT_Y     (AG_USB_Y + SATIR)
#define AG_LISTE_Y  (AG_BT_Y + SATIR + 22)
#define AG_SATIR    38
#define AG_TAVAN    12

static int ag_taraniyor = 0;
/* Altta hangi liste duruyor: 0 hicbiri, 1 wifi, 2 bluetooth.
   26 Agustos: bt'ye basildiginda altta WIFI listesi duruyordu, cunku cizim
   kodu tarama isteninceye kadar da ayni dosyayi okuyordu.  Ustelik sayfaya
   her girildiginde eski tarama sonucu gorunuyordu - kimse istememisken.
   Liste artik yalnizca DOKUNULAN seyin sonucunu gosteriyor. */
enum { LISTE_YOK, LISTE_WIFI, LISTE_BT };
static int liste_tipi = LISTE_YOK;
/* Cizerken hangi satirda hangi SSID oldugunu kaydet: dokunma geldiginde
   dosyayi yeniden ayristirmak, listenin bu arada degismis olma ihtimalini
   getirir ve kullanici bastigindan baskasina baglanir. */
static char ag_liste[AG_TAVAN][64];
static int  ag_liste_n = 0;
static int  ag_liste_bas = AG_LISTE_Y;

static void ciz_ag(struct durum *d)
{
    char b[128];
    int y, bt;

    baslik();

    y = AG_WIFI_Y;
    if (d->ssid[0]) {
        snprintf(b, sizeof(b), "%.11s", d->ssid);
        satir_yaz(y, "wifi", b, IYI);
        if (d->sinyal) {
            snprintf(b, sizeof(b), "%s  %lddBm", d->ip[0] ? d->ip : "no ip", d->sinyal);
            metin(DEGER, y + 30, b, 1, d->ip[0] ? SOLUK : KOTU);
        }
    } else {
        satir_yaz(y, "wifi", "not connected", KOTU);
    }
    /* Dokunulabilir oldugunu soyleyen tek isaret; yazi degil, cizgi. */
    kutu(SOL, AG_WIFI_Y + SATIR + 8, fb_g - 2 * SOL, 1, SOLUK);

    satir_yaz(AG_USB_Y, "usb", arayuz_var("rndis0") ? "192.168.42.2" : "off",
              arayuz_var("rndis0") ? IYI : SOLUK);

    bt = bt_acik();
    satir_yaz(AG_BT_Y, "bt", bt ? "on" : "off", bt ? IYI : SOLUK);

    y = AG_LISTE_Y;
    ag_liste_bas = y;
    ag_liste_n = 0;
    kutu(SOL, y - 14, fb_g - 2 * SOL, 1, SOLUK);
    if (ag_taraniyor) {
        metin(SOL, y, "scanning...", 2, SOLUK);
        return;
    }
    if (liste_tipi == LISTE_YOK) {
        /* Radyoyu kendiliginden gezdirmiyoruz; istenmeden yapilacak en pahali
           sey bu.  Ne yapilacagini soyle, sonra bekle. */
        metin(SOL, y, "tap wifi or bt", 2, SOLUK);
        return;
    }
    {
        FILE *f = fopen(liste_tipi == LISTE_BT ? "/data/sirsch/bt-taranan.txt"
                                               : "/data/sirsch/aglar-taranan.txt", "r");
        char satir[256];
        int n = 0;
        char sonuc[128] = "";
        {   /* Son baglanma denemesinin sonucu, varsa */
            int sf = open("/data/sirsch/ag-sonuc", O_RDONLY), sn;
            if (sf >= 0) {
                sn = read(sf, sonuc, sizeof(sonuc) - 1);
                close(sf);
                if (sn > 0) { sonuc[sn] = 0; sonuc[strcspn(sonuc, "\n")] = 0; }
            }
        }
        if (sonuc[0]) {
            metin_sinir(SOL, y, sonuc, 2,
                        strncmp(sonuc, "bagli", 5) == 0 ? IYI : SOLUK, fb_g - SOL);
            y += SATIR - 6;
        }
        ag_liste_bas = y;
        if (!f) {
            metin(SOL, y, liste_tipi == LISTE_BT ? "no bt result" : "no wifi result",
                  2, SOLUK);
            return;
        }
        while (fgets(satir, sizeof(satir), f) && y + FONT_Y * 2 < SEKME_Y - 8) {
            char *t1, *t2;
            long sinyal;
            satir[strcspn(satir, "\n")] = 0;
            t1 = strchr(satir, '\t');
            if (!t1) continue;
            *t1++ = 0;
            t2 = strchr(t1, '\t');
            if (t2) *t2++ = 0;
            sinyal = strtol(satir, NULL, 10);
            {
                char sag[24];
                unsigned long c = sinyal > -70 ? IYI : (sinyal > -82 ? AK : SOLUK);
                snprintf(sag, sizeof(sag), "%ld", sinyal);
                metin_sinir(SOL, y, t1, 2, c, fb_g - SOL - FONT_G * 2 * 5);
                metin(fb_g - SOL - FONT_G * 2 * 4, y, sag, 2, SOLUK);
            }
            if (ag_liste_n < AG_TAVAN)
                snprintf(ag_liste[ag_liste_n++], sizeof(ag_liste[0]), "%s", t1);
            y += AG_SATIR; n++;
        }
        fclose(f);
        if (!n) metin(SOL, y, "nothing found", 2, SOLUK);
    }
}

/* Sayfa degistiginde liste temizlenir: net sayfasina tekrar girildiginde
   eski tarama sonucunun canliymis gibi durmamasi icin. */
static void net_sifirla(void)
{
    liste_tipi = LISTE_YOK;
    ag_liste_n = 0;
}

/* net sayfasindaki dokunmayi isle.  1 donerse hemen yeniden ciz. */
static void bagla_baslat(const char *ssid)
{
    pid_t p;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
    p = fork();
    if (p == 0) {
        execl("/system/bin/sh", "sh", "/system/bin/sirsch-ag-bagla.sh", ssid, (char *)NULL);
        _exit(127);
    }
}

static void bt_tara_baslat(void)
{
    pid_t p;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
    p = fork();
    if (p == 0) {
        execl("/system/bin/sh", "sh", "/system/bin/sirsch-bt.sh", "tara", (char *)NULL);
        _exit(127);
    }
}

static int ag_dokunma(int x, int y)
{
    int i;
    (void)x;
    if (y >= AG_WIFI_Y - 20 && y < AG_WIFI_Y + SATIR + 8) {
        ag_taraniyor = 1;
        liste_tipi = LISTE_WIFI;
        tarama_baslat();
        return 1;
    }
    if (y >= AG_BT_Y - 16 && y < AG_BT_Y + SATIR - 8) {
        ag_taraniyor = 1;
        liste_tipi = LISTE_BT;
        bt_tara_baslat();
        return 1;
    }
    /* Listedeki bir satira dokunma: hangi satira dustugu, cizerken kaydedilen
       konumdan hesaplaniyor.  Baglanmak yalnizca WIFI listesinde anlamli -
       bt satirina dokunup bir SSID'ye baglanmaya calismak, listeler
       karistiginda olacak seyin ta kendisiydi. */
    if (liste_tipi != LISTE_WIFI) return 0;
    for (i = 0; i < ag_liste_n; i++) {
        int ust = ag_liste_bas + i * AG_SATIR - 6;
        if (y >= ust && y < ust + AG_SATIR) {
            bagla_baslat(ag_liste[i]);
            return 1;
        }
    }
    return 0;
}

static void ciz(struct durum *d)
{
    kutu(0, 0, fb_g, fb_y, ZEMIN);
    switch (sayfa) {
    case S_VERI:  ciz_veri();   break;
    case S_ISLER: ciz_isler();  break;
    case S_AG:    ciz_ag(d);    break;
    default:      ciz_durum(d); break;
    }
    sekmeler_ciz();
    fb_bas();
}

/* ---------- arka isik ---------- */

/* Olculdu 26 Agustos: panel acikken -278 mA, kapaliyken -172 mA.
 * Ekran tek basina 106 mA yiyor - bu cihazda pil omrunun yarisindan fazlasi.
 * Bir panel surekli acik durmaz: dokununca ya da tusa basinca uyanir, sonra
 * kendini kapatir.  "Acma tusuna bastigimda bu ekrani goreyim" tam olarak bu.
 */
/* 0 ile basla: 1 olsaydi ilk isik_ac() hemen doner ve ayarlanan
   parlaklik hic yazilmazdi - 26 Agu: 90 istendi, panel 160'ta kaldi. */
static int isik_acik = 0;

/* Dokunmadan sonra ekranin acik kalacagi sure, saniye.
   /data/sirsch/ekran-suresi ile degistirilir; 0 = hic kapanma. */
static int ISIK_SURESI = 45;

/* Arka isik seviyesi (0-255).  Varsayilan kasten dusuk: 160 astigmatta hale
   yapiyor ve bu panelde parlaklik dogrudan akim demek.
   /data/sirsch/parlaklik ile degistirilir. */
static int PARLAKLIK = 90;
static char parlaklik_metin[8] = "90\n";

static void isik_yaz(const char *deger, const char *guc)
{
    static const char *yollar[] = {
        "/sys/class/backlight/panel/brightness",
        "/sys/class/leds/lcd-backlight/brightness",
        NULL
    };
    DIR *d;
    struct dirent *e;
    char yol[320];
    int i, f;

    for (i = 0; yollar[i]; i++) {
        f = open(yollar[i], O_WRONLY);
        if (f >= 0) { if (write(f, deger, strlen(deger)) < 0) {} close(f); }
    }
    d = opendir("/sys/class/backlight");
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(yol, sizeof(yol), "/sys/class/backlight/%s/brightness", e->d_name);
        f = open(yol, O_WRONLY);
        if (f >= 0) { if (write(f, deger, strlen(deger)) < 0) {} close(f); }
        snprintf(yol, sizeof(yol), "/sys/class/backlight/%s/bl_power", e->d_name);
        f = open(yol, O_WRONLY);
        if (f >= 0) { if (write(f, guc, strlen(guc)) < 0) {} close(f); }
    }
    closedir(d);
}

/* wake_lock YALNIZCA ekran acikken.
 *
 * 26 Agustos, bilerek kaydedilen bir gerileme: dokunmatigi ayakta tutmak icin
 * kilit kalici tutuluyordu, ve kilit sistemin bosta dusuk guc durumlarina
 * girmesini engelliyor.  Sonuc: sabah cerceve kapaliyken +143 mA sarj alan
 * cihaz aksam -256 mA'de bosaliyordu ve kapandi.  Dokunmatik ugruna sarj
 * olabilmek feda edilmisti.
 *
 * Kural: ekran acikken kilit tut (dokunmatik lazim), kapaninca birak.  Guc
 * tusu uyandiriyor ve late_resume dokunmatigi geri getiriyor - o dongunun
 * calistigi olculdu.
 *
 * Tek istisna: sarj oluyorsa kilit birakilmiyor.  Prizdeyken amac dusuk guc
 * degil, ULASILABILIR olmak - uyuyan bir dugum ne is kosar ne cevap verir.
 */
static void kilit_yaz(const char *yol, const char *ad)
{
    int f = open(yol, O_WRONLY);
    if (f < 0) return;
    if (write(f, ad, strlen(ad)) < 0) { }
    close(f);
}

static int sarj_oluyor(void)
{
    char b[16];
    int f = open("/sys/class/power_supply/battery/current_now", O_RDONLY), n;
    if (f < 0) return 1;              /* bilinmiyorsa guvenli taraf: kilidi tut */
    n = read(f, b, sizeof(b) - 1);
    close(f);
    if (n <= 0) return 1;
    b[n] = 0;
    return atol(b) > 0;
}

static void kilit_al(void)
{
    kilit_yaz("/sys/power/wake_lock", "ekran\n");
}

static void kilit_birak(void)
{
    if (sarj_oluyor()) return;       /* prizde: ulasilabilir kal */
    kilit_yaz("/sys/power/wake_unlock", "ekran\n");
}

static void isik_kapat(void)
{
    if (!isik_acik) return;
    isik_yaz("0\n", "1\n");
    isik_acik = 0;
    kilit_birak();
}

static void isik_ac(void)
{
    if (isik_acik) return;
    kilit_al();
    if (fbd >= 0) ioctl(fbd, FBIOBLANK, FB_BLANK_UNBLANK);
    isik_yaz(parlaklik_metin, "0\n");
    isik_acik = 1;
}

/* Android saat dilimini kendi bicim inde tutuyor ve /etc/localtime yok, o
   yuzden statik bir glibc ikilisi "Europe/Istanbul"u cozemiyor ve localtime()
   UTC'ye dusuyor: duvar saati 16:20 iken ekran 13:04 gosteriyordu.  POSIX TZ
   dizgisi hicbir dosya istemez; Turkiye 2016'dan beri sabit +03 ve yaz saati
   yok, dolayisiyla bu yaklasik degil kesin.  Ulke disina cikarsa
   /data/sirsch/tz ile degistirilir. */
static void saat_dilimi(void)
{
    char tz[64];
    int f = open("/data/sirsch/tz", O_RDONLY);
    int n = -1;
    if (f >= 0) { n = read(f, tz, sizeof(tz) - 1); close(f); }
    if (n > 0) {
        tz[n] = 0;
        while (n > 0 && (tz[n-1] == '\n' || tz[n-1] == ' ')) tz[--n] = 0;
    } else {
        snprintf(tz, sizeof(tz), "<+03>-3");
    }
    setenv("TZ", tz, 1);
    tzset();
}

/* Tek ornek garantisi PROGRAMIN KENDISINDE.
 *
 * Bunu cagirana birakmak yetmiyor: 26 Agustos'ta uc ornek ayni anda
 * calisiyordu - biri elle baslatilmis, digerleri kalkan dongusu "ekran yok"
 * sanip baslatmis.  Ucu birden framebuffer'a yazip farkli tamponlara geciyor,
 * ve sonuc ekranda titreme olarak gorunuyor.  Kilit dosyasi, kim baslatirsa
 * baslatsin ikinciyi sessizce reddediyor.
 */
static int tek_ornek(void)
{
    static const char *yol = "/data/sirsch/ekran.kilit";
    int f = open(yol, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (f < 0) return 1;                 /* kilit kurulamadi: engelleme */
    /* O_CLOEXEC SART.
     *
     * 27 Agustos: panel oldu ve bir daha acilmadi.  Sebep: `ekran` ag/bt
     * taramasi ve is kosturmak icin fork+exec yapiyor, ve cocuk ACIK
     * TANITICILARI devraliyor - bu kilit dahil.  bt taramasi cekirdekte
     * `sysfs_addrm_finish` uzerinde kesintisiz (D) duruma dustu, SIGKILL bile
     * onu almadi, ve o surec kilidi tutmaya devam ettigi surece `ekran`
     * "zaten calisiyor" deyip hic baslamadi.
     *
     * Yani panelin kendi kilidi, panelin catalladigi herhangi bir cocuk
     * takildiginda paneli KALICI olarak disari kilitliyordu.  CLOEXEC bu
     * zinciri kokunden kesiyor: cocuk tanitici gormuyor.
     */
    if (flock(f, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "ekran zaten calisiyor\n");
        close(f);
        return 0;
    }
    /* fd bilerek acik birakiliyor: kilit surec olunce kendiliginden dusuyor. */
    return 1;
}

int main(int argc, char **argv)
{
    saat_dilimi();
    {
        char b[16];
        int f = open("/data/sirsch/ekran-suresi", O_RDONLY), n;
        if (f >= 0) {
            n = read(f, b, sizeof(b) - 1);
            close(f);
            if (n > 0) { b[n] = 0; ISIK_SURESI = atoi(b); }
        }
        f = open("/data/sirsch/parlaklik", O_RDONLY);
        if (f >= 0) {
            n = read(f, b, sizeof(b) - 1);
            close(f);
            if (n > 0) {
                b[n] = 0;
                n = atoi(b);
                if (n >= 5 && n <= 255) PARLAKLIK = n;
            }
        }
        snprintf(parlaklik_metin, sizeof(parlaklik_metin), "%d\n", PARLAKLIK);
    }
    const char *aygit = NULL, *ppm = NULL;
    int bekle = 5, tek = 0, demo = 0, i;
    struct durum d;
    time_t son_temas = time(NULL);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) aygit = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) bekle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-1")) tek = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "-j") && i + 1 < argc) ISLER = argv[++i];
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) PANEL_DIZIN = argv[++i];
        else if (!strcmp(argv[i], "-t")) demo = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) sayfa_secim = atoi(argv[++i]);
        /* -k: data sayfasinda kacinci kaynak.  Cihazin gercekte ne cizdigini
           panele bakmadan denetleyebilmek icin; dokunarak gezmek uzaktan
           mumkun degil. */
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) veri_secim = atoi(argv[++i]);
        else { fprintf(stderr, "kullanim: ekran [-d fb] [-s sn] [-1] [-o cikti.ppm] [-j isler] [-p sayfa] [-k kaynak] [-t]\n"); return 2; }
    }

    /* Tek-ornek kilidi YALNIZCA panele cizen surum icin.
     *
     * Ilk surumde kilit main()'in en basindaydi ve -o (dosyaya ciz) kipi de
     * ona takiliyordu: calisan panel varken cihazin GERCEK ekranini bir
     * dosyaya dokmek imkansizdi.  Kilidin amaci iki surecin ayni framebuffer'a
     * yazmasini onlemek; dosyaya yazan surec o yarisin icinde degil. */
    if (ppm) {
        if (fb_bellek(480, 800) < 0) return 1;
        topla(&d);
        /* Sayfa secimi demo kipine BAGLI DEGIL: cihazin gercek durumunu
           bir dosyaya dokebilmek, panele bakmadan neyin gorundugunu
           denetlemenin tek yolu. */
        if (sayfa_secim >= 0) sayfa = sayfa_secim;
        if (demo) {   /* cihazdaki gercekci degerlerle yerlesimi denetlemek icin */
            snprintf(d.ssid, sizeof(d.ssid), "eduroam");
            snprintf(d.ip, sizeof(d.ip), "10.3.24.117");
            d.sinyal = -54; d.mv = 4012; d.ma = 412; d.yuzde = 78; d.olcer = 78; d.mv_oc = 3930; d.kalan_dk = -47;
            d.khz0 = d.khz1 = 1000000; d.up = 4 * 86400 + 6 * 3600;
            d.bos_mb = 497; d.yuk = 0.12;
        }
        ciz(&d);
        return ppm_yaz(ppm) < 0 ? 1 : 0;
    }

    if (!tek_ornek()) return 0;
    if (aygit) {
        if (fb_ac(aygit) < 0) { fprintf(stderr, "fb acilamadi %s: %s\n", aygit, strerror(errno)); return 1; }
    } else if (fb_ac("/dev/graphics/fb0") < 0 && fb_ac("/dev/fb0") < 0) {
        fprintf(stderr, "fb acilamadi: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "fb %dx%d %dbpp satir=%d\n", fb_g, fb_y, fb_bpp, fb_satir);

    dokunma_ac();
    isik_ac();
    for (;;) {
        int cx, cy, olay;
        time_t simdi;

        topla(&d);
        if (ag_taraniyor) {
            /* Tarama cocugu bitti mi?  Bitince liste dosyasi tazelenmis olur. */
            if (waitpid(-1, NULL, WNOHANG) > 0) ag_taraniyor = 0;
        }
        if (isik_acik) ciz(&d);          /* karanlik panele cizmek bosuna is */
        if (tek) break;

        /* Bekleme uykuda degil DINLEMEDE gecer: dokunmaya bir sonraki
           yenilemeye kadar beklemeden cevap vermek gerekiyor. */
        olay = dokunma_bekle((ag_taraniyor ? 2 : (isik_acik ? bekle : 30)) * 1000, &cx, &cy);
        simdi = time(NULL);

        if (olay == 1) {
            if (!isik_acik) {
                /* Karanlikken ilk dokunus yalnizca uyandirir; o dokunusla
                   sekme degistirmek, ekrani acmak isteyen birine istemedigi
                   sayfayi acar. */
                isik_ac();
            } else if (cy < BASLIK_Y_SON) {
                /* Once ekrani karart, sonra kapat: dokunusun islendigini
                   gormeden kapanan bir ekran, calismamis gibi hissettiriyor. */
                kutu(0, 0, fb_g, fb_y, ZEMIN);
                fb_bas();
                /* Basliga dokunmak uyutur.  45 saniye beklemeden kapatabilmek
                   gerekiyor: ekran 106 mA yiyor ve bakmayi bitirdiginde bunu
                   soyleyecek bir yer olmali. */
                isik_kapat();
            } else {
                int t = sekme_bul(cx, cy);
                if (t >= 0 && t < S_ADET) { if (t != sayfa) net_sifirla(); sayfa = t; }
                else if (sayfa == S_AG) ag_dokunma(cx, cy);
                else if (sayfa == S_ISLER) is_dokunma(cx, cy);
                else if (sayfa == S_VERI && veri_adet > 1) {
                    /* Sol yari geri, sag yari ileri.  Tek yonlu donmek,
                       dorduncu kaynaktan ucuncuye gitmek icin uc dokunus
                       demekti. */
                    if (cx < fb_g / 2)
                        veri_secim = (veri_secim + veri_adet - 1) % veri_adet;
                    else
                        veri_secim = (veri_secim + 1) % veri_adet;
                }
            }
            son_temas = simdi;
        } else if (olay == 3 || olay == 4) {
            /* Kaydirma.  Veri sayfasinda kaynaklar arasinda, baska her yerde
               sekmeler arasinda geziyor - bir panelde "yandaki sey" ne ise
               parmak onu bekliyor. */
            int ileri = (olay == 3);
            if (!isik_acik) {
                isik_ac();
            } else if (sayfa == S_VERI && veri_adet > 1) {
                veri_secim = ileri ? (veri_secim + 1) % veri_adet
                                   : (veri_secim + veri_adet - 1) % veri_adet;
            } else {
                sayfa = ileri ? (sayfa + 1) % S_ADET : (sayfa + S_ADET - 1) % S_ADET;
                net_sifirla();
            }
            son_temas = simdi;
        } else if (olay == 2) {
            isik_ac();
            son_temas = simdi;
        } else if (isik_acik && ISIK_SURESI > 0 &&
                   simdi - son_temas > ISIK_SURESI) {
            isik_kapat();
        }
    }
    isik_ac();
    return 0;
}
