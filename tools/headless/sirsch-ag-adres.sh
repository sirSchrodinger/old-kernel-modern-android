#!/system/bin/sh
# Arayuze adres ve varsayilan rota kazandirir.
#
# NEDEN AYRI BIR IS
#
# Cerceve yok, yani DhcpClient de yok: wpa_supplicant baglaniyor ama arayuz
# adressiz kaliyor.  Bu is olmadan telefon aga BAGLI ama ULASILAMAZ - 26
# Agustos'ta tam olarak bu oldu, ekranda "wifi ok" yaziyordu ve hicbir sey
# cevap vermiyordu.
#
# NEDEN FLUSH YOK
#
# Ayni gun `ip addr flush dev wlan0` ile telefon tamamen kayboldu: flush yalniz
# adresi degil, ona bagli her seyi birakti ve geri donus yolu kalmadi.  Burada
# adres EKLENIR, hicbir sey silinmez.  Link-local adres (169.254.x) bilerek
# duruyor: aynı L2 uzerindeki bir makine, DHCP calismasa bile ona ulasabilir.

D=/data/sirsch
ARAYUZ=${1:-wlan0}
DHCP=/system/bin/sirsch-dhcp
[ -x "$DHCP" ] || DHCP=/data/local/tmp/dhcp
[ -x "$DHCP" ] || exit 1

# Arayuz ayakta degilse yapacak bir sey yok - wifi isi baska betigin.
[ "$(cat /sys/class/net/$ARAYUZ/operstate 2>/dev/null)" = "up" ] || exit 0

# Zaten yonlendirilebilir bir adres ve varsayilan rota varsa dokunma.
ADRES=$(ip -4 addr show "$ARAYUZ" 2>/dev/null | awk '$1=="inet"{print $2}' | grep -v '^169\.254\.' | head -1)
ROTA=$(ip route 2>/dev/null | grep -c '^default')
if [ -n "$ADRES" ] && [ "$ROTA" -gt 0 ]; then
    echo "$ADRES" > "$D/adres"
    echo "$ADRES, rota var"
    exit 0
fi

# Kirayi al ama arayuzu DHCP istemcisine biraktirma: uygulamayi burada,
# gorulebilir ve geri alinabilir sekilde yapiyoruz.
CIKTI=$("$DHCP" -i "$ARAYUZ" -n -t 20 2>&1 | tail -1)
case "$CIKTI" in
    *"DENEME"*) : ;;
    *) echo "kira alinamadi: $CIKTI" >&2; exit 1 ;;
esac
IP=$(echo "$CIKTI" | awk '{print $3}')
MASKE=$(echo "$CIKTI" | sed -n 's/.*maske=\([0-9.]*\).*/\1/p')
GECIT=$(echo "$CIKTI" | sed -n 's/.*gecit=\([0-9.]*\).*/\1/p')
[ -n "$IP" ] && [ -n "$GECIT" ] || { echo "kira ayristirilamadi: $CIKTI" >&2; exit 1; }

# Maskeyi onek uzunluguna cevir (ip komutu /24 bekliyor).
ONEK=$(echo "$MASKE" | awk -F. '{n=0; for(i=1;i<=4;i++){v=$i; while(v>0){n+=v%2; v=int(v/2)}} print n}')
[ "$ONEK" -ge 8 ] 2>/dev/null || ONEK=24

ip addr add "$IP/$ONEK" dev "$ARAYUZ" 2>/dev/null
ip route add default via "$GECIT" dev "$ARAYUZ" 2>/dev/null
echo "$IP/$ONEK" > "$D/adres"
echo "adres verildi: $IP/$ONEK gecit=$GECIT"
exit 0
