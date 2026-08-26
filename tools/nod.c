/*
 * golden nod - what turns this handset from a phone that boots into a node
 * you can point something at.
 *
 * DESIGN, and why it is C and not Python in the chroot
 *
 * The whole value of this device is that it stays up: it has its own battery,
 * so it keeps answering through a power cut that takes the router's mains with
 * it.  A watchdog that depends on the Android framework is worthless the
 * moment the framework is what failed, and a watchdog that depends on a chroot
 * is worthless the moment /data does not mount.  So: one static binary, no
 * libraries, no interpreter, no framework, ~40 KB, started from init.  It runs
 * in shield mode, it runs during boot, it runs when zygote is crash-looping.
 *
 * HONESTY ABOUT THE BATTERY
 *
 * This handset's fuel gauge fabricates values - a 46 -> 28 -> 29 percent jump
 * inside one session, and a temperature field that is really a float's bit
 * pattern.  So /durum never reports a single "battery" number.  It reports
 * what each source said and whether they agree, and it derives its own
 * estimate from voltage, which is the one measurement that has never lied
 * here.  A caller can then decide what to trust instead of being handed a
 * confident wrong answer.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#define PORT_VARSAYILAN 8088
#define TAMPON 65536

/* ---------------------------------------------------------------- yardimci */

/* Read a small file into buf.  Returns length, or -1.  Trailing newline is
 * stripped because every sysfs value has one and no caller ever wants it. */
static int oku(const char *yol, char *buf, size_t n) {
    int fd = open(yol, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t k = read(fd, buf, n - 1);
    close(fd);
    if (k < 0) return -1;
    while (k > 0 && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) k--;
    buf[k] = 0;
    return (int)k;
}

static long oku_uzun(const char *yol, long varsayilan) {
    char b[64];
    if (oku(yol, b, sizeof b) <= 0) return varsayilan;
    char *son;
    long v = strtol(b, &son, 10);
    return son == b ? varsayilan : v;
}

/* Append to a growing string with bounds checking.  Every writer below goes
 * through this so a long field can never walk off the end of the buffer. */
typedef struct { char *p; size_t n, kap; } Yazi;
static void ekle(Yazi *y, const char *bic, ...) {
    if (y->n >= y->kap) return;
    va_list ap;
    va_start(ap, bic);
    int k = vsnprintf(y->p + y->n, y->kap - y->n, bic, ap);
    va_end(ap);
    if (k > 0) y->n += ((size_t)k < y->kap - y->n) ? (size_t)k : y->kap - y->n;
}

/* JSON string escape - values here come from sysfs and could contain anything */
static void ekle_metin(Yazi *y, const char *s) {
    ekle(y, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') ekle(y, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) ekle(y, "\\u%04x", *s);
        else ekle(y, "%c", *s);
    }
    ekle(y, "\"");
}

/* ------------------------------------------------------------------- pil */

/*
 * Voltage -> remaining charge, for a single Li-ion cell at rest.
 *
 * This is a coarse curve and it is labelled as such in the output: under load
 * the terminal voltage sags and this reads low, while charging it reads high.
 * It is here because it is still more honest than a gauge that jumped 18
 * points in one session, and because unlike that gauge it fails gradually.
 */
static int yuzde_gerilimden(int mv) {
    static const int e[][2] = {
        {4200, 100}, {4100, 92}, {4000, 82}, {3950, 74}, {3900, 66},
        {3850, 57},  {3800, 48}, {3750, 40}, {3700, 32}, {3650, 24},
        {3600, 16},  {3500, 8},  {3400, 3},  {3300, 0},
    };
    int n = (int)(sizeof e / sizeof e[0]);
    if (mv >= e[0][0]) return 100;
    if (mv <= e[n - 1][0]) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (mv <= e[i][0] && mv > e[i + 1][0]) {
            int dv = e[i][0] - e[i + 1][0], dp = e[i][1] - e[i + 1][1];
            return e[i + 1][1] + (mv - e[i + 1][0]) * dp / dv;
        }
    }
    return 0;
}

#define PS "/sys/class/power_supply"

