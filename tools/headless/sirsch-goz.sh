#!/system/bin/sh
# Bir kareye bakip ne gordugunu panele yazar.
#
# NEDEN OLAY, NEDEN AKIS DEGIL
#
# NEDEN TEK IS PARCACIGI
#
# Olculdu (26 Agustos, bu cihaz, yolo-fastestv2 int8 352x352):
#
#              gercekten bos    baska bir sey uyanik   bir cekirdek dolu
#   is=1          270-297 ms          270-297 ms            436-554 ms
#   is=2          198-235 ms          526-1325 ms          1221-1527 ms
#
# Iki is parcacigi EN IYI durumda 1,3 kat hizli, ama iki cekirdegin de bos
# olmasini sart kosuyor.  Bir cekirdek mesgulse ncnn'in bariyeri donuyor ve
# sure alti kata cikiyor.  Bu cihaz bir dugum: zamanlayici, panel ve nod
# arka planda kosuyor, yani "iki cekirdek de bos" garanti edilemez.
#
# Tek is parcacigi en iyi durumda daha yavas ama en kotu durumda uc kat daha
# iyi ve TAHMIN EDILEBILIR.  Bir olay dedektorunde onemli olan tepe hiz degil,
# kacirilmayan kare.  O yuzden burada is=1 SABIT.
#
# (Ilk olcumde yalnizca yuklu haldeki sayilar goruldu ve "iki is parcacigi
#  4,8 kat yavas" diye kaydedildi - yanlisti.  Sayi kosula bagli; kosulu
#  yazmayan olcum, olcum degil.)
#
# 4 kare-sn video degil.  Dogru kullanim: seyrek bak, degisince olay uret.
# Bir kareyi cekip islemek uctan uca ~1,5 sn suruyor (agdan cekme dahil) -
# dakikada bir bakan bir gozcu icin fazlasiyla yeterli, canli takip icin degil.

D=/data/sirsch
CONF=$D/goz.conf
P=$D/panel
GOZ=/system/bin/sirsch-goz
PARAM=/system/etc/sirsch/yolo-int8.param
BIN=/system/etc/sirsch/yolo-int8.bin
KARE=$D/kare.jpg

[ -x "$GOZ" ] || GOZ=/data/local/tmp/goz
[ -f "$PARAM" ] || PARAM=/data/local/tmp/yolo-int8.param
[ -f "$BIN" ] || BIN=/data/local/tmp/yolo-int8.bin
[ -x "$GOZ" ] && [ -f "$PARAM" ] || exit 1

KAYNAK=$(awk '$1=="kaynak"{print $2}' "$CONF" 2>/dev/null)
ESIK=$(awk '$1=="esik"{print $2}' "$CONF" 2>/dev/null)
BASLIK=$(awk '$1=="baslik"{$1="";sub(/^ /,"");print}' "$CONF" 2>/dev/null)
[ -n "$KAYNAK" ] || exit 0
[ -n "$ESIK" ] || ESIK=0.35
[ -n "$BASLIK" ] || BASLIK=camera

mkdir -p "$P" 2>/dev/null
curl -s -m 20 -o "$KARE.yeni" "$KAYNAK" 2>/dev/null || { rm -f "$KARE.yeni"; echo "kare cekilemedi"; exit 1; }
# Bos ya da JPEG olmayan cikti: "200 dondu ama govde bos" bu aglarda sik.
# Sessizce eski kareyi yeniden islemek, panelde canli veri gorunumu yaratir.
[ -s "$KARE.yeni" ] || { rm -f "$KARE.yeni"; echo "kare BOS geldi (200 ama govde yok)"; exit 1; }
mv "$KARE.yeni" "$KARE"

CIKTI=$("$GOZ" "$PARAM" "$BIN" "$KARE" "$ESIK" 1 2>/dev/null)
[ -n "$CIKTI" ] || exit 1

MS=$(echo "$CIKTI" | sed 's/.*"sure_ms":\([0-9.]*\).*/\1/' | cut -d. -f1)
# Siniflari say: {"sinif":"person" gecen her yer bir tespit.
SINIFLAR=$(echo "$CIKTI" | tr '{' '\n' | sed -n 's/.*"sinif":"\([a-z ]*\)".*/\1/p' \
           | sort | uniq -c | awk '{printf "%s x%s ", $2, $1}')
ENIYI=$(echo "$CIKTI" | tr '{' '\n' | sed -n 's/.*"skor":\([0-9.]*\).*/\1/p' | sort -rn | head -1)
ADET=$(echo "$CIKTI" | tr '{' '\n' | grep -c '"sinif"')

# Tespit sayisi gecmisi: panelde grafik olsun diye.
GEC=$D/goz-gecmis
echo "$ADET" >> "$GEC"
tail -60 "$GEC" > "$GEC.yeni" && mv "$GEC.yeni" "$GEC"
SERI=$(tr '\n' ' ' < "$GEC")

{
    echo "title $BASLIK"
    echo "freshness 0"
    echo "value seen $ADET"
    [ "$ADET" -gt 0 ] && echo "value best $ENIYI"
    echo "value ms $MS"
    [ -n "$SINIFLAR" ] && echo "text $SINIFLAR"
    [ "$ADET" -eq 0 ] && echo "text nothing over $ESIK"
    echo "series seen $SERI"
} > "$P/40-goz.pnl.yeni"
mv "$P/40-goz.pnl.yeni" "$P/40-goz.pnl"
if [ "$ADET" -gt 0 ]; then
    echo "$ADET nesne, en iyi $ENIYI, $MS ms"
else
    echo "temiz, $MS ms"
fi
exit 0
