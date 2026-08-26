/*
 * agci - talk to wpa_supplicant's control socket directly.
 *
 * Why this exists.  Android's wpa_supplicant does not connect on its own: it
 * registers with the HIDL layer and waits to be told which network to select.
 * The framework is what normally tells it, and the framework refuses to
 * auto-join below roughly -80 dBm on 2.4 GHz.  This handset sees its own AP at
 * -87 dBm, so the supplicant read the config, parsed the SSID, reached
 * DISCONNECTED and then sat there - "EAPOL: disable timer tick" and nothing
 * else.  No scan was ever requested.
 *
 * A node whose networking depends on the Android framework is not a server.
 * This speaks the control protocol itself, so wifi can be driven with the
 * framework stopped.
 *
 * Build:  arm-linux-gnueabihf-gcc -O2 -static -o agci agci.c
 * Usage:  agci [-i wlan0] <komut> [<komut> ...]
 *         agci SCAN SCAN_RESULTS
 *         agci LIST_NETWORKS ENABLE_NETWORK\ 0 SELECT_NETWORK\ 0 STATUS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

static int   s = -1;
static char  yerel[100];

static int ac(const char *yol)
{
    struct sockaddr_un ben, hedef;
    struct timeval zaman = { .tv_sec = 10, .tv_usec = 0 };

    s = socket(PF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return -1; }

    /* The control protocol is request/response over datagrams, so the client
       needs a bound address of its own for the reply to come back to. */
    snprintf(yerel, sizeof(yerel), "/data/local/tmp/agci-%d", (int)getpid());
    memset(&ben, 0, sizeof(ben));
    ben.sun_family = AF_UNIX;
    snprintf(ben.sun_path, sizeof(ben.sun_path), "%s", yerel);
    unlink(yerel);
    if (bind(s, (struct sockaddr *)&ben, sizeof(ben)) < 0) {
        perror("bind"); return -1;
    }

    memset(&hedef, 0, sizeof(hedef));
    hedef.sun_family = AF_UNIX;
    snprintf(hedef.sun_path, sizeof(hedef.sun_path), "%s", yol);
    if (connect(s, (struct sockaddr *)&hedef, sizeof(hedef)) < 0) {
        fprintf(stderr, "connect %s: %s\n", yol, strerror(errno));
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &zaman, sizeof(zaman));
    return 0;
}

static int gonder(const char *komut)
{
    char cevap[8192];
    ssize_t n;

    if (send(s, komut, strlen(komut), 0) < 0) {
        fprintf(stderr, "send(%s): %s\n", komut, strerror(errno));
        return -1;
    }
    printf("> %s\n", komut);
    for (;;) {
        n = recv(s, cevap, sizeof(cevap) - 1, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "  (zaman asimi)\n");
                return -1;
            }
            fprintf(stderr, "recv: %s\n", strerror(errno));
            return -1;
        }
        cevap[n] = 0;
        /* Unsolicited events start with '<'; they are not the answer to the
           command that was just sent, so keep reading past them. */
        if (cevap[0] == '<') { printf("  [olay] %s", cevap); continue; }
        printf("%s", cevap);
        if (n == 0 || cevap[n - 1] != '\n') printf("\n");
        return 0;
    }
}

int main(int argc, char **argv)
{
    char yol[104];
    const char *arayuz = "wlan0";
    const char *dizin  = "/data/vendor/wifi/wpa/sockets";
    int i = 1, rc = 0;

    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-i") && i + 1 < argc)      arayuz = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) dizin  = argv[++i];
        else { fprintf(stderr, "bilinmeyen secenek: %s\n", argv[i]); return 2; }
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "kullanim: agci [-i wlan0] [-p soket_dizini] <komut> ...\n");
        return 2;
    }

    snprintf(yol, sizeof(yol), "%s/%s", dizin, arayuz);
    if (ac(yol) < 0) { if (*yerel) unlink(yerel); return 1; }

    for (; i < argc; i++)
        if (gonder(argv[i]) < 0) { rc = 1; break; }

    close(s);
    unlink(yerel);
    return rc;
}
