#!/usr/bin/env python3
"""Run commands on the handset over the gadget's CDC-ACM console.

Holds the port open for the whole session: closing it hangs up the phone-side
shell, which is why earlier attempts died after one or two queries.  Writes in
32-byte chunks with a pause - this tty drops characters when written fast.
"""
import fcntl, os, select, sys, termios, time

import glob


# Bu tty'yi ayni anda iki okuyucu paylasamaz: nobetci saniyede bir yokluyor,
# elle atilan bir sorgu araya girdiginde ikisinin yazdiklari birbirine giriyor
# ve iki taraf da bozuk cevap aliyor ("konsol cevap vermiyor" satirlarinin bir
# kismi bu).  Butun erisimler tek bir dosya kilidinden geciyor.
KILIT = "/tmp/golden-konsol.kilit"


def kilit_al(saniye=180):
    f = open(KILIT, "w")
    son = time.time() + saniye
    while time.time() < son:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return f
        except BlockingIOError:
            time.sleep(0.5)
    return None


def _port():
    """The gadget does not always come back as ttyACM0.

    After a gadget reset on 25 August the console reappeared as ttyACM1 while
    this script kept opening ttyACM0 and reported "kabuk alinamadi" - the
    handset was fine and only the name had moved.  Take the highest-numbered
    one, which is the most recently created.
    """
    l = sorted(glob.glob("/dev/ttyACM*"))
    return l[-1] if l else "/dev/ttyACM0"


PORT = _port()
MARK = "__son__"


class Konsol:
    def __init__(self, fd): self.fd = fd

    def w(self, b, s=20):
        b = b.encode() if isinstance(b, str) else b
        son = time.time() + s
        while b and time.time() < son:
            try:
                n = os.write(self.fd, b[:32]); b = b[n:]; time.sleep(0.04)
            except BlockingIOError:
                select.select([], [self.fd], [], 0.2)
            except OSError:
                return False
        return not b

    def r(self, s, dur=None):
        son = time.time() + s; v = b""
        while time.time() < son:
            rr, _, _ = select.select([self.fd], [], [], 0.3)
            if rr:
                try: p = os.read(self.fd, 65536)
                except BlockingIOError: continue
                except OSError: break
                if p:
                    v += p
                    if dur and dur.encode() in v: break
        return v.decode("utf-8", "replace")

    def komut(self, k, s=30):
        if not self.w(k + f"\necho {MARK}\n"): return None
        o = self.r(s, dur=MARK).replace("\r", "")
        return "\n".join(l for l in o.split("\n") if MARK not in l and l.strip())


def ac(deneme=8):
    for _ in range(deneme):
        PORT = _port()
        if not os.path.exists(PORT):
            time.sleep(2); continue
        try:
            fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError:
            time.sleep(2); continue
        a = termios.tcgetattr(fd)
        a[0] = 0; a[1] = 0; a[3] = 0
        a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, a)
        k = Konsol(fd)
        for _ in range(6):
            k.r(0.3); k.w("\n")
            if "#" in k.r(1.2): 
                k.w("stty -echo -iuclc 2>/dev/null\n"); k.r(0.6)
                return fd, k
            time.sleep(0.8)
        os.close(fd); time.sleep(2)
    return None, None


if __name__ == "__main__":
    _kf = kilit_al()
    if _kf is None:
        print("konsol kilidi alinamadi (baska bir okuyucu tutuyor)", file=sys.stderr)
        sys.exit(2)
    try:
        fd, k = ac()
        if not k:
            print("kabuk alinamadi", file=sys.stderr); sys.exit(1)
        try:
            for komut in sys.argv[1:]:
                print(f"--- {komut}", flush=True)
                print(k.komut(komut), flush=True)
        finally:
            os.close(fd)
    finally:
        fcntl.flock(_kf, fcntl.LOCK_UN); _kf.close()
