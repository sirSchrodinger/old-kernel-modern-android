#!/system/bin/sh
# The handset's own way back in, so that recovery stops being a person's job.
#
# Reaching TWRP needs a human on Volume-Up+Home+Power: `reboot recovery` writes
# its reason into the AB8500 RTC alarm registers and the bootloader ignores it,
# and the other channel - sec_set_param_value - writes to /mnt/.lfs/param.blk,
# a stock-ROM path that does not exist here.  So the ROM has to be able to hold
# itself open instead.
#
# Shield mode is that: no graphics, no framework, nothing that has been seen to
# crash - only a serial console, USB ethernet and adbd on TCP.  Everything
# recovery is used for (remount /system rw, dd the boot partition, rewrite an
# init script) works from here, and unlike recovery it comes back by itself
# after every reboot.
#
# One full boot is requested by creating /cache/tam-boot.  It is consumed
# immediately, so a full boot that dies returns to shield mode on its own
# rather than to a key combo.

L=/cache/sirsch-kalkan.log
[ -d /cache ] || L=/dev/sirsch-kalkan.log
: > $L
yaz() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $L; }

yaz "=== kalkan basladi ==="

# 26 Agustos: VARSAYILAN TERSINE CEVRILDI.
#
# Eskiden varsayilan kalkandi ve tam boot icin /cache/tam-boot dosyasi gerekiyordu.
# O, framework'un hic ayakta duramadigi 24 Agustos'un mantigiydi.  Artik telefon
# boot ediyor, yani kalkan istisna olmali, kural degil.
#
# Eski hali bir tuzak da kuruyordu: dosya bir kez okunup siliniyor, yani bayragi
# koymayi atlayan HER tur sessizce kalkana dusuyor.  26 Agustos'ta tam bu oldu -
# nobetci'nin bayrak adimi calismadi, telefon beyaz ekranla acildi ve sebebi
# gorunmuyordu.  Daha kotusu: kalkan "stop" ettigi servisi init'e DISABLED
# isaretletiyor, dolayisiyla sonradan "setprop sirsch.mod tam" demek yetmiyor -
# audioserver, media, ril-daemon disabled kaliyor, sistem yarim kaliyor ve
# boot_completed hic gelmiyor.  Yani yanlis varsayilan geri alinamaz bir boot
# uretiyordu.
#
# Kalkan artik acikca istenmeli: /cache/kalkan dosyasi.  Silinmiyor - istendigi
# surece kalsin, birakmak icin dosyayi kaldirmak yeter.
# 26 Agustos aksami: VARSAYILAN BASSIZ.
#
# Olculdu, tahmin degil: cerceve acikken 825 MB'in 24-50 MB'i bos kaliyor, 814
# surec kosuyor ve cihaz gorunur sekilde kasiyor; ustelik bu turda
# sys.boot_completed hic gelmedi - "phone is starting" ekranda asili kaldi,
# menu bos gorundu ve frekans 800 MHz'de takildi cunku onu 1 GHz'e acan rc
# satiri boot_completed'a bagli.  Cerceve durdurulunca 510 MB bos, yuk ~0.
#
# Bu cihaz bir telefon olarak degil, bir agda durup is yapan kutu olarak
# kullanilacak; o is icin cerceve saf yuk.  Ekrani `ekran` ciziyor, agi
# `sirsch-wifi.sh` kuruyor, ikisi de framework'e bagli degil.
#
# Cerceve geri isteniyorsa: /cache/cerceve dosyasini olustur ve yeniden baslat.
# Dosya SILINMIYOR - istendigi surece kalsin.  Reboot sart, cunku `stop` edilen
# servisi init DISABLED isaretliyor ve sonradan class_start onu atliyor.
MOD=bassiz
[ -f /cache/cerceve ] && MOD=tam
[ -f /cache/kalkan ] && MOD=kalkan
# Gecis: eski bayragi hala koyan bir arac varsa onu da onurlandir.
if [ -f /cache/tam-boot ]; then
    rm -f /cache/tam-boot
    sync
    MOD=tam
