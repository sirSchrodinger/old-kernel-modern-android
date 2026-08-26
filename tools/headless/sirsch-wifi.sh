#!/system/bin/sh
# Wifi without the Android framework, and without touching a settings screen.
#
# Why this exists at all.  Android's wpa_supplicant does not connect on its own:
# it registers with HIDL and waits to be told which network to select.  The
# framework is what normally tells it, and the framework refuses to auto-join
# below roughly -80 dBm on 2.4 GHz.  Association at that level works fine -
# measured on this handset at -90 dBm: 1-5.5 Mbit/s, 137 ms RTT - so the refusal
# is policy, not radio.  `agci` speaks the supplicant control protocol directly,
# which also means wifi survives with the framework stopped.
#
# Networks live in ONE plain file, /data/sirsch/aglar.conf, one per line:
#
#     ssid;parola                       WPA2-PSK
#     ssid;                             acik ag
#     ssid;kullanici;parola;eap         WPA2-Enterprise (PEAP/MSCHAPv2)
#
# Order is priority order: the first line is tried first.  Adding a network is
# editing a text file - no menu, no pull-down panel, no settings activity.

AGLAR=/data/sirsch/aglar.conf
KONF=/data/vendor/wifi/wpa/wpa_supplicant.conf
SOKET=/data/vendor/wifi/wpa/sockets
AGCI=/system/bin/agci
SUP=/vendor/bin/hw/wpa_supplicant
L=/data/sirsch/wifi.log

yaz() { echo "[$(cut -d' ' -f1 /proc/uptime)] $*" >> $L 2>/dev/null; }
bagli() { [ "$($AGCI STATUS 2>/dev/null | grep -c '^wpa_state=COMPLETED')" = 1 ]; }

[ -x "$AGCI" ] || { yaz "agci yok"; exit 1; }
bagli && exit 0

mkdir -p /data/sirsch "$SOKET" 2>/dev/null

# Seed the config from aglar.conf.  update_config=0 so the supplicant never
# rewrites this file: it is the human-editable source of truth.
if [ -f "$AGLAR" ]; then
    T=$KONF.uret
    {
        echo "ctrl_interface=$SOKET"
        echo "update_config=0"
        echo "ap_scan=1"
        n=0
        while IFS=';' read -r a b c d; do
            case "$a" in ''|'#'*) continue ;; esac
            echo "network={"
            echo "    ssid=\"$a\""
            echo "    scan_ssid=1"
            echo "    priority=$((100 - n))"
            if [ "$d" = "eap" ]; then
                echo "    key_mgmt=WPA-EAP"
                echo "    eap=PEAP"
                echo "    identity=\"$b\""
                echo "    password=\"$c\""
                echo "    phase2=\"auth=MSCHAPV2\""
            elif [ -n "$b" ]; then
                echo "    key_mgmt=WPA-PSK"
                echo "    psk=\"$b\""
            else
                echo "    key_mgmt=NONE"
            fi
            echo "}"
            n=$((n + 1))
        done < "$AGLAR"
    } > "$T" 2>/dev/null
    # Rename, never truncate in place: the supplicant may have this file open.
    mv "$T" "$KONF" 2>/dev/null
    yaz "aglar.conf'tan $n ag uretildi"
fi

# wpa_supplicant drops to user "wifi"; anything root creates here is unreadable
# to it, and the resulting log line is "Terminating..." - which reads like the
# supplicant gave up.  Supplicant::terminate() is a HIDL method: that message
# means somebody CALLED it.
chown wifi:wifi "$KONF" "$SOKET" 2>/dev/null
chmod 660 "$KONF" 2>/dev/null
chmod 770 "$SOKET" 2>/dev/null

ip link set wlan0 up 2>/dev/null
pgrep wpa_supplicant >/dev/null 2>&1 || {
    $SUP -Dnl80211 -iwlan0 -c"$KONF" -O"$SOKET" -B 2>/dev/null
    sleep 3
}

$AGCI SCAN >/dev/null 2>&1
sleep 6

# Enable every configured network and let the supplicant pick by priority and
# signal.  ENABLE_NETWORK all + RECONNECT is the framework-free equivalent of
# what WifiConnectivityManager would do.
$AGCI "ENABLE_NETWORK all" RECONNECT >/dev/null 2>&1
i=0
while [ $i -lt 12 ] && ! bagli; do sleep 3; i=$((i + 1)); done

if ! bagli; then
    yaz "baglanamadi: $($AGCI STATUS 2>/dev/null | grep '^wpa_state' | head -1)"
    exit 1
fi

SSID=$($AGCI STATUS 2>/dev/null | sed -n 's/^ssid=//p' | head -1)
yaz "bagli: $SSID"

# Address.  DHCP first when a client exists, static fallback so the node is
# still reachable on a network that has no lease for it.
ip addr show wlan0 2>/dev/null | grep -q "inet " && exit 0
if command -v dhcptool >/dev/null 2>&1; then
    dhcptool wlan0 >/dev/null 2>&1
    sleep 4
fi
if ! ip addr show wlan0 2>/dev/null | grep -q "inet "; then
    YEDEK=$(cat /data/sirsch/yedek-ip 2>/dev/null)
    [ -n "$YEDEK" ] && { ip addr add "$YEDEK" dev wlan0 2>/dev/null; yaz "statik: $YEDEK"; }
fi
# netd swaps the main-table rule for fwmark rules plus a catch-all unreachable,
# so a route added by hand is otherwise never consulted.
ip rule show 2>/dev/null | grep -q "lookup main" || \
    ip rule add priority 99 from all lookup main 2>/dev/null
exit 0