static void pil_json(Yazi *y) {
    char b[128];
    long uv = oku_uzun(PS "/battery/voltage_now", -1);
    long akim = oku_uzun(PS "/battery/current_now", 0);
    long sicaklik = oku_uzun(PS "/battery/temp", -9999);
    long yuzde = oku_uzun(PS "/battery/capacity", -1);
    int mv = uv > 100000 ? (int)(uv / 1000) : (int)uv;   /* uV or mV, both seen */

    ekle(y, "\"pil\":{");
    ekle(y, "\"gerilim_mv\":%d", mv);
    ekle(y, ",\"akim_ham\":%ld", akim);
    ekle(y, ",\"sarj_oluyor\":%s", akim > 0 ? "true" : "false");
    ekle(y, ",\"yuzde_gerilimden\":%d", mv > 0 ? yuzde_gerilimden(mv) : -1);
    ekle(y, ",\"yuzde_olcerden\":%ld", yuzde);

    /* The disagreement is the interesting number, so it is computed here
     * rather than left for the caller to notice. */
    if (mv > 0 && yuzde >= 0) {
        int fark = yuzde_gerilimden(mv) - (int)yuzde;
        ekle(y, ",\"fark\":%d", fark);
        ekle(y, ",\"olcer_guvenilir\":%s", (fark > -12 && fark < 12) ? "true" : "false");
    }
    if (oku(PS "/battery/status", b, sizeof b) > 0) { ekle(y, ",\"durum\":"); ekle_metin(y, b); }
    if (oku(PS "/battery/health", b, sizeof b) > 0) { ekle(y, ",\"saglik\":"); ekle_metin(y, b); }

    /* Temperature: reported, plus whether it is physically possible.  This is
     * the field that powered the handset off fifty seconds into every boot on
     * 26 August, so it is never passed on without its verdict attached. */
    ekle(y, ",\"sicaklik_ondC\":%ld", sicaklik);
    ekle(y, ",\"sicaklik_makul\":%s",
         (sicaklik > -300 && sicaklik < 900) ? "true" : "false");

    /* The other nodes, named, so a future reader can see the trap that cost a
     * night rather than rediscovering it. */
    ekle(y, ",\"diger_dugumler\":{");
    int ilk = 1;
    DIR *d = opendir(PS);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            if (!strcmp(e->d_name, "battery")) continue;
            char yol[512];
            snprintf(yol, sizeof yol, PS "/%s/type", e->d_name);
            if (oku(yol, b, sizeof b) <= 0) continue;
            if (strcmp(b, "Battery")) continue;
            snprintf(yol, sizeof yol, PS "/%s/temp", e->d_name);
            long t = oku_uzun(yol, -9999999);
            if (!ilk) ekle(y, ",");
            ilk = 0;
            ekle(y, "\"%s\":{\"temp\":%ld,\"makul\":%s}", e->d_name, t,
                 (t > -300 && t < 900) ? "true" : "false");
        }
        closedir(d);
    }
    ekle(y, "}}");
}

/* ------------------------------------------------------------------- cpu */