fi
yaz "mod=$MOD"
setprop sirsch.mod "$MOD"

# The framework side.  Stopping a service also marks it disabled, so a later
# class_start skips it - which is why this runs from "on post-fs", before
# anything has started, instead of racing the crash loop afterwards.
KAPAT="surfaceflinger bootanim zygote zygote_secondary audioserver cameraserver
       media mediadrm mediaextractor mediametrics drm ril-daemon vendor.ril-daemon
       iorapd incidentd statsd traced traced_probes gpu blank_screen
       sirsch_izle"

sustur() {
    for s in $KAPAT; do stop $s 2>/dev/null; done
}

# The screen costs about as much as this cable manages to deliver, and nobody
# is looking at a handset that is deliberately not drawing anything.
karart() {
    for b in /sys/class/backlight/*/brightness /sys/class/leds/lcd-backlight/brightness; do
        [ -w "$b" ] && echo 0 > "$b" 2>/dev/null
    done
}

# f_rndis is entirely in-kernel, so the interface exists as soon as the gadget
# binds - no daemon, no association.  The name is usb0 on most kernels and
# rndis0 on some, so do not hard-code either.
ag() {
    for i in usb0 rndis0 eth0; do
        [ -e /sys/class/net/$i ] || continue
        ip link set $i up 2>/dev/null
        # netd deletes the "from all lookup main" rule and replaces it with
        # fwmark rules plus a catch-all unreachable, so a route added by hand
        # into the main table is never consulted: ARP answers (that is layer 2)
        # but every IP reply is dropped, which reads exactly like a dead link.
        # Measured - ARP said REACHABLE with the right MAC while ping and TCP
        # both timed out.
        ip rule show 2>/dev/null | grep -q "lookup main" || {
            ip rule add priority 99 from all lookup main 2>/dev/null
            yaz "ip rule: lookup main geri konuldu"
        }
        ip addr show $i 2>/dev/null | grep -q 192.168.42.2 && return 0
        ip addr add 192.168.42.2/24 dev $i 2>/dev/null
        yaz "ag: $i -> 192.168.42.2/24"
        return 0
    done
    return 1
}

# adbd over TCP needs no FunctionFS, which is the whole reason USB adb is
# impossible on this kernel.  ro.adb.secure=1 here, so the host key has to be
# in place as well - it is installed next to this script.
adb_tcp() {
    [ "$(getprop service.adb.tcp.port)" = "5555" ] || {
        setprop service.adb.tcp.port 5555
        stop adbd 2>/dev/null
        start adbd 2>/dev/null
        yaz "adbd tcp 5555 baslatildi"
    }
    [ "$(getprop init.svc.adbd)" = "running" ] || start adbd 2>/dev/null
}

# Kilit ekrani: ROM'da hala cikiyor.
#
# device/.../SettingsProvider defaults.xml'de def_lockscreen_disabled=true
# yapilmisti ama o deger yalnizca /data ILK kurulurken okunuyor - cihazda
# /data coktan var, dolayisiyla ayar hicbir zaman uygulanmadi.  26 Agustos:
# Alperen ekranda "swipe to unlock" gordu.
#
# Bu yuzden calisma aninda ayarlaniyor, ve yalnizca BIR KEZ: /data'ya bayrak
# birakiliyor, sonraki boot'larda tekrar denenmiyor.  Framework ayaga kalkmadan
# settings/locksettings komutlari calismaz, o yuzden dongunun icinde ve
# system_server gorulunce.
kilidi_kaldir() {
    [ -f /data/sirsch/kilit-kalkti ] && return 0
    surec_var system_server || return 0
    mkdir -p /data/sirsch 2>/dev/null
    # Bayragi ancak GERCEKTEN uygulanirsa koy.  Ilk surumde komut basarisiz
    # olsa bile bayrak konuyordu: 26 Agustos'ta "settings" servisi henuz
    # kayitli degildi, komut "Can't find service: settings" ile dondu, bayrak
    # yine de kondu ve duzeltme bir daha hic denenmedi.
    settings put secure lockscreen.disabled 1 >/dev/null 2>&1 || return 0
    [ "$(settings get secure lockscreen.disabled 2>/dev/null)" = "1" ] || return 0
    locksettings set-disabled true >/dev/null 2>&1
    settings put global device_provisioned 1 >/dev/null 2>&1
    settings put secure user_setup_complete 1 >/dev/null 2>&1
    touch /data/sirsch/kilit-kalkti 2>/dev/null
    yaz "kilit ekrani kapatildi (bir kerelik)"
}

# 26 Agustos: sirsch_ag ARTIK KAPATILMIYOR, tam tersine kalkanda ACIKCA
# baslatiliyor.
#
# Eski hali onu da sustururdu ve gerekcesi "kalkan telefonu sahiplenir" idi.
# Bu gece o gerekcenin bedeli olculdu: telefon kalkan modunda kilitlendi ve
# geriye tek kanal olarak USB kablosunun ucundaki rndis kaldi.  Kablo cekilseydi
# ya da telefon duvar sarjina takilsaydi cihaz komple ulasilamaz olurdu.
#
# Bir dugumun agi, uzerinde ne calistigindan daha temel.  Cerceve
# durdurulabilir; WiFi durdurulmamali.
# sirsch_ag init.rc'de "oneshot": cikinca "stopped" kaliyor.  Kosulsuz yeniden
# baslatmak, wlan0 hic gelmeyen bir cihazda 25 saniyelik sonsuz bir dongu
# uretirdi - gunlugu doldurur, pili yer, hicbir seyi duzeltmez.  Bu yuzden
# sayili deneme: uc kez dene, sonra birak.  Bir dugumun kendini kurtarmasi
# gereken sey ag, ama kurtaramiyorsa ugrasmasi da bir maliyet.
AG_DENEME=0
AG_TAVAN=1   # 26 Agu: 3 deneme = 3 x 120 sn bosuna yuk; bir kez dene, yeter
ag_baslat() {
    [ "$(getprop init.svc.sirsch_ag)" = "running" ] && return 0
    [ "$AG_DENEME" -ge "$AG_TAVAN" ] && return 0
    AG_DENEME=$((AG_DENEME + 1))
    yaz "sirsch_ag baslatiliyor ($AG_DENEME/$AG_TAVAN)"
    start sirsch_ag 2>/dev/null
    sleep 2
}

# WiFi, framework'e bagli olmadan.
#
# Android'in wpa_supplicant'i kendi kendine baglanmaz: HIDL'e kaydolur ve
# hangi agi secmesi gerektiginin soylenmesini bekler.  Bunu normalde framework
# soyler, ve framework 2,4 GHz'de yaklasik -80 dBm altinda otomatik katilmayi
# reddeder.  Bu cihaz kendi AP'sini -87..-92 dBm goruyor, dolayisiyla framework
# hicbir zaman "Trying to associate" bile demiyordu.  Oysa o seviyede
# iliskilenme calisiyor - olculdu: 1-5,5 Mbit/s, 137 ms RTT, kayipsiz.
#
# Bir sunucu dugumunun agi, uzerinde ne calistigindan daha temel; o yuzden
# supplicant dogrudan suruluyor (bkz. sirsch-wifi.sh / agci).
# 26 Agustos: TAVAN KALDIRILDI.
#
# Uc denemeden sonra birakmak, "baglanamiyorsa ugrasmasi da bir maliyet"
# mantigiyla konmustu.  Ama bu cihazin AGI, uzerinde ne calistigindan daha
# temel: wifi dusunce telefon komple ulasilamaz oluyor ve geri getirmenin tek
# yolu fiziksel temas kaliyor.  Bu aksam tam bu oldu - bir test wlan0'in
# adresini sildi, iliskilenme de dustu, uc deneme tukendi ve cihaz agdan
# kayboldu.
#
# Yeni kural: vazgecme, ama SEYREK dene.  Ilk denemeler sik, sonra aralik
# aciliyor; boylece hicbir zaman "artik denemiyorum" durumu olusmuyor ama
# wlan0 hic gelmeyen bir cihazda da sonsuz dongu kurulmuyor.
WIFI_DENEME=0
wifi_ayakta() {
    [ -x /system/bin/sirsch-wifi.sh ] || return 0
    [ "$(/system/bin/agci STATUS 2>/dev/null | grep -c wpa_state=COMPLETED)" = 1 ] && {
        WIFI_DENEME=0
        return 0
    }
    WIFI_DENEME=$((WIFI_DENEME + 1))
    # Seyrelme: ilk 3 tur her seferinde, sonra her 5. tur, 20'den sonra her 20.
    if [ "$WIFI_DENEME" -gt 20 ]; then
        [ $((WIFI_DENEME % 20)) = 0 ] || return 0
    elif [ "$WIFI_DENEME" -gt 3 ]; then
        [ $((WIFI_DENEME % 5)) = 0 ] || return 0
    fi
    yaz "wifi baglanmis degil, suruluyor (deneme $WIFI_DENEME)"
    /system/bin/sirsch-wifi.sh >> $L 2>&1
}

# Boot bittiginde cekirdeklere ust basamagi geri veren rc satiri
# sys.boot_completed'a bagli.  Bassiz kipte o property hic gelmez, dolayisiyla
# frekans 800 MHz'de kalirdi - bu turda tam bu oldu.
# DIKKAT: bu ROM'un toybox'inda `pgrep -f` ESLESME OLMASA BILE 0 donuyor.
# Olculdu:  pgrep -f "kesinlikle-olmayan-bir-sey" ; echo $?  ->  0
# (-f olmadan dogru davraniyor, 1 donuyor.)  Bu yuzden `pgrep -f ... && ...`
# her zaman dogru sayilir - ve 26 Agustos'ta "zaten calisiyorsa tekrar baslatma"
# korumasi, ekrani HIC baslatmamaya donustu.  Kimse hata gormedi cunku sessizce
# erken donuyordu.  /proc'u kendimiz tariyoruz.
# /proc/<pid>/cmdline OKUMAK BU CEKIRDEKTE ASILABILIR.
#
# 26 Agustos gecesi kalkan iki dakikadan uzun sure dondu ve hicbir sey
# denetlemedi - ne ekrani geri getirdi ne zamanlayiciyi.  Sebep: burada
# catallanan `tr '\0' ' ' < /proc/7445/cmdline`, 7445 okuma sirasinda olunce
# EOF hic almadi ve R durumunda donmeye devam etti.  $(...) o boruyu
# bekledigi icin butun denetleyici onunla birlikte dondu.  Hicbir yerde hata
# yoktu; "kalkan calisiyor" gorunuyordu.
#
# Cozum iki yonlu:
#   1. cmdline yerine comm - tek satir, NUL yok, surec kaybolursa read
#      basitce basarisiz oluyor;
#   2. `tr` catallamak yok, kabugun kendi `read`i - 140 surecte 140 fork da
#      gitmis oluyor (bu dongu turda bir kez degil, birkac kez cagriliyor).
#
# comm 15 karakterle sinirli, o yuzden cagiranlar TAM YOL degil KISA AD verir.
surec_var() {
    for p in /proc/[0-9]*; do
        read -r _c < "$p/comm" 2>/dev/null || continue
        case "$_c" in *"$1"*) return 0 ;; esac
    done
    return 1
}

frekansi_ac() {
    for c in /sys/devices/system/cpu/cpu0 /sys/devices/system/cpu/cpu1; do
        [ -w "$c/cpufreq/scaling_max_freq" ] || continue
        echo 1000000 > "$c/cpufreq/scaling_max_freq" 2>/dev/null
        echo interactive > "$c/cpufreq/scaling_governor" 2>/dev/null
    done
}

# init.rc'de bir sirsch_ekran servisi tanimli, ama o tanim boot.img'in icinde;
# yeni bir boot.img yazilana kadar cihazdaki init onu bilmiyor.  O yuzden once
# servis denenir, yoksa ikili dogrudan baslatilir - dongu her turda kontrol
# ettigi icin olen bir ekran yine geri gelir.
ekran_ayakta() {
    [ -x /system/bin/ekran ] || return 0
    [ "$(getprop init.svc.sirsch_ekran)" = "running" ] && return 0
    surec_var ekran && return 0
    start sirsch_ekran 2>/dev/null
    sleep 1
    [ "$(getprop init.svc.sirsch_ekran)" = "running" ] && { yaz "ekran: init servisi"; return 0; }
    # Ikinci ornegi ekranin KENDISI reddediyor (kilit dosyasi), ama yine de
    # burada da bakiyoruz: 26 Agu'da uc ornek birden calisti ve ucu birden
    # framebuffer'a yazip farkli tamponlara gecti - ekranda titreme olarak
    # gorundu.  Iki katman kontrol, birinin kacirdigini digeri tutar.
    surec_var ekran && return 0
    /system/bin/ekran -s 4 >>/data/sirsch/ekran.log 2>&1 &
    sleep 1
    if surec_var ekran; then
        yaz "ekran baslatildi (dogrudan)"
    else
        yaz "ekran BASLAMADI: $(tail -1 /data/sirsch/ekran.log 2>/dev/null)"
    fi
}

# Is zamanlayici.  ekran_ayakta ile ayni desen: init servisi yok, dogrudan
# baslatiliyor ve her turda bakiliyor.
#
# Isler neden buradan degil de zamanlayicidan kosuyor: bu dongude "her N
# turda bir" seklinde yazildiklarinda ne ne zaman kostu GORUNMUYORDU.  Bir is
# sessizce hata verdiginde fark etmenin yolu yoktu.  Zamanlayici her isin son
# durumunu ve sonraki kosusunu yaziyor, ekran onu gosteriyor - ve dokununca
# elle kosturmak mumkun.
# Tek ornek guvencesi BURADA DEGIL, zamanlayicinin kendi flock kilidinde.
#
# 26 Agustos: burada `surec_var "sirsch-isler.sh"` vardi ve zamanlayici bir
# turlu baslamiyordu.  Sebep: o dizgi, zamanlayiciyi ARAYAN komutlarin kendi
# komut satirlarinda da geciyordu - kabuk "zaten calisiyor" deyip cikiyordu.
# Ayni tuzagin ucuncu tekrari (pgrep -f kendini sayiyor, pkill -f kendini
# olduruyor).  Cozum kalibi duzeltmek degil: dislamayi TEK yerde tutmak.
# Zamanlayici ikinci ornegi kendi reddediyor, buradan kosulsuz baslatiliyor.
isler_ayakta() {
    [ -x /system/bin/sirsch-isler.sh ] || return 0
    # Zamanlayici kendi PID'ini yaziyor; /proc'ta duruyorsa dokunma.
    # Desen eslestirme YOK - o yol dort kez yanittti.  Kilit yine de son soz:
    # PID dosyasi bayatsa bile ikinci ornek kendini reddediyor.
    _p=$(cat /data/sirsch/isler.pid 2>/dev/null)
    [ -n "$_p" ] && [ -d "/proc/$_p" ] && return 0
    /system/bin/sirsch-isler.sh dongu >/dev/null 2>&1 &
}

# Not: burada bir `uyku_kilidi` vardi; 26 Agustos aksami islevi bosaltildi ve
# 27 Agustos'ta govdesiyle birlikte kaldirildi.  Kalici bir wake_lock cihazin
# sarj almasini engelliyordu (+143 mA'den -256 mA'ye).  Kilit artik `ekran`in
# isi ve gerekcesi orada yaziyor: ekran acikken aliniyor, kapaninca
# birakiliyor.  Hicbir sey yapmayan bir fonksiyonu cagirmaya devam etmek,
# okuyana hala bir sey yapiliyormus gibi gosteriyordu.

# Dokunmatigi ACAN sey bir uyku->uyanma GECISI.
#
# mxt224s probe'u yongayi kurar ama IRQ'sunu acmaz; onu acan tek yol
# mxt_resume(), ve ona ulasmanin tek yolu late_resume - yani once bir
# early_suspend olmali.  Cerceve calisirken bu gecisi PowerManager uretiyor;
# bassiz kipte hic olmuyor ve dokunmatik sonsuza kadar sessiz kaliyor.
# Olculdu (26 Agu): gecisten once dokunmaya kesme bile gelmiyor, obj_show
# "I/O error"; gecisten sonra dmesg "T6: normal mode" diyor ve dokunma
# 30 -> 1851 kesme, 59 KB olay uretiyor (X 3..471, Y 31..642 - panele birebir).
#
# wake_lock elde oldugu icin "mem" SoC'yi gercekten uyutmuyor; yalnizca
# erken-uyku kancalarini kosturuyor.  Bir kez yeter, o yuzden bayrakli.
dokunmatigi_ac() {
    [ -f /data/sirsch/dokunmatik-acildi ] && return 0
    [ -w /sys/power/state ] || return 0
    grep -q golden /sys/power/wake_lock 2>/dev/null || echo golden > /sys/power/wake_lock 2>/dev/null
    echo mem > /sys/power/state 2>/dev/null
    sleep 2
    echo on > /sys/power/state 2>/dev/null
    sleep 1
    mkdir -p /data/sirsch 2>/dev/null
    touch /data/sirsch/dokunmatik-acildi 2>/dev/null
    yaz "dokunmatik uyandirildi (uyku->uyanma gecisi)"
}

if [ "$MOD" = "bassiz" ]; then
    dokunmatigi_ac
    sustur
    frekansi_ac
    ekran_ayakta
    yaz "bassiz: cerceve kapatildi, ekran ve frekans acildi"
fi

if [ "$MOD" = "kalkan" ]; then
    sustur
    ag_baslat
    yaz "cerceve kapatildi: $(echo $KAPAT | tr '\n' ' ')"
    karart
fi
ag
adb_tcp

# Reaching the host proves the channel works.  Until that has happened once,
# shield mode is on a clock: a boot that cannot be reached is worth no more
# than a boot that crashed, and TWRP is a state that can always be worked from.
# Touching /cache/kal pins it open - the serial console is a real channel even
# when rndis is not, and a session in progress must not be rebooted out from
# under itself.
GORULDU=0
BASLANGIC=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
SURE=720
gecen() { echo $(( $(cut -d' ' -f1 /proc/uptime | cut -d. -f1) - BASLANGIC )); }
temas() {
    [ -f /cache/kal ] && return 0
    timeout 4 ping -c 1 -W 2 192.168.42.1  >/dev/null 2>&1 && return 0
    timeout 4 ping -c 1 -W 2 192.168.0.24  >/dev/null 2>&1 && return 0
    return 1
}

# Evidence, written every round.  If something still resets the handset this is
# the only thing that survives it, and it has to be small enough that a serial
# console can print it inside one boot window.
i=0
while :; do
    # Read the mode every round rather than trusting the one decided at boot:
    # switching between shield and full framework is the single most repeated
    # step in this work, and a reboot per switch costs three minutes and a
    # percent of a battery that cannot be recharged over this cable.
    #   setprop sirsch.mod tam     -> let the framework run
    #   setprop sirsch.mod kalkan  -> hold it down again
    MOD=$(getprop sirsch.mod)
    [ -n "$MOD" ] || MOD=tam
    [ "$MOD" = "kalkan" ] && sustur
    if [ "$MOD" = "bassiz" ]; then
        sustur
        frekansi_ac
        ekran_ayakta
    fi
    isler_ayakta
    # Pil arsivi, saat ve panel tazeleme buradan KALDIRILDI (26 Agu aksami):
    # ucu de artik /data/sirsch/isler.conf'ta birer is ve zamanlayici
    # kosturuyor.  Iki yerden kosturmak, ikisinin de calistigini sanip
    # hicbirinin calismadigi durumu gizler.  Zamanlayici olurse bu dongu onu
    # geri getiriyor - kontrol katmani duruyor, isin kendisi tek yerde.
    # Only in shield mode.  A full boot is one somebody may actually be looking
    # at, and a black screen during it cannot be told apart from a dead one.
    [ "$MOD" = "kalkan" ] && karart
    ag
    adb_tcp
    ag_baslat
    wifi_ayakta
    kilidi_kaldir
    # Kalp atisi: dmesg + logcat okumak bu donguuun EN pahali adimi.  Boot
    # bittikten sonra 10 turda bir (5 dakika) yeterli.
    KALP=5
    [ "$(getprop sys.boot_completed)" = "1" ] && KALP=10
    if [ $((i % KALP)) = 0 ]; then
        {
            echo "up=$(cut -d' ' -f1 /proc/uptime) mod=$MOD tur=$i"
            echo "adbd=$(getprop init.svc.adbd) sf=$(getprop init.svc.surfaceflinger) zyg=$(getprop init.svc.zygote)"
            echo "net=$(ip -4 -o addr show 2>/dev/null | tr -s ' ' | cut -d' ' -f2,4 | tr '\n' ' ')"
            echo "pil=$(cat /sys/class/power_supply/battery/capacity 2>/dev/null) akim=$(cat /sys/class/power_supply/battery/current_now 2>/dev/null)"
            echo "-- dmesg son 12 --"; dmesg 2>/dev/null | tail -12
            echo "-- crash son 12 --"; logcat -b crash -d -v brief 2>/dev/null | tail -12
        } > /cache/sirsch-kalp.txt 2>&1
        sync
    fi
    if [ "$GORULDU" = 0 ] && temas; then
        GORULDU=1
        yaz "host'a ulasildi (up=$(cut -d' ' -f1 /proc/uptime)) - geri donus saati iptal"
    fi
    # 26 Agustos: BU GERI DONUS ARTIK VARSAYILAN OLARAK KAPALI.
    #
    # Kanit (last_kmsg):
    #   init: Received sys.powerctl='reboot,shell' from pid: 19921 (reboot) @730 sn
    #   init: Received sys.powerctl='reboot,shell' from pid: 15829 (reboot) @921 sn
    # Yani host'a ping gecmedigi her ~12 dakikada telefon kendini TWRP'ye
    # atiyordu.  24 Agustos'ta dogru karardi (telefon hic boot etmiyordu ve
    # kaybolmasi en buyuk riskti); artik boot ediyor ve ayni mekanizma onu
    # boot ETMEKTEN alikoyuyor - kalkan modunun ters varsayilaniyla ayni hata.
    #
    # Geri donus artik acikca istenir: /cache/geri-don dosyasi.  Yoksa telefon
    # ne olursa olsun ROM'da kalir; zaten seri konsol, rndis ve adb uc ayri
    # kanal olarak duruyor.
    if [ -f /cache/geri-don ] && [ "$GORULDU" = 0 ] && [ "$(gecen)" -gt "$SURE" ]; then
        yaz "=== /cache/geri-don var ve $SURE saniyede temas yok, TWRP'ye donuluyor ==="
        sync
        exec /system/bin/sirsch-recovery.sh
    fi
    i=$((i + 1))

    # ADAPTIF BEKLEME.
    #
    # 26 Agustos, nod'un /surecler ucuyla olculdu: bu dongu ve kardesi
    # sirsch-izle.sh bosta duran bir telefonda bir cekirdegin ~%11'ini, ve
    # daha kotusu init'in %18,3'unu yiyordu.  Init'in payi dolayli: her
    # getprop/start bir soket gidis-donusu, ve uc saniyede bir onlarcasi
    # gonderiliyordu.  Toplam, hicbir sey olmazken surekli yanan ~%30 cekirdek
    # ve pil.
    #
    # Boot bittiyse ve makine sakinse bu tempoya gerek yok: dongunun isi
    # arizayi yakalamak, ve ariza saniyede bir yoklanarak degil dakikada bir
    # yoklanarak da yakalanir.  Boot henuz bitmediyse eski tempo korunuyor -
    # kritik pencere orasi.
    if [ "$(getprop sys.boot_completed)" = "1" ]; then
        BEKLE=30
    else
        BEKLE=3
    fi
    sleep $BEKLE
done
