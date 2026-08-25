#!/bin/bash
# golden nobetcisi - telefonu kendi basina ilerleten dongu.
#
# Runs without waiting for a human hand: telefonu USB'de gorur gormez ne durumda
# oldugunu anlar, recovery'ye dustuyse hazirdaki duzeltmeleri /system'e yazar ve
# ROM'a geri gonderir, ROM'da ise seri konsoldan boot'u izler.  Tek isteyen
# tarafi sarj: bu kablo telefonu sarj etmiyor (AB8500 USB_CHARGER_NOT_OK'i
# donanimda tekrar tekrar bildiriyor, chg_current_adc=0), o yuzden pil esigin
# altindaysa dongu boot denemez - kapatir ki duvar sarjinda gercekten dolsun.
#
# Durumlar USB kimliginden okunur, tahminle degil:
#   04e8:685d  recovery  -> adb calisir
#   04e8:6860  ROM       -> adb calismaz, /dev/ttyACM* konsolu calisir
#   yok        kapali    -> yapacak sey yok, sarj oluyordur
#
# adb'yi ROM'a KESINLIKLE sokma: ROM'un gadget'i cevaplanamayan bir ADB arayuzu
# gosteriyor (adbd yalniz FunctionFS konusur, bu cekirdekte yok), her `adb
# devices` bir USB reset uretiyor ve port akimi 100 mA'e dusuyor.
#
# Frenler:
#   nobet/DUR   dosya varsa hicbir sey yapmaz, sadece durum yazar
#   --dry-run   ne yapacagini yazar, telefona dokunmaz
set -uo pipefail
KOK=${GOLDEN_KOK:-$HOME/rom-golden}
N=$KOK/nobet
SAHNE=$KOK/sahne
# /data altina gidecekler ayri agacta: sahne/ -> /system, sahne-data/ -> /data.
# Cekirdek degistiginde yeni boot.img buraya konur; recovery turu once onu
# /data/sirsch/boot.img'e yazar, sonra oradan p20'ye dd eder.
SAHNE_DATA=$KOK/sahne-data
mkdir -p "$N"
DURUM=$N/durum.txt
GUNLUK=$N/nobetci.log
KURU=0
[ "${1:-}" = "--dry-run" ] && KURU=1

# Boot denemesi icin gereken en dusuk pil.  25 Agustos'ta %14'te yeniden
# baslatma pilin altini cekti ve telefon USB'den komple kayboldu; daha once de
# %27, %20 ve %18'de boot yarida dondu.  Recovery de sarj etmiyor, o yuzden
# esigin altinda tek dogru hamle KAPATMAK.
PIL_ESIK=40

yaz() { echo "$(date +%H:%M:%S) $*" | tee -a "$GUNLUK"; }

# Sahnedeki her dosyanin md5'inden tek bir imza.  ROM ayaga kalktiginda
# /system/etc/sirsch-surum.txt bununla eslesmiyorsa telefonda eski dosyalar
# var demektir; o boot'u izlemenin anlami yok, dogrudan recovery'ye gonderilir.
surum() { local a; a=$(find "$SAHNE" "$SAHNE_DATA" -type f -print0 2>/dev/null | sort -z | xargs -0 md5sum); \
          echo "$(echo "$a" | wc -l)-$(echo "$a" | md5sum | cut -c1-12)"; }

# Ayni durumu ust uste bildirme; WA'da dakikada 5 / saatte 30 siniri var ve
# zaten tekrar eden bir satir bilgi tasimiyor.
son_bildirim=""
bildir() {
  [ "$1" = "$son_bildirim" ] && return 0
  son_bildirim="$1"
  [ "$KURU" = 1 ] && { yaz "[kuru] WA: $2"; return 0; }
  ${BILDIR_KOMUTU:-true} "$2" >/dev/null 2>&1 || yaz "WA gonderilemedi"
}