static void cpu_json(Yazi *y) {
    char b[128], yol[512];
    ekle(y, "\"cpu\":{\"cekirdekler\":[");
    for (int i = 0; i < 8; i++) {
        snprintf(yol, sizeof yol, "/sys/devices/system/cpu/cpu%d", i);
        if (access(yol, F_OK)) break;
        if (i) ekle(y, ",");
        snprintf(yol, sizeof yol, "/sys/devices/system/cpu/cpu%d/online", i);
        long on = oku_uzun(yol, 1);
        snprintf(yol, sizeof yol, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        long f = oku_uzun(yol, -1);
        snprintf(yol, sizeof yol, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        if (oku(yol, b, sizeof b) <= 0) strcpy(b, "?");
        ekle(y, "{\"no\":%d,\"acik\":%ld,\"khz\":%ld,\"governor\":", i, on, f);
        ekle_metin(y, b);
        ekle(y, "}");
    }
    ekle(y, "]");
    if (oku("/proc/loadavg", b, sizeof b) > 0) { ekle(y, ",\"yuk\":"); ekle_metin(y, b); }
    ekle(y, "}");
}

/* ------------------------------------------------------------------ sistem */

static void sistem_json(Yazi *y) {
    char b[256];
    struct sysinfo si;
    ekle(y, "\"sistem\":{");
    double up = 0;
    if (oku("/proc/uptime", b, sizeof b) > 0) up = atof(b);
    ekle(y, "\"uptime_sn\":%.2f", up);
    if (!sysinfo(&si)) {
        ekle(y, ",\"ram_toplam_kb\":%lu", (unsigned long)(si.totalram / 1024));
        ekle(y, ",\"ram_bos_kb\":%lu", (unsigned long)(si.freeram / 1024));
        ekle(y, ",\"surec\":%u", si.procs);
    }
    time_t t = time(NULL);
    struct tm tm;
    if (gmtime_r(&t, &tm)) {
        strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
        ekle(y, ",\"utc\":"); ekle_metin(y, b);
    }
    /* Which shutdown paths are currently disarmed, so the caller can tell a
     * healthy node from one that is merely not being killed yet. */
    ekle(y, ",\"kalkan\":%s", access("/cache/kalkan", F_OK) == 0 ? "true" : "false");
    ekle(y, ",\"geri_don\":%s", access("/cache/geri-don", F_OK) == 0 ? "true" : "false");
    ekle(y, "}");
}

static void ag_json(Yazi *y) {
    char b[512];
    ekle(y, "\"ag\":{\"arayuzler\":[");
    DIR *d = opendir("/sys/class/net");
    int ilk = 1;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char yol[512];
            snprintf(yol, sizeof yol, "/sys/class/net/%s/operstate", e->d_name);
            if (oku(yol, b, sizeof b) <= 0) strcpy(b, "?");
            if (!ilk) ekle(y, ",");
            ilk = 0;
            ekle(y, "{\"ad\":"); ekle_metin(y, e->d_name);
            ekle(y, ",\"durum\":"); ekle_metin(y, b);
            snprintf(yol, sizeof yol, "/sys/class/net/%s/address", e->d_name);
            if (oku(yol, b, sizeof b) > 0) { ekle(y, ",\"mac\":"); ekle_metin(y, b); }
            ekle(y, "}");
        }
        closedir(d);
    }
    ekle(y, "]}");
}

static void isi_json(Yazi *y) {
    char b[128], yol[512];
    ekle(y, "\"isi\":[");
    int ilk = 1;
    for (int i = 0; i < 16; i++) {
        snprintf(yol, sizeof yol, "/sys/class/thermal/thermal_zone%d/temp", i);
        long t = oku_uzun(yol, -999999);
        if (t == -999999) continue;
        snprintf(yol, sizeof yol, "/sys/class/thermal/thermal_zone%d/type", i);
        if (oku(yol, b, sizeof b) <= 0) strcpy(b, "?");
        if (!ilk) ekle(y, ",");
        ilk = 0;
        ekle(y, "{\"bolge\":%d,\"tur\":", i); ekle_metin(y, b);
        ekle(y, ",\"deger\":%ld}", t);
    }
    ekle(y, "]");
}


/* gonder() asagida tanimli; halka kodu ondan once geliyor. */
static void gonder(int s, const char *durum, const char *tur, const char *govde, size_t n);

/* --------------------------------------------------------------- gecmis
 *
 * A fixed-size ring of samples on disk, and the reason it is a ring and not a
 * log: this node's whole point is that it keeps running through a power cut
 * that takes the router with it.  During that cut nothing can collect from it,
 * so the data has to survive locally - and on an 8 GB handset an unbounded log
 * is a way to fill /data and lose the node instead.
 *
 * Pull, not push.  A pushing agent needs an outbound queue, retry logic and a
 * clock to decide when to give up; a ring that holds a day of history needs
 * none of that, because the collector simply catches up when the network comes
 * back.  The queue IS the ring.
 *
 * Records are fixed-width ASCII.  Fixed-width so the ring can be written in
 * place with one pwrite and no locking; ASCII so that when something has gone
 * wrong the file can be read with `tail` on a handset that may not have much
 * else working.
 */
#define KAYIT_BOYU 128
#define KAYIT_ADEDI 8640          /* 10 s araliklarla 24 saat */
#define GECMIS_YOL "/data/sirsch/gecmis.ring"
#define ORNEK_ARALIK 10

static char gecmis_yol[256] = GECMIS_YOL;

/* Header lives in record slot 0 and holds nothing but the next write index,
 * so a torn write can lose one sample and never the file. */
static long gecmis_indis(int fd) {
    char b[KAYIT_BOYU + 1];
    if (pread(fd, b, KAYIT_BOYU, 0) != KAYIT_BOYU) return 0;
    b[KAYIT_BOYU] = 0;
    long v = strtol(b, NULL, 10);
    return (v < 0 || v >= KAYIT_ADEDI) ? 0 : v;
}

