/*
 * dhcp - the smallest DHCP client that will do, for a handset with no framework.
 *
 * Why it exists: on this ROM the DHCP client lives INSIDE the Android
 * framework (IpClient / netd).  With the framework stopped - which is how this
 * device is used - there is no dhcptool, no dhcpcd, no udhcpc, no busybox
 * applet.  Looked; none.  A static address works at home and is useless
 * anywhere else, and portability is the whole point of the box.
 *
 * DISCOVER -> OFFER -> REQUEST -> ACK, then address/netmask/gateway are applied.
 *
 * The one thing that is easy to get wrong: this must use AF_PACKET, not a UDP
 * socket.  Servers unicast the OFFER to an address the interface does not have
 * yet, and a raw sendto() on an address-less interface is silently swallowed -
 * the call succeeds and the TX counter never moves.
 *
 * Build: arm-linux-gnueabihf-gcc -O2 -static -o dhcp dhcp.c
 * Use:   dhcp [-i wlan0] [-t seconds] [-y] [-v] [-n]
 *        -n is a dry run: it takes the lease but does not touch the interface.
 *
 * NOTE: the inline commentary below is in Turkish.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <linux/if_ether.h>
#include <linux/sockios.h>
#include <linux/if_packet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#define BOOTREQUEST 1
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6
#define COOKIE 0x63825363u

struct paket {
    unsigned char  op, htype, hlen, hops;
    unsigned int   xid;
    unsigned short secs, flags;
    unsigned int   ciaddr, yiaddr, siaddr, giaddr;
    unsigned char  chaddr[16];
    unsigned char  sname[64], file[128];
    unsigned char  sec[312];          /* magic + options */
};

static int   ayrintili = 0;
static int   cihaza_bagla = 1;
static int   deneme = 0;
static char  arayuz[IFNAMSIZ] = "wlan0";
static unsigned char mac[ETH_ALEN];

static void ayrinti(const char *b, ...) __attribute__((format(printf, 1, 2)));
static void ayrinti(const char *b, ...)
{
    va_list a;
    if (!ayrintili) return;
    va_start(a, b); vfprintf(stderr, b, a); va_end(a);
}

static int mac_al(int s)
{
    struct ifreq r;
    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, IFNAMSIZ, "%s", arayuz);
    if (ioctl(s, SIOCGIFHWADDR, &r) < 0) return -1;
    memcpy(mac, r.ifr_hwaddr.sa_data, ETH_ALEN);
    return 0;
}

static int arayuz_ac(int s)
{
    struct ifreq r;
    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, IFNAMSIZ, "%s", arayuz);
    if (ioctl(s, SIOCGIFFLAGS, &r) < 0) return -1;
    r.ifr_flags |= IFF_UP | IFF_BROADCAST;
    return ioctl(s, SIOCSIFFLAGS, &r);
}

/* Options are written from the caller's cursor so the two messages we send can
   share the code that builds them. */
static unsigned char *sec_yaz(unsigned char *p, unsigned char kod,
                              unsigned char n, const void *v)
{
    *p++ = kod; *p++ = n;
    memcpy(p, v, n);
    return p + n;
}

static void paket_kur(struct paket *pk, unsigned int xid, int tur,
                      unsigned int istenen, unsigned int sunucu)
{
    unsigned char *p;
    unsigned int cookie = htonl(COOKIE);
    unsigned char istek[] = { 1, 3, 6, 15, 28, 51 };  /* mask, router, dns, domain, bcast, lease */
    unsigned char kimlik[1 + ETH_ALEN];

    memset(pk, 0, sizeof(*pk));
    pk->op = BOOTREQUEST; pk->htype = 1; pk->hlen = ETH_ALEN;
    pk->xid = xid;
    /* Broadcast flag: the server must not unicast to an address we do not have
       yet.  Without it some APs ARP for an unassigned IP and the reply is lost. */
    pk->flags = htons(0x8000);
    memcpy(pk->chaddr, mac, ETH_ALEN);

    p = pk->sec;
    memcpy(p, &cookie, 4); p += 4;
    { unsigned char t = (unsigned char)tur; p = sec_yaz(p, 53, 1, &t); }
    kimlik[0] = 1; memcpy(kimlik + 1, mac, ETH_ALEN);
    p = sec_yaz(p, 61, sizeof(kimlik), kimlik);
    p = sec_yaz(p, 12, 6, "golden");
    if (istenen) p = sec_yaz(p, 50, 4, &istenen);
    if (sunucu)  p = sec_yaz(p, 54, 4, &sunucu);
    p = sec_yaz(p, 55, sizeof(istek), istek);
    *p++ = 255;
}