# Ucuncu bir kimlik daha var ve bunu ogrenmek pahaliya mal oldu:
# framework ayaga kalkinca UsbDeviceManager gadget'i KENDI yeniden
# yapilandiriyor ve VID/PID Samsung'un 04e8'inden AOSP'nin 18d1:4ee7'sine
# geciyor.  O anda CDC-ACM konsolu da rndis de kayboluyor - yani boot'un
# BASARILI oldugu anda telefona giden butun kanallar kapaniyor.  Eski kod bunu
# "telefon yok" sayiyordu, oysa telefon calisiyordu.
#   04e8:685d  recovery          -> adb calisir
#   04e8:6860  ROM, init gadget  -> seri konsol calisir
#   18d1:4ee7  ROM, framework up -> ikisi de yok; adbd USB'yi sunamiyor
#                                   (bu cekirdekte CONFIG_USB_FUNCTIONFS yok)
usb_kimlik() {
  local f v p
  for f in /sys/bus/usb/devices/*/idVendor; do
    [ -r "$f" ] || continue
    v=$(cat "$f" 2>/dev/null)
    p=$(cat "$(dirname "$f")/idProduct" 2>/dev/null)
    case "$v:$p" in
      04e8:685d) echo "685d"; return 0;;
      04e8:6860) echo "6860"; return 0;;
      18d1:*)    echo "cerceve"; return 0;;
    esac
  done
  echo ""
}

# --- recovery tarafi -------------------------------------------------------
recovery_turu() {
  local d A T pil
  d=$(adb devices 2>/dev/null | awk 'NR>1 && $2=="recovery" {print $1; exit}')
  [ -n "$d" ] || { yaz "recovery USB'de ama adb cevap vermiyor"; return 1; }
  A="adb -s $d"
  T="$N/tur-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$T"

  # /cache elle baglanmali: recovery onu bagli getirmiyor ve baglanmamis
  # /cache'ten cekilen her dosya "yok" donuyor - bu bir kez butun teshisi
  # yanlis yola soktu.
  $A shell </dev/null 'busybox mountpoint -q /cache || busybox mount -t ext4 /dev/block/mmcblk0p23 /cache 2>/dev/null
            busybox mountpoint -q /data  || busybox mount -t ext4 /dev/block/mmcblk0p25 /data  2>/dev/null' >/dev/null 2>&1
  $A shell </dev/null 'busybox cat /proc/last_kmsg' > "$T/last_kmsg.txt" 2>&1
  for f in sirsch-tur.log sirsch-konsol.log sirsch-logcat.txt sirsch-dmesg.txt sirsch-crash.txt; do
    $A pull "/cache/$f" "$T/$f" >/dev/null 2>&1
  done
  $A shell </dev/null 'busybox ls -t /data/tombstones/tombstone_* 2>/dev/null | busybox head -4' 2>/dev/null | tr -d '\r' | while read -r t; do
    [ -n "$t" ] && $A pull "$t" "$T/$(basename "$t").txt" >/dev/null 2>&1
  done
  pil=$($A shell </dev/null 'busybox cat /sys/class/power_supply/battery/capacity' 2>/dev/null | tr -d '\r ')
  echo "${pil:-?}" > "$T/pil.txt"
  yaz "recovery turu -> $T (pil=%${pil:-?})"

  if [ -f "$N/DUR" ]; then yaz "DUR var, recovery'de birakiliyor"; return 0; fi

  if [ -n "$pil" ] && [ "$pil" -lt "$PIL_ESIK" ] 2>/dev/null; then
    # ARTIK KAPATMIYORUZ.  25 Agustos: guc tusu fiziksel olarak sokuldu.
    # Kapali bir telefonu geri acmanin tek yolu guc tusuydu; sarj kablosu
    # takilinca healthd -c ile "charger" kipine giriyor ve o kip de yalnizca
    # KEY_POWER'i dinliyor.  Yani `reboot -p` bu telefonu geri acilamaz halde
    # birakirdi - dongunun kendisi cihazi kaybederdi.
    #
    # Bunun yerine recovery'de park ediyoruz: orada yuk dusuk, adb acik ve
    # telefon her an geri gonderilebilir.
    bildir "pil_dusuk" "golden: pil %$pil, esigin altinda. Recovery'de park edildi - boot denenmiyor. Duvar sarjina tak."
    yaz "pil %$pil < %$PIL_ESIK -> recovery'de park (KAPATILMIYOR: guc tusu yok)"
    [ "$KURU" = 1 ] && { yaz "[kuru] recovery'de park"; return 0; }
    $A shell </dev/null 'busybox sync' >/dev/null 2>&1
    sleep 60
    return 0
  fi

  # hazirdaki duzeltmeleri yaz
  if [ "$KURU" = 1 ]; then
    yaz "[kuru] $SAHNE icerigi /system'e yazilacakti:"
    find "$SAHNE" -type f | sed "s|$SAHNE/system|  /system|"
    yaz "[kuru] sonra boot.img p20'ye geri yazilip yeniden baslatilacakti"
    return 0
  fi
  $A shell </dev/null 'busybox mountpoint -q /system || busybox mount -t ext4 /dev/block/mmcblk0p22 /system' >/dev/null 2>&1
  # sahne/system/... -> cihazda /system/system/...  (p22 sistem-kok duzeninde,
  # bolumun kokunde "system/" klasoru var; TWRP onu /system'e baglar)
  #
  # Liste ONCE diziye okunur ve her adb cagrisi stdin'i /dev/null'dan alir.
  # `while read ... done < <(find ...)` dongusunun icinde `adb shell` cagirmak
  # sessizce yanlis calisiyordu: adb shell stdin'i tuketiyor, yani find
  # ciktisinin geri kalanini yiyor.  25 Agustos 21:01 turunda dokuz dosyadan
  # yalnizca biri yazildi ve dongu "1/1 dogrulandi" diyerek basarili gorundu -
  # bu, olcum aracinin kendisinin arizayi gizlemesi.
  local -a dosyalar=()
  while IFS= read -r f; do dosyalar+=("$f"); done < <(find "$SAHNE" -type f | sort)
  local n=0 t=${#dosyalar[@]}
  local f
  for f in "${dosyalar[@]}"; do
    local ic="${f#$SAHNE}"          # /system/lib/x.so
    local hedef="/system$ic"        # /system/system/lib/x.so
    $A shell "busybox mkdir -p $(dirname "$hedef")" </dev/null >/dev/null 2>&1
    $A push "$f" "/tmp/nb.bin" </dev/null >/dev/null 2>&1 || { yaz "push basarisiz: $ic"; continue; }
    $A shell "busybox cp /tmp/nb.bin $hedef && busybox chmod 644 $hedef && busybox chown 0:0 $hedef" </dev/null >/dev/null 2>&1
    local uzak yerel
    uzak=$($A shell "busybox md5sum $hedef" </dev/null 2>/dev/null | tr -d '\r' | cut -c1-32)
    yerel=$(md5sum "$f" | cut -c1-32)
    if [ "$uzak" = "$yerel" ]; then n=$((n+1)); else yaz "MD5 TUTMADI: $ic ($uzak != $yerel)"; fi
  done
  yaz "$n/$t dosya /system'e yazildi ve md5 dogrulandi"
  if [ "$t" -lt 2 ]; then
    yaz "sahnede $t dosya gorundu - beklenen bu degil, DUR konuyor"
    touch "$N/DUR"; return 1
  fi
  if [ "$n" -ne "$t" ]; then
    # Yarim yazilmis bir /system ile yeniden baslatmak, hic yazmamaktan kotu:
    # telefon alti dakika kaybolur ve geri dondugunde hangi dosyanin eski
    # hangisinin yeni oldugu bilinmez, olculen hicbir sey anlam tasimaz.
    yaz "eksik yazim -> DUR konuyor, yeniden baslatilmiyor"
    touch "$N/DUR"
    bildir "yazim_hatasi" "golden: /system yazimi eksik kaldi ($n/$t). Dongu durduruldu."
    return 1
  fi
  $A shell </dev/null "busybox sh -c 'echo $(surum) > /system/system/etc/sirsch-surum.txt'" >/dev/null 2>&1

  # /data agaci (varsa).  Cekirdek degistiyse yeni boot.img burada; p20'ye
  # yazilmadan ONCE /data/sirsch/boot.img guncellenmeli, cunku dd oradan okuyor.
  if [ -d "$SAHNE_DATA" ]; then
    local dn=0 dt=0
    local -a ddosyalar=()
    while IFS= read -r f; do ddosyalar+=("$f"); done < <(find "$SAHNE_DATA" -type f | sort)
    dt=${#ddosyalar[@]}
    for f in "${ddosyalar[@]}"; do
      local dhedef="/data${f#$SAHNE_DATA}"
      $A shell "busybox mkdir -p $(dirname "$dhedef")" </dev/null >/dev/null 2>&1
      $A push "$f" "$dhedef" </dev/null >/dev/null 2>&1 || { yaz "push basarisiz: $dhedef"; continue; }
      local du dy
      du=$($A shell "busybox md5sum $dhedef" </dev/null 2>/dev/null | tr -d '\r' | cut -c1-32)
      dy=$(md5sum "$f" | cut -c1-32)
      if [ "$du" = "$dy" ]; then dn=$((dn+1)); else yaz "MD5 TUTMADI: $dhedef"; fi
    done
    yaz "$dn/$dt dosya /data'ya yazildi ve md5 dogrulandi"
    if [ "$dn" -ne "$dt" ]; then
      yaz "/data yazimi eksik -> DUR"; touch "$N/DUR"; return 1
    fi
  fi

  # boot.img'i geri yaz: recovery'ye p21->p20 dd ile gelindi, geri donus p20'ye
  # /data/sirsch/boot.img yazmak.
  $A shell </dev/null 'busybox dd if=/data/sirsch/boot.img of=/dev/block/mmcblk0p20 bs=4096 2>/dev/null; busybox sync
            busybox dd if=/dev/block/mmcblk0p20 bs=4096 count=4096 2>/dev/null | busybox md5sum' 2>/dev/null | tr -d '\r' | sed 's/^/  p20: /' | tee -a "$GUNLUK"
  # /cache/tam-boot olmadan sirsch-kalkan.sh kalkan modunda aciliyor ve
  # framework'u basligi gibi durduruyor - o boot'u izlemenin anlami yok.
  # Dosyayi kalkan betigi bir kez okuyup siliyor, yani her tur yeniden konmali.
  $A shell </dev/null 'busybox rm -f /cache/sirsch-* ; busybox touch /cache/kal /cache/tam-boot ; busybox sync' >/dev/null 2>&1
  yaz "ROM'a geri gonderiliyor"
  $A reboot >/dev/null 2>&1
  adb kill-server >/dev/null 2>&1
  sleep 30
}

# --- ROM tarafi ------------------------------------------------------------
rom_turu() {
  local o
  o=$(timeout 100 python3 "$KOK/konsol-sor.py" \
        'echo "UP=$(cut -d" " -f1 /proc/uptime)"; echo "BOOT=$(getprop sys.boot_completed)"; echo "PIL=$(cat /sys/class/power_supply/battery/capacity)"; echo "AKIM=$(cat /sys/class/power_supply/battery/current_now)"; echo "MOD=$(getprop sirsch.mod)"; echo "SS=$(pidof system_server)"; echo "ZY=$(pidof zygote)"' 2>/dev/null \
      | grep -E '^(UP|BOOT|PIL|AKIM|MOD|SS|ZY)=')
  [ -n "$o" ] || { yaz "ROM'da ama konsol cevap vermiyor"; return 1; }
  echo "$o" | tr '\n' ' ' | sed 's/$/\n/' | tee -a "$GUNLUK" >/dev/null
  yaz "ROM: $(echo "$o" | tr '\n' ' ')"
  echo "$o" > "$DURUM"
  # Surum kontrolu ROM oturumu basina BIR kez.  Her turda sormak konsolu
  # gereksiz mesgul ediyor ve bu tty hizli yazimda karakter dusuruyor.
  #
  # Esik 90 -> 25: framework gadget'i ~85-100. saniyede devraliyor ve o anda
  # konsol oluyor.  90'da sormak, pencereyi kacirmak demekti.
  # Ve boot'un ilk saniyelerinde hic sorulmaz: 21:04'te UP=36'da sorulan soru
  # 'n_' donduruldu - konsolun kendi yankisindan kopmus bir parca - ve dongu
  # bunu "surum farkli" sayip calisan bir boot'u recovery'ye atti.  OKUNAMAYAN
  # DEGER, FARKLI DEGER DEGILDIR.  O yuzden cevap SURUM= ile isaretleniyor;
  # isaretli satir gelmezse hicbir sey yapilmaz.
  local up
  up=$(echo "$o" | sed -n 's/^UP=\([0-9]*\).*/\1/p')
  if [ -z "${surum_bakildi:-}" ] && [ -n "$up" ] && [ "$up" -ge 25 ] 2>/dev/null; then
    local ham
    ham=$(timeout 60 python3 "$KOK/konsol-sor.py" 'echo "SURUM=$(cat /system/etc/sirsch-surum.txt 2>/dev/null)"' 2>/dev/null \
          | grep -o 'SURUM=[0-9a-f-]*' | tail -1)
    if [ -n "$ham" ]; then
      surum_bakildi=1
      uzak_surum=${ham#SURUM=}
      yaz "telefondaki /system surumu: '${uzak_surum:-(bos)}'"
    else
      yaz "surum okunamadi (konsol bozuk cevap verdi) - dokunulmuyor"
    fi
  fi
  if [ -n "${surum_bakildi:-}" ] && [ -n "${uzak_surum:-}" ] && [ "$uzak_surum" != "$(surum)" ]; then
    yaz "telefondaki /system surumu '$uzak_surum' != sahne '$(surum)' -> recovery'ye gonderiliyor"
    [ "$KURU" = 1 ] && { yaz "[kuru] sirsch-recovery.sh calistirilacakti"; return 0; }
    timeout 60 python3 "$KOK/konsol-sor.py" 'nohup /system/bin/sirsch-recovery.sh > /cache/rec.log 2>&1 &' >/dev/null 2>&1
    sleep 45
    return 0
  fi
  if echo "$o" | grep -q '^BOOT=1'; then
    bildir "boot_tamam" "golden: BOOT TAMAMLANDI. sys.boot_completed=1"
    yaz "*** BOOT COMPLETED ***"
    touch "$N/TAMAM"
    return 0
  fi
  # Boot suruyorken konsolu 15 saniyede bir yoklamak gereksiz: bu tty hizli
  # yazimda karakter dusuruyor ve olculen sey zaten dakikalar olceginde
  # degisiyor.  Yaklasik dakikada bir yeter.
  sleep 45
}

yaz "=== nobetci basladi (kuru=$KURU) ==="
onceki=""
while :; do
  [ -f "$N/TAMAM" ] && { yaz "TAMAM var, dongu duruyor"; break; }
  k=$(usb_kimlik)
  [ "$k" != "$onceki" ] && { yaz "durum degisti: '${onceki:-yok}' -> '${k:-yok}'"; onceki="$k"; }
  case "$k" in
    685d) surum_bakildi=""; recovery_turu ;;
    6860) rom_turu ;;
    cerceve)
      # Framework gadget'i devraldi: boot buraya kadar geldi.  Kanal yok, ama
      # bu bir basari sinyali - donguyu durdur ki kimse bu telefonu recovery'ye
      # atmasin.
      if [ ! -f "$N/CERCEVE" ]; then
        touch "$N/CERCEVE"
        yaz "*** framework gadget'i devraldi (18d1) - boot UsbDeviceManager'a kadar geldi ***"
        bildir "cerceve" "golden: framework ayaga kalkti, USB kimligi 18d1'e gecti. Telefonun ekranina bak."
      fi
      ;;
    *)    : ;;
  esac
  [ "$KURU" = 1 ] && { yaz "[kuru] tek tur bitti"; break; }
  sleep 15
done
