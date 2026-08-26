#!/system/bin/sh
# Is zamanlayici.
#
# NEDEN AYRI BIR DONGU
#
# Bu cihaz bir panel degil, bir DUGUM: ekran kapaliyken de is kosmasi gerekiyor
# (veri cekmek, saati tutmak, kareye bakmak).  Cizim dongusune bagli olsaydi
# ekran uyudugunda butun isler dururdu.
#
# NEDEN CRON DEGIL
#
# Cronun bir daemon'u, kendi saat kavrami ve /etc yazma ihtiyaci var; burada
# ucu de fazla.  Isin tamami "en son ne zaman kostu" dosyasini okuyup aralik
# dolduysa calistirmak - 30 satir kabuk.
#
# TETIKLEME
#
# Ekran bir ise dokundugunda /data/sirsch/is-simdi dosyasina adi yazar.  Bu
# dongu her turda ona bakar.  Boylece dokunus beklemeden calisir ama cizim
# kodu is kosma isini hic bilmez.
#
# BICIM
#   isler.conf   ad|aralik_sn|komut          (aralik 0 = yalniz elle)
#   isler.txt    ad<TAB>durum<TAB>saat<TAB>kalan<TAB>ms<TAB>ozet  (ekranin okudugu)
#   isler.sonuc  ad<TAB>isin bastigi son satir

D=/data/sirsch
CONF=$D/isler.conf
CIKTI=$D/isler.txt
DURUM=$D/isler.durum
SONUC=$D/isler.sonuc
SIMDI_D=$D/is-simdi
TUR=${1:-dongu}

# Ilk acilista is tablosu yok: ROM /data'ya dosya koyamiyor.  Varsayilani
# burada uret - bos bir jobs sayfasi, "zamanlayici yok" ile ayirt edilemez.
if [ ! -f "$CONF" ]; then
    mkdir -p "$D" 2>/dev/null
    cat > "$CONF" <<'VARSAYILAN'
# ad|aralik_sn|komut     (aralik 0 = yalniz elle)
panel|120|/system/bin/sirsch-panel-uret.sh
goz|300|/system/bin/sirsch-goz.sh
pil|600|/system/bin/sirsch-pil-arsiv.sh
saat|3600|case $(date -u +%Y) in 20[2-9][0-9]) : ;; *) /system/bin/sirsch-saat.sh ;; esac
ag|300|/system/bin/sirsch-ag-adres.sh wlan0
VARSAYILAN
    chmod 666 "$CONF" 2>/dev/null
fi
[ -f "$CONF" ] || exit 0