static const unsigned char *sec_bul(const struct paket *pk, unsigned char kod,
                                    unsigned char *uzunluk)
{
    const unsigned char *p = pk->sec + 4, *son = pk->sec + sizeof(pk->sec);
    while (p < son && *p != 255) {
        if (*p == 0) { p++; continue; }
        if (p + 2 > son) break;
        if (p[0] == kod) { *uzunluk = p[1]; return p + 2; }
        p += 2 + p[1];
    }
    return NULL;
}

static int tur_al(const struct paket *pk)
{
    unsigned char n;
    const unsigned char *v = sec_bul(pk, 53, &n);
    return (v && n == 1) ? *v : -1;
}

/* Before the interface has an address the UDP path does not work: many servers
 * unicast the OFFER to the address they are about to hand out, and the kernel
 * drops it because that address is not ours yet.  This is why dhcpcd and udhcpc
 * use AF_PACKET.  Measured here on 26 August: DISCOVER left the interface (tx
 * counter moved), ping through the same link worked at 21 ms, and the UDP
 * socket on port 68 never saw a byte.
 */
struct ham {
    struct iphdr  ip;
    struct udphdr udp;
    struct paket  dhcp;
} __attribute__((packed));

static unsigned short toplam(const void *v, int n, unsigned int on)
{
    const unsigned short *p = v;
    unsigned int t = on;
    while (n > 1) { t += *p++; n -= 2; }
    if (n) t += *(const unsigned char *)p;
    while (t >> 16) t = (t & 0xFFFF) + (t >> 16);
    return (unsigned short)~t;
}

static int ham_ac(int indeks)
{
    struct sockaddr_ll a;
    int s = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (s < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sll_family   = AF_PACKET;
    a.sll_protocol = htons(ETH_P_IP);
    a.sll_ifindex  = indeks;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
    return s;
}

static int ham_gonder(int s, int indeks, const struct paket *pk)
{
    struct sockaddr_ll a;
    struct ham h;
    int n = sizeof(h);

    memset(&h, 0, sizeof(h));
    h.dhcp = *pk;
    h.udp.source = htons(68);
    h.udp.dest   = htons(67);
    h.udp.len    = htons(sizeof(struct udphdr) + sizeof(struct paket));
    /* Zero is a legal IPv4 UDP checksum meaning "not computed", and every DHCP
       server accepts it.  Hand-rolling the pseudo-header here would add a class
       of bug that is invisible from this side: a wrong checksum is dropped
       silently by the server and looks exactly like no server at all. */
    h.udp.check = 0;

    h.ip.version = 4; h.ip.ihl = 5; h.ip.tot_len = htons(n);
    h.ip.ttl = 64; h.ip.protocol = IPPROTO_UDP;
    h.ip.saddr = 0; h.ip.daddr = INADDR_BROADCAST;
    h.ip.check = toplam(&h.ip, sizeof(h.ip), 0);

    memset(&a, 0, sizeof(a));
    a.sll_family   = AF_PACKET;
    a.sll_protocol = htons(ETH_P_IP);
    a.sll_ifindex  = indeks;
    a.sll_halen    = ETH_ALEN;
    memset(a.sll_addr, 0xFF, ETH_ALEN);
    return sendto(s, &h, n, 0, (struct sockaddr *)&a, sizeof(a));
}

static unsigned long ham_sayac = 0, ham_udp = 0;

static int ham_al(int s, struct paket *pk, unsigned int xid)
{
    struct ham h;
    ssize_t n = recv(s, &h, sizeof(h), 0);
    if (n < 0) return -1;
    ham_sayac++;
    if (n < (ssize_t)(sizeof(struct iphdr) + sizeof(struct udphdr) + 240)) return -1;
    if (h.ip.protocol != IPPROTO_UDP) return -1;
    ham_udp++;
    ayrinti("  udp %u->%u (xid %08x)\n",
            ntohs(h.udp.source), ntohs(h.udp.dest), h.dhcp.xid);
    if (h.udp.dest != htons(68) || h.udp.source != htons(67)) return -1;
    if (h.dhcp.xid != xid) return -1;
    *pk = h.dhcp;
    return 0;
}

static int adres_uygula(int s, unsigned int ip, unsigned int maske, unsigned int gecit)
{
    struct ifreq r;
    struct sockaddr_in *sa = (struct sockaddr_in *)&r.ifr_addr;
    struct rtentry rt;
    struct sockaddr_in *g;

    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, IFNAMSIZ, "%s", arayuz);
    sa->sin_family = AF_INET; sa->sin_addr.s_addr = ip;
    if (ioctl(s, SIOCSIFADDR, &r) < 0) { perror("SIOCSIFADDR"); return -1; }

    if (maske) {
        sa->sin_addr.s_addr = maske;
        if (ioctl(s, SIOCSIFNETMASK, &r) < 0) perror("SIOCSIFNETMASK");
    }
    arayuz_ac(s);

    if (!gecit) return 0;
    memset(&rt, 0, sizeof(rt));
    g = (struct sockaddr_in *)&rt.rt_gateway;
    g->sin_family = AF_INET; g->sin_addr.s_addr = gecit;
    ((struct sockaddr_in *)&rt.rt_dst)->sin_family = AF_INET;
    ((struct sockaddr_in *)&rt.rt_genmask)->sin_family = AF_INET;
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    rt.rt_dev = arayuz;
    if (ioctl(s, SIOCADDRT, &rt) < 0 && errno != EEXIST)
        perror("SIOCADDRT");
    return 0;
}

