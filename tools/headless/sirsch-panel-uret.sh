#!/system/bin/sh
# Panel dosyalarini uretir.  Ekran verinin nereden geldigini bilmez; bu betik
# de tek bir kaynaga bagli degil.
#
#   1. yerel   - cihazin kendi gecmisi (nod'un halkasi)
#   2. uzak    - /data/sirsch/panel-kaynak.conf icindeki her satir icin
#                bir HTTP adresi cekilir ve oldugu gibi panel dosyasi yapilir
#
# Uzak kaynak formatı, satir basina:  <sira>-<ad> <url> [baslik]
#   20-sera https://ornek/api/panel
#   20-sera https://ornek/api/panel Authorization:Bearer_xyz
#
# Baslik alani opsiyonel ve auth icin: sera gibi giris isteyen uclar kimliksiz
# istekte giris SAYFASINI donduruyor - 200 ve HTML, yani "basarili" gorunen bir
# hata.  O yuzden cikti .pnl'ye benzemiyorsa yazilmiyor.

P=/data/sirsch/panel
HALKA=/data/sirsch/gecmis.ring
mkdir -p "$P" 2>/dev/null

# --- 1. yerel: pil gecmisi ---------------------------------------------------
# Halkadan son ~2 saatin gerilimini al ve seriye dok.  Halka slot sirasinda
# tutuluyor, o yuzden zamana gore siralamak gerekiyor.
if [ -f "$HALKA" ]; then
    # SON 60 ORNEK DEGIL, SON 3 SAAT.
    #
    # Halka 10 saniyede bir yaziyor; son 60 ornek 10 DAKIKA demek.  O
    # pencerede grafikte gorunen sey pilin gidisati degil, gurultu: olculdu,
    # ardisik ornekler arasinda ortalama 22 mV, en fazla 193 mV siciyor ve
    # toplam aralik 86 mV - yani cizginin dortte biri gurultu.  Uc saatlik
    # pencerede ayni sicrama 11 mV'ye dusuyor, aralik 309 mV'ye cikiyor ve
    # sarj egrisi gercekten gorunuyor.
    #
    # Her kova MEDYAN aliniyor, ortalama degil: tek bir 193 mV'lik sicrama
    # ortalamayi kaldiriyor, medyani kaldirmiyor.
    #
    # (V_oc = V + I*R duzeltmesi burada YAPILMIYOR.  Tek bir anlik deger icin
    #  dogru - sys sayfasi oyle hesapliyor - ama zaman serisinde iki gurultulu
    #  sinyali toplamak demek: olculdu, ardisik sicrama 22,4'ten 24,8'e CIKTI.)
    SERI=$(tail -c +129 "$HALKA" 2>/dev/null \
        | tr -s ' ' \
        | awk -v simdi="$(date +%s)" '
            NF >= 4 && $1 ~ /^[0-9]+$/ && $3 > 2500 && $3 < 4500 {
                bas = simdi - 10800
                if ($1 < bas) next
                k = int(($1 - bas) / 180)          # 180 sn = 60 kova
                if (k < 0 || k > 59) next
                n[k]++; v[k, n[k]] = $3
                if (k > enson) enson = k
            }
            END {
                for (k = 0; k <= enson; k++) {
                    if (!n[k]) continue
                    for (i = 2; i <= n[k]; i++) {   # kucuk kovalar, ekleme siralamasi yeter
                        t = v[k, i]
                        for (j = i; j > 1 && v[k, j-1] > t; j--) v[k, j] = v[k, j-1]
                        v[k, j] = t
                    }
                    printf "%d ", v[k, int((n[k] + 1) / 2)]
                }
            }')
    MV=$(cat /sys/class/power_supply/battery/voltage_now 2>/dev/null)
    MV=$((MV / 1000))
    MA=$(cat /sys/class/power_supply/battery/current_now 2>/dev/null)
    SIC=$(cat /sys/class/power_supply/battery/temp 2>/dev/null)
    # Baslik "schrod" DEGIL: ustteki basligin aynisini tekrar etmek yer israfi.
    # Ekranda tek fazla kelime bile yok; ne oldugu adiyla belli olsun.
    {
        echo "title battery"
        echo "value mV $MV"
        echo "value mA $MA"
        [ -n "$SIC" ] && echo "value temp $((SIC / 10)) C"
        if [ -n "$SERI" ]; then
            echo "series mV $SERI"
            echo "series-label son 3 saat, 3 dk'lik medyanlar"
        fi
    } > "$P/10-schrod.pnl.yeni"
    mv "$P/10-schrod.pnl.yeni" "$P/10-schrod.pnl"
fi

# --- 2. uzak kaynaklar -------------------------------------------------------
K=/data/sirsch/panel-kaynak.conf
[ -f "$K" ] || exit 0
command -v curl >/dev/null 2>&1 || exit 0
while read -r ad url baslik; do
    case "$ad" in ''|'#'*) continue ;; esac
    [ -n "$url" ] || continue
    if [ -n "$baslik" ]; then
        curl -s -m 12 -H "$(echo "$baslik" | tr '_' ' ')" "$url" > "$P/$ad.pnl.yeni" 2>/dev/null
    else
        curl -s -m 12 "$url" > "$P/$ad.pnl.yeni" 2>/dev/null
    fi
    # Giris sayfasi 200 ve HTML donuyor: "basarili" gorunen bir hata.  Cikti
    # gercekten .pnl gibi gorunmuyorsa yazma; yarim/yanlis veri, veri yoklugundan
    # daha kotudur cunku ekranda gercek gibi durur.
    if [ -s "$P/$ad.pnl.yeni" ] && grep -qE '^(title|value|series|text) ' "$P/$ad.pnl.yeni"; then
        mv "$P/$ad.pnl.yeni" "$P/$ad.pnl"
    else
        rm -f "$P/$ad.pnl.yeni"
        BASARISIZ="$BASARISIZ $ad"
    fi
done < "$K"

# Isin son satiri ekranda gorunuyor: "ok" tek basina bir sey soylemiyor.
N=$(ls "$P"/*.pnl 2>/dev/null | wc -l)
if [ -n "$BASARISIZ" ]; then
    echo "$N kaynak, cekilemedi:$BASARISIZ"
else
    echo "$N kaynak tazelendi"
fi
exit 0