# Durum dosyasi BOSLUKLA ayrilmis, boru ile degil.
#
# Ilk surumde "ad|epoch|durum|ms" idi ve alanlar ${x%%|*} ile ayikliniyordu.
# Bu kabukta o kalip BOS donuyor - dosya doluydu, ekran bos gosteriyordu ve
# hicbir yerde hata yoktu.  (Ayni tuzak 26 Agustos'ta saat betiginde de
# yasandi: curl "http:///" adresine gitti.)  `set --` bosluga gore boler ve
# bu kabukta calistigi olculdu; ustelik hic surec catallamiyor.
son_kosu() {           # ad -> IS_EPOCH IS_DU IS_MS
    IS_EPOCH=0; IS_DU=-; IS_MS=0
    _s=$(grep "^$1 " "$DURUM" 2>/dev/null | tail -1)
    [ -n "$_s" ] || return 0
    # shellcheck disable=SC2086
    set -- $_s
    [ $# -ge 4 ] || return 0
    IS_EPOCH=$2; IS_DU=$3; IS_MS=$4
}

kos() {                # ad komut
    _ad=$1; shift
    _t0=$(date +%s)
    # Cikti bilerek yutuluyor: bir isin ciktisi ekranda degil kendi .pnl
    # dosyasinda gorunur.  Burada onemli olan tek sey cikis kodu.
    # </dev/null SART.  Isler `while read ... done < isler.conf` dongusunun
    # icinden kosuyor; stdin kapatilmazsa calisan komut geri kalan yapilandirma
    # satirlarini YUTUYOR.  26 Agustos: dort isten sonra listede tek is
    # kalmisti ve hicbir yerde hata yoktu - is tablosu sessizce kisaliyordu.
    #
    # Cikti artik YUTULMUYOR: son satiri isin SONUCU olarak sakliniyor.
    # "ok" tek basina bir sey soylemiyor - Alperen bir ise basti ve "hicbir
    # sey olmuyor" dedi, hakliydi: is kosuyordu ama NE YAPTIGI hicbir yerde
    # gorunmuyordu.  Her is tek satirlik bir ozet basiyor, o satir ekranda
    # isin altinda duruyor.
    if sh -c "$*" </dev/null > "$D/.is-cikti" 2>/dev/null; then _du=ok; else _du=hata; fi
    _sonuc=$(tail -1 "$D/.is-cikti" 2>/dev/null | tr -d '\t' | cut -c1-34)
    rm -f "$D/.is-cikti"
    grep -v "^$_ad	" "$SONUC" 2>/dev/null > "$SONUC.yeni"
    printf '%s\t%s\n' "$_ad" "$_sonuc" >> "$SONUC.yeni"
    mv "$SONUC.yeni" "$SONUC"
    _t1=$(date +%s)
    # date +%s%N bu toybox'ta yok; saniye cozunurlugu isler icin yeterli.
    _ms=$(( (_t1 - _t0) * 1000 ))
    grep -v "^$_ad " "$DURUM" 2>/dev/null > "$DURUM.yeni"
    echo "$_ad $_t1 $_du $_ms" >> "$DURUM.yeni"
    mv "$DURUM.yeni" "$DURUM"
}

tur() {
    SIMDI=$(date +%s)
    ELLE=""
    if [ -f "$SIMDI_D" ]; then
        ELLE=$(cat "$SIMDI_D" 2>/dev/null)
        rm -f "$SIMDI_D"
    fi
    # Cikti once gecici dosyaya: ekran yarim yazilmis bir listeyi asla okumaz.
    : > "$CIKTI.yeni"
    while IFS='|' read -r ad aralik komut; do
        case "$ad" in ''|'#'*) continue ;; esac
        [ -n "$komut" ] || continue
        son_kosu "$ad"
        gecen=$(( SIMDI - IS_EPOCH ))
        if [ "$ELLE" = "$ad" ] || { [ "$aralik" -gt 0 ] 2>/dev/null && [ "$gecen" -ge "$aralik" ]; }; then
            kos "$ad" "$komut"
            son_kosu "$ad"
            gecen=0
        fi
        epoch=$IS_EPOCH; du=$IS_DU; ms=$IS_MS
        if [ "$aralik" -gt 0 ] 2>/dev/null; then kalan=$(( aralik - gecen )); else kalan=-1; fi
        [ "$kalan" -lt 0 ] 2>/dev/null && [ "$aralik" -gt 0 ] 2>/dev/null && kalan=0
        if [ "$epoch" -gt 0 ] 2>/dev/null; then
            saat=$(date -d "@$epoch" +%H:%M 2>/dev/null)
            [ -n "$saat" ] || saat=$(date +%H:%M)
        else
            saat="-"
        fi
        ozet=$(grep "^$ad	" "$SONUC" 2>/dev/null | tail -1)
        ozet=${ozet#*	}
        [ "$ozet" = "$ad" ] && ozet=""
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$ad" "$du" "$saat" "$kalan" "$ms" "$ozet" >> "$CIKTI.yeni"
    done < "$CONF"
    mv "$CIKTI.yeni" "$CIKTI"
}

# Tek ornek.  26 Agustos'ta `ekran`in uc ornegi ayni anda framebuffer'a yazdi
# ve Alperen bunu titreme olarak gordu; burada iki ornek her turda ayni isleri
# iki kez kosturur ve "kalan" sayaci gorunurde delirir.
# flock kilidi TUTAN dosyaya bagli: kilit dosyasi silinirse yeni surecler
# kilidin tutuldugunu goremez, o yuzden dosya hic silinmiyor.
KILIT=/data/sirsch/isler.kilit

case "$TUR" in
    tur)   tur ;;
    dongu)
        # `exec 9<>dosya` sonra `flock -n 9` DEGIL: bu kabukta o bicimde
        # flock "Bad file descriptor" veriyor ve betik sessizce cikiyordu -
        # kalkan her turda baslatiyor, zamanlayici her turda oluyordu ve
        # hicbir yerde hata gorunmuyordu.  Alt kabuk + 9>dosya bicimi
        # flock'a gercek bir tanitici veriyor; toybox flock zaten yalniz
        # tanitici aliyor, dosya yolu almiyor.
        [ -f "$KILIT" ] || : > "$KILIT"
        (
            flock -n 9 || { echo "zaten calisiyor" >&2; exit 0; }
            # PID'i yaz: kalkan her turda yeni bir ornek catallayip kilide
            # takilmasin diye once buna bakiyor.  Desen eslestirmek yerine
            # /proc/<pid> var mi diye bakmak, "arayan komutun kendi satirini
            # eslemesi" tuzagina hic girmiyor.
            echo $$ > "$D/isler.pid"
            while :; do tur; sleep 30; done
        ) 9>"$KILIT" ;;
    *)     echo "kullanim: sirsch-isler.sh [tur|dongu]" >&2; exit 2 ;;
esac