int main(int argc, char **argv)
{
    int s, i, indeks, zaman_asimi = 20, yenile = 0, gecici = 0;
    unsigned int xid, sunucu = 0, teklif = 0, maske = 0, gecit = 0, kira = 3600;
    struct paket pk;
    struct timeval tv;
    char yazi[32];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) snprintf(arayuz, IFNAMSIZ, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) zaman_asimi = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y")) yenile = 1;
        else if (!strcmp(argv[i], "-v")) ayrintili = 1;
        else if (!strcmp(argv[i], "-b")) cihaza_bagla = 0;
        /* -n: kiralamayi al ama UYGULAMA.  Bagli oldugun arayuzde adresi
           degistirmek baglantiyi koparir - 26 Agu: tam bunu yaptim ve
           telefonu kendi testimle kaybettim. */
        else if (!strcmp(argv[i], "-n")) deneme = 1;
        else { fprintf(stderr, "kullanim: dhcp [-i arayuz] [-t sn] [-y] [-v] [-b] [-n]\n"); return 2; }
    }

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    if (mac_al(s) < 0) { perror("SIOCGIFHWADDR"); return 1; }
    arayuz_ac(s);
    close(s);

    {
        struct ifreq r;
        int t = socket(AF_INET, SOCK_DGRAM, 0);
        memset(&r, 0, sizeof(r));
        snprintf(r.ifr_name, IFNAMSIZ, "%s", arayuz);
        if (ioctl(t, SIOCGIFINDEX, &r) < 0) { perror("SIOCGIFINDEX"); return 1; }
        indeks = r.ifr_ifindex;
        close(t);
    }
    /* Chicken and egg, measured on this driver: sendto() on the AF_PACKET
       socket reports success while the interface tx counter does not move, as
       long as the interface has no address.  With one assigned, the same call
       puts 82 bytes on the wire.  So give it a link-local address first and
       take it back once the lease arrives - RFC 3927 space exists for exactly
       this kind of "no address yet" state. */
    {
        int t = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq r;
        struct sockaddr_in *sa = (struct sockaddr_in *)&r.ifr_addr;
        memset(&r, 0, sizeof(r));
        snprintf(r.ifr_name, IFNAMSIZ, "%s", arayuz);
        if (ioctl(t, SIOCGIFADDR, &r) < 0) {
            memset(&r, 0, sizeof(r));
            snprintf(r.ifr_name, IFNAMSIZ, "%s", arayuz);
            sa->sin_family = AF_INET;
            /* 169.254.<mac5>.<mac6>, .0 ve .255 haric */
            sa->sin_addr.s_addr = htonl(0xA9FE0000u
                                        | ((unsigned int)mac[4] << 8)
                                        | (mac[5] ? mac[5] : 1));
            if (ioctl(t, SIOCSIFADDR, &r) < 0) perror("gecici adres");
            else { gecici = 1; ayrinti("gecici link-local verildi\n"); }
            arayuz_ac(t);
        }
        close(t);
    }

    s = ham_ac(indeks);
    if (s < 0) { perror("AF_PACKET"); return 1; }
    tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    xid = (unsigned int)time(NULL) ^ ((unsigned int)getpid() << 16)
        ^ (mac[4] << 8) ^ mac[5];

    for (i = 0; i < zaman_asimi / 3 + 1; i++) {
        paket_kur(&pk, xid, DHCPDISCOVER, 0, 0);
        if (ham_gonder(s, indeks, &pk) < 0) perror("discover");
        ayrinti("discover gonderildi (%d)\n", i + 1);
        for (;;) {
            struct paket c;
            if (ham_al(s, &c, xid) < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                continue;
            }
            ayrinti("  paket: tur=%d yiaddr=%s\n", tur_al(&c),
                    inet_ntop(AF_INET, &c.yiaddr, yazi, sizeof(yazi)));
            if (tur_al(&c) != DHCPOFFER) continue;
            {
                unsigned char u;
                const unsigned char *v;
                teklif = c.yiaddr;
                v = sec_bul(&c, 54, &u); if (v && u == 4) memcpy(&sunucu, v, 4);
                v = sec_bul(&c,  1, &u); if (v && u == 4) memcpy(&maske, v, 4);
                v = sec_bul(&c,  3, &u); if (v && u >= 4) memcpy(&gecit, v, 4);
            }
            ayrinti("offer: %s\n", inet_ntop(AF_INET, &teklif, yazi, sizeof(yazi)));
            break;
        }
        if (teklif) break;
    }
    if (!teklif) {
        fprintf(stderr, "teklif gelmedi (%s) - ham sokette %lu paket, %lu udp\n",
                arayuz, ham_sayac, ham_udp);
        if (gecici) {   /* gecici adresi birakma: yanlis agda kalmasin */
            int t = socket(AF_INET, SOCK_DGRAM, 0);
            struct ifreq r;
            struct sockaddr_in *sa = (struct sockaddr_in *)&r.ifr_addr;
            memset(&r, 0, sizeof(r));
            snprintf(r.ifr_name, IFNAMSIZ, "%s", arayuz);
            sa->sin_family = AF_INET; sa->sin_addr.s_addr = 0;
            ioctl(t, SIOCSIFADDR, &r);
            close(t);
        }
        return 1;
    }

    for (i = 0; i < 4; i++) {
        struct paket c;
        paket_kur(&pk, xid, DHCPREQUEST, teklif, sunucu);
        if (ham_gonder(s, indeks, &pk) < 0) perror("request");
        if (ham_al(s, &c, xid) < 0) continue;
        if (tur_al(&c) == DHCPNAK) { fprintf(stderr, "NAK\n"); return 1; }
        if (tur_al(&c) != DHCPACK) continue;
        {
            unsigned char u;
            const unsigned char *v;
            teklif = c.yiaddr;
            v = sec_bul(&c, 1, &u);  if (v && u == 4) memcpy(&maske, v, 4);
            v = sec_bul(&c, 3, &u);  if (v && u >= 4) memcpy(&gecit, v, 4);
            v = sec_bul(&c, 51, &u); if (v && u == 4) { memcpy(&kira, v, 4); kira = ntohl(kira); }
            v = sec_bul(&c, 6, &u);
            if (v && u >= 4) {
                FILE *f = fopen("/data/sirsch/dns", "w");
                if (f) {
                    unsigned char k;
                    for (k = 0; k + 4 <= u; k += 4) {
                        unsigned int d;
                        memcpy(&d, v + k, 4);
                        fprintf(f, "%s\n", inet_ntop(AF_INET, &d, yazi, sizeof(yazi)));
                    }
                    fclose(f);
                }
            }
        }
        break;
    }
    close(s);

    if (deneme) {
        printf("DENEME (uygulanmadi): %s ", inet_ntop(AF_INET, &teklif, yazi, sizeof(yazi)));
        printf("maske=%s ", inet_ntop(AF_INET, &maske, yazi, sizeof(yazi)));
        printf("gecit=%s kira=%us\n", inet_ntop(AF_INET, &gecit, yazi, sizeof(yazi)), kira);
        return 0;
    }
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (adres_uygula(s, teklif, maske, gecit) < 0) return 1;
    printf("%s ", inet_ntop(AF_INET, &teklif, yazi, sizeof(yazi)));
    printf("maske=%s ", inet_ntop(AF_INET, &maske, yazi, sizeof(yazi)));
    printf("gecit=%s kira=%us\n", inet_ntop(AF_INET, &gecit, yazi, sizeof(yazi)), kira);
    close(s);

    if (!yenile) return 0;
    /* Renew at half the lease, the way the RFC asks; if the answer stops coming
       the address simply stays until something else re-runs us. */
    for (;;) {
        sleep(kira / 2 ? kira / 2 : 1800);
        if (fork() == 0) {
            execl("/proc/self/exe", "dhcp", "-i", arayuz, (char *)NULL);
            _exit(1);
        }
    }
}