static void gecmis_yaz(void) {
    int fd = open(gecmis_yol, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return;

    /* First run: lay the whole ring down as blanks so every later write is an
     * in-place overwrite and the file never grows. */
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)((KAYIT_ADEDI + 1) * KAYIT_BOYU)) {
        char bos[KAYIT_BOYU];
        memset(bos, ' ', KAYIT_BOYU); bos[KAYIT_BOYU - 1] = '\n';
        for (long i = 0; i <= KAYIT_ADEDI; i++)
            if (pwrite(fd, bos, KAYIT_BOYU, i * KAYIT_BOYU) != KAYIT_BOYU) { close(fd); return; }
    }

    long uv = oku_uzun(PS "/battery/voltage_now", -1);
    int mv = uv > 100000 ? (int)(uv / 1000) : (int)uv;
    char yuk[64] = "-";
    oku("/proc/loadavg", yuk, sizeof yuk);
    char *bosluk = strchr(yuk, ' '); if (bosluk) *bosluk = 0;
    char up[64] = "0";
    oku("/proc/uptime", up, sizeof up);
    bosluk = strchr(up, ' '); if (bosluk) *bosluk = 0;
    struct sysinfo si; si.freeram = 0; si.mem_unit = 1;
    sysinfo(&si);

    char kayit[KAYIT_BOYU + 1];
    int k = snprintf(kayit, sizeof kayit,
        "%ld %s %d %ld %ld %ld %ld %ld %s %lu %s",
        (long)time(NULL), up, mv,
        oku_uzun(PS "/battery/current_now", 0),
        oku_uzun(PS "/battery/capacity", -1),
        oku_uzun(PS "/battery/temp", -9999),
        oku_uzun("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", -1),
        oku_uzun("/sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq", -1),
        yuk, (unsigned long)(si.freeram / 1024 * si.mem_unit),
        access("/sys/class/net/wlan0/operstate", R_OK) == 0 ? "wlan" : "-");
    if (k < 0) { close(fd); return; }
    if (k > KAYIT_BOYU - 1) k = KAYIT_BOYU - 1;
    memset(kayit + k, ' ', KAYIT_BOYU - k);
    kayit[KAYIT_BOYU - 1] = '\n';

    long i = gecmis_indis(fd);
    /* Sample first, index second.  Crash between the two and the collector
     * re-reads one old record; the other order would hand it a blank. */
    if (pwrite(fd, kayit, KAYIT_BOYU, (i + 1) * KAYIT_BOYU) == KAYIT_BOYU) {
        char h[KAYIT_BOYU];
        memset(h, ' ', KAYIT_BOYU); h[KAYIT_BOYU - 1] = '\n';
        snprintf(h, sizeof h, "%ld", (i + 1) % KAYIT_ADEDI);
        h[strlen(h)] = ' ';
        h[KAYIT_BOYU - 1] = '\n';
        if (pwrite(fd, h, KAYIT_BOYU, 0) != KAYIT_BOYU) { /* bir ornek kaybi, dosya saglam */ }
    }
    close(fd);
}

/* Oldest first, so a collector can just append what it has not seen. */
static void gecmis_ver(int s) {
    int fd = open(gecmis_yol, O_RDONLY);
    if (fd < 0) { gonder(s, "404 Not Found", "text/plain", "gecmis yok\n", 11); return; }
    static char buf[KAYIT_ADEDI * 40 + 512];
    size_t n = 0;
    const char *b = "# epoch uptime mv akim yuzde tempC10 khz0 khz1 yuk1 mem_kb ag\n";
    n += snprintf(buf + n, sizeof buf - n, "%s", b);
    long i = gecmis_indis(fd);
    char kayit[KAYIT_BOYU + 1];
    for (long adim = 0; adim < KAYIT_ADEDI && n < sizeof buf - KAYIT_BOYU - 2; adim++) {
        long slot = ((i + adim) % KAYIT_ADEDI) + 1;
        if (pread(fd, kayit, KAYIT_BOYU, slot * KAYIT_BOYU) != KAYIT_BOYU) break;
        kayit[KAYIT_BOYU] = 0;
        if (kayit[0] == ' ') continue;                 /* henuz yazilmamis */
        char *son = kayit + KAYIT_BOYU - 1;
        while (son > kayit && (*son == ' ' || *son == '\n')) son--;
        size_t uzun = (size_t)(son - kayit + 1);
        memcpy(buf + n, kayit, uzun); n += uzun;
        buf[n++] = '\n';
    }
    close(fd);
    gonder(s, "200 OK", "text/plain; charset=utf-8", buf, n);
}

