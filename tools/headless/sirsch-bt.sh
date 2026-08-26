#!/system/bin/sh
# Bluetooth: ac ve tara.
#
# Cerceve yok, yani Android'in bluetooth yigini da yok.  Elde kalan: rfkill ile
# radyoyu birakmak ve hciattach ile HCI aygitini kurmak.  rfkill'in state=1'i
# "acik" DEGIL "engellenmemis" demek - gercek olcut /sys/class/bluetooth altinda
# bir aygitin var olmasi (26 Agu: ekran "bt on" yaziyordu, hic aygit yoktu).
CIKTI=/data/sirsch/bt-taranan.txt
SONUC=/data/sirsch/ag-sonuc
yaz() { echo "$*" > "$SONUC"; }

hci_var() { [ "$(ls /sys/class/bluetooth 2>/dev/null | wc -l)" -gt 0 ]; }

# HER DONANIM TEMASI `timeout` ICINDE.
#
# 27 Agustos: rfkill'e yazan bu betik cekirdekte `sysfs_addrm_finish` uzerinde
# KESINTISIZ (D) duruma dustu.  SIGKILL bile almadi.  Ve ekranin catalladigi
# bir cocuk oldugu icin ekranin kilit tanticisini devralmisti - panel bir daha
# hic acilmadi.  Kilit tarafi CLOEXEC ile kapatildi; burasi da takilmamali.
ac() {
    hci_var && return 0
    for d in /sys/class/rfkill/rfkill*; do
        [ "$(timeout 2 cat $d/type 2>/dev/null)" = "bluetooth" ] || continue
        timeout 3 sh -c "echo 0 > $d/soft" 2>/dev/null
    done
    # hciattach: vendor blob'u varsa HCI aygitini kurar.
    for h in /system/bin/hciattach /vendor/bin/hciattach; do
        [ -x "$h" ] || continue
        timeout 12 "$h" -n /dev/ttyAMA1 bcm43xx 3000000 flow -t 20 >/dev/null 2>&1 &
        sleep 4
        break
    done
    hci_var
}

case "${1:-durum}" in
  ac)
    ac && yaz "bt acildi" || yaz "bt acilamadi"
    ;;
  tara)
    if ! ac; then
        yaz "bt yok (hciattach/blob eksik)"
        : > "$CIKTI"
        exit 1
    fi
    command -v hcitool >/dev/null 2>&1 || { yaz "hcitool yok"; exit 1; }
    timeout 5 hciconfig hci0 up >/dev/null 2>&1
    yaz "bt taraniyor"
    timeout 25 hcitool scan 2>/dev/null | awk 'NR > 1 && NF >= 2 { $1=$1; ad=$0; sub(/^[^ \t]+[ \t]+/, "", ad); printf "0\t%s\tbt\n", ad }' > "$CIKTI.yeni"
    mv "$CIKTI.yeni" "$CIKTI" 2>/dev/null
    yaz "bt: $(wc -l < "$CIKTI" 2>/dev/null) aygit"
    ;;
  *)
    hci_var && echo on || echo off
    ;;
esac
exit 0