/* ------------------------------------------------------------------- http */

static void gonder(int s, const char *durum, const char *tur, const char *govde, size_t n) {
    char bas[512];
    int k = snprintf(bas, sizeof bas,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n"
                     "Cache-Control: no-store\r\n\r\n", durum, tur, n);
    if (write(s, bas, k) < 0) return;
    size_t yazildi = 0;
    while (yazildi < n) {
        ssize_t w = write(s, govde + yazildi, n - yazildi);
        if (w <= 0) break;
        yazildi += (size_t)w;
    }
}

static void durum_ver(int s) {
    static char buf[TAMPON];
    Yazi y = {buf, 0, sizeof buf};
    ekle(&y, "{");
    ekle(&y, "\"nod\":\"golden\",");
    sistem_json(&y); ekle(&y, ",");
    pil_json(&y);    ekle(&y, ",");
    cpu_json(&y);    ekle(&y, ",");
    isi_json(&y);    ekle(&y, ",");
    ag_json(&y);
    ekle(&y, "}\n");
    gonder(s, "200 OK", "application/json; charset=utf-8", buf, y.n);
}

/* Serve a file, capped: this runs on a handset with 800 MB of RAM and the
 * caller could ask for anything under the allowed roots. */
static void dosya_ver(int s, const char *yol, const char *tur) {
    int fd = open(yol, O_RDONLY);
    if (fd < 0) { gonder(s, "404 Not Found", "text/plain", "yok\n", 4); return; }
    static char buf[TAMPON];
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k < 0) k = 0;
    buf[k] = 0;
    gonder(s, "200 OK", tur, buf, (size_t)k);
}


/* Tarayicidan bakildiginda ne gorunecegi.
 *
 * Tek dosya, dis kaynak yok: bu telefon cogu zaman evin agina bile bagli
 * degil, rndis ucundaki tek IP olabiliyor.  CDN'den font ceken bir sayfa tam
 * ihtiyac duyuldugu anda bos ekran olur.
 *
 * Pil kismi bilerek uc sayi gosteriyor.  Bu cihazin yakit olceri uyduruyor
 * (bir oturumda %46 -> %28 -> %29), o yuzden tek bir yuzde gostermek okuyanı
 * yanlis bir kesinlige ikna etmek olurdu.  Olcerin dedigi, gerilimden
 * turetilen ve ikisinin farki yan yana duruyor; anlasmiyorlarsa bunu sayfa
 * kendisi soyluyor. */
static const char *SAYFA =
"<!doctype html><html lang='tr'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>golden nod</title><style>"
":root{--z:#faf9f7;--y:#1b1a18;--k:#6b6862;--c:#d9d5cd;--i:#2f6f4f;--u:#a8442a}"
"@media(prefers-color-scheme:dark){:root{--z:#141412;--y:#eceae5;--k:#8f8b83;--c:#2c2a26;--i:#63b98c;--u:#e0785a}}"
"*{box-sizing:border-box}body{margin:0;padding:18px;background:var(--z);color:var(--y);"
"font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
"h1{font-size:15px;margin:0 0 2px;letter-spacing:.06em;text-transform:uppercase}"
".alt{color:var(--k);font-size:12px;margin:0 0 16px}"
".k{border:1px solid var(--c);border-radius:5px;padding:12px 14px;margin:0 0 12px}"
".k h2{font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--k);margin:0 0 9px;font-weight:600}"
"table{width:100%;border-collapse:collapse}td{padding:2px 0;vertical-align:top}"
"td:first-child{color:var(--k);width:48%}td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
".b{color:var(--i)}.f{color:var(--u)}"
"canvas{width:100%;height:70px;display:block}"
"</style></head><body>"
"<h1>golden nod</h1><p class='alt' id='alt'>yukleniyor...</p>"
"<div class='k'><h2>pil</h2><table id='pil'></table></div>"
"<div class='k'><h2>gerilim &mdash; son kayitlar</h2><canvas id='g'></canvas></div>"
"<div class='k'><h2>islemci</h2><table id='cpu'></table></div>"
"<div class='k'><h2>sistem</h2><table id='sis'></table></div>"
"<div class='k'><h2>ag</h2><table id='ag'></table></div>"
"<script>\n"
"const q=s=>document.getElementById(s);\n"
"const sat=(t,r)=>{t.innerHTML=r.map(([a,b,c])=>"
"`<tr><td>${a}</td><td class='${c||\"\"}'>${b}</td></tr>`).join('')};\n"
"function sure(s){s=Math.floor(s);const g=Math.floor(s/86400),h=Math.floor(s%86400/3600),"
"d=Math.floor(s%3600/60);return (g?g+'g ':'')+(h||g?h+'s ':'')+d+'dk'}\n"
"async function tazele(){\n"
" try{\n"
"  const d=await (await fetch('/durum',{cache:'no-store'})).json();\n"
"  q('alt').textContent=d.sistem.utc+' UTC \\u00b7 ayakta '+sure(d.sistem.uptime_sn);\n"
"  const p=d.pil, uy=p.olcer_guvenilir===false;\n"
"  sat(q('pil'),[\n"
"   ['gerilim',p.gerilim_mv+' mV'],\n"
"   ['gerilimden yuzde',p.yuzde_gerilimden+'%'],\n"
"   ['olcerin dedigi',p.yuzde_olcerden+'%',uy?'f':''],\n"
"   ['fark',(p.fark>0?'+':'')+p.fark+' puan',uy?'f':'b'],\n"
"   ['olcere guvenilir mi',uy?'HAYIR':'evet',uy?'f':'b'],\n"
"   ['durum',p.durum||'-',p.sarj_oluyor?'b':''],\n"
"   ['sicaklik',(p.sicaklik_ondC/10).toFixed(1)+' \\u00b0C',p.sicaklik_makul?'':'f'],\n"
"   ['sicaklik makul mu',p.sicaklik_makul?'evet':'HAYIR',p.sicaklik_makul?'b':'f']]);\n"
"  sat(q('cpu'),d.cpu.cekirdekler.map(c=>['cekirdek '+c.no,\n"
"   (c.acik?(c.khz/1000)+' MHz \\u00b7 '+c.governor:'KAPALI'),c.acik?'':'f'])\n"
"   .concat([['yuk',d.cpu.yuk.split(' ').slice(0,3).join(' ')]]));\n"
"  sat(q('sis'),[['ram bos',Math.round(d.sistem.ram_bos_kb/1024)+' / '+\n"
"   Math.round(d.sistem.ram_toplam_kb/1024)+' MiB'],['surec',d.sistem.surec],\n"
"   ['kalkan modu',d.sistem.kalkan?'ACIK':'kapali',d.sistem.kalkan?'f':''],\n"
"   ['isi',(d.isi||[]).map(z=>z.tur+'='+z.deger).join(' ')||'-']]);\n"
"  sat(q('ag'),d.ag.arayuzler.filter(a=>a.ad!=='lo')\n"
"   .map(a=>[a.ad,a.durum,a.durum==='up'?'b':'']));\n"
" }catch(e){q('alt').textContent='durum alinamadi: '+e}\n"
" try{\n"
"  const t=await (await fetch('/gecmis',{cache:'no-store'})).text();\n"
"  const v=t.split('\\n').filter(l=>l&&l[0]!=='#').map(l=>+l.split(' ')[2]).filter(x=>x>1000);\n"
"  const c=q('g'),x=c.getContext('2d'),W=c.width=c.clientWidth*2,H=c.height=140;\n"
"  x.clearRect(0,0,W,H); if(v.length<2)return;\n"
"  const mn=Math.min(...v),mx=Math.max(...v),d2=(mx-mn)||1;\n"
"  x.strokeStyle=getComputedStyle(document.body).getPropertyValue('--i');\n"
"  x.lineWidth=2; x.beginPath();\n"
"  v.forEach((y,i)=>{const px=i/(v.length-1)*W,py=H-8-(y-mn)/d2*(H-22);\n"
"   i?x.lineTo(px,py):x.moveTo(px,py)}); x.stroke();\n"
"  x.fillStyle=getComputedStyle(document.body).getPropertyValue('--k');\n"
"  x.font='20px monospace'; x.fillText((mx/1000).toFixed(2)+'V',4,18);\n"
"  x.fillText((mn/1000).toFixed(2)+'V',4,H-4);\n"
" }catch(e){}\n"
"}\n"
"tazele(); setInterval(tazele,10000);\n"
"</script></body></html>\n";

static const char *KOK_SAYFA =
    "golden nod\n"
    "\n"
    "  /durum    JSON: uptime, pil (uc kaynak ayri ayri), cpu, isi, ag\n"
    "  /gecmis   son 24 saatin ornekleri (10 sn arayla, halka tampon)\n"
    "  /olcum    son olcum raporu\n"
    "  /kmesg    cekirdek halka tamponu (son 64 KB)\n"
    "  /wifi     wifi kurulum gunlugu\n"
    "  /saglik   tek satir: ayakta miyim\n"
    "  /         tarayici icin durum sayfasi\n"
    "\n"
    "Pil hakkinda: bu cihazin yakit olceri uyduruyor.  /durum yuzdeyi TEK bir\n"
    "sayi olarak vermez - olcerin dedigini, gerilimden turetileni ve ikisinin\n"
    "farkini ayri ayri verir.  Fark 12 puandan buyukse olcer_guvenilir=false.\n";

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : PORT_VARSAYILAN;
    signal(SIGPIPE, SIG_IGN);

    int d = socket(AF_INET, SOCK_STREAM, 0);
    if (d < 0) { perror("socket"); return 1; }
    int bir = 1;
    setsockopt(d, SOL_SOCKET, SO_REUSEADDR, &bir, sizeof bir);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(d, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(d, 8) < 0) { perror("listen"); return 1; }

    /* Testte halka baska bir yere yazilabilsin: cihazda /data/sirsch dogru
     * yer ama qemu altinda dogrulama yapmanin tek yolu bu. */
    const char *ozel = getenv("GOLDEN_GECMIS");
    if (ozel && *ozel) snprintf(gecmis_yol, sizeof gecmis_yol, "%s", ozel);
    mkdir("/data/sirsch", 0755);
    time_t son_ornek = 0;

    for (;;) {
        /* One thread, no forking.  The sampler shares the accept loop through
         * select's timeout: on a two-core handset a status endpoint that
         * spawns under load is a way to lose the node, and the sampling
         * interval is 10 s - there is no contention to design around. */
        fd_set r;
        FD_ZERO(&r); FD_SET(d, &r);
        struct timeval bekle = {1, 0};
        int hazir = select(d + 1, &r, NULL, NULL, &bekle);
        time_t simdi_sn = time(NULL);
        if (simdi_sn - son_ornek >= ORNEK_ARALIK) { gecmis_yaz(); son_ornek = simdi_sn; }
        if (hazir <= 0) continue;

        int s = accept(d, NULL, NULL);
        if (s < 0) { if (errno == EINTR) continue; break; }
        /* One request per connection, served inline.  There is no concurrency
         * here on purpose: two Cortex-A9 cores are the whole machine and a
         * status endpoint that forks under load is a way to lose the node. */
        struct timeval zaman = {5, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &zaman, sizeof zaman);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &zaman, sizeof zaman);
        char istek[1024];
        ssize_t k = read(s, istek, sizeof istek - 1);
        if (k <= 0) { close(s); continue; }
        istek[k] = 0;
        char *yol = strchr(istek, ' ');
        if (!yol) { close(s); continue; }
        yol++;
        char *son = strpbrk(yol, " ?\r\n");
        if (son) *son = 0;

        if (!strcmp(yol, "/") )              gonder(s, "200 OK", "text/html; charset=utf-8", SAYFA, strlen(SAYFA));
        else if (!strcmp(yol, "/yardim"))    gonder(s, "200 OK", "text/plain; charset=utf-8", KOK_SAYFA, strlen(KOK_SAYFA));
        else if (!strcmp(yol, "/durum"))     durum_ver(s);
        else if (!strcmp(yol, "/saglik"))    gonder(s, "200 OK", "text/plain", "ayakta\n", 7);
        else if (!strcmp(yol, "/gecmis"))    gecmis_ver(s);
        else if (!strcmp(yol, "/olcum"))     dosya_ver(s, "/data/olcum/sonuc.txt", "text/plain; charset=utf-8");
        else if (!strcmp(yol, "/wifi"))      dosya_ver(s, "/data/olcum/wifi.log", "text/plain; charset=utf-8");
        else if (!strcmp(yol, "/kmesg"))     dosya_ver(s, "/proc/kmsg", "text/plain");
        else                                 gonder(s, "404 Not Found", "text/plain", "yok\n", 4);
        close(s);
    }
    return 0;
}
