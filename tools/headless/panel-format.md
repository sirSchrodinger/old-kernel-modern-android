# Panel dosya biçimi

`ekran` veriyi **nereden geldiğini bilmeden** çizer. Bir şey `/data/sirsch/panel/`
altına düz metin bırakır, ekran onu sayfa olarak gösterir. Kaynağın ne olduğu
ekranı ilgilendirmez: sera'nın HTTP API'si, USB'den seri konuşan bir Arduino,
kaplikaya'dan gelen bir rsync, ya da telefonun kendi sensörü — hepsi aynı
dosyayı yazar.

Bu ayrımın sebebi: veri kaynağı değiştiğinde çizim kodunun değişmemesi gerekiyor.

## Dosya: `/data/sirsch/panel/<sira>-<ad>.pnl`

Sıra numarası sayfa sırasını belirler (`10-sera.pnl`, `20-devre.pnl`).

```
baslik Sera — Orhangazi
tazelik 42                      # kaç saniye önce güncellendi
deger sicaklik 24,3 °C
deger nem 61 %
deger toprak 38 % dikkat        # 4. alan varsa durum: ok | dikkat | hata
seri sicaklik 22,1 22,4 23,0 23,8 24,1 24,3
seri-etiket son 24 saat
metin Sulama 06:00'da çalıştı
```

Satır tipleri:

| tip | anlamı |
|---|---|
| `baslik` | sayfa başlığı |
| `tazelik` | verinin yaşı, saniye. Ekran eskiyeni soluk gösterir |
| `deger <ad> <değer> [birim] [durum]` | tek satırlık okuma |
| `seri <ad> <sayı> <sayı> ...` | çizgi grafik. En fazla 120 nokta |
| `seri-etiket <metin>` | grafiğin altına yazılacak |
| `metin <...>` | serbest satır |
| `resim <yol>` | ham RGB565/RGB888 kare (kamera görüntüsü için) |

## Neden düz metin

Bir kabuk betiği, bir Arduino, bir `curl | awk` zinciri — hepsi bunu üretebilir.
JSON olsaydı ekranın içine bir ayrıştırıcı koymam gerekirdi ve her kaynağın
JSON üretmesi gerekirdi. Bu cihazda ikisi de gereksiz maliyet.

## Örnek: sera verisini panele dökmek

```sh
curl -s https://sera.sirschrodinger.com/api/son | \
  awk -F'[:,]' '{...}' > /data/sirsch/panel/10-sera.pnl.yeni
mv /data/sirsch/panel/10-sera.pnl.yeni /data/sirsch/panel/10-sera.pnl
```

`mv` ile: ekran yarım yazılmış bir dosyayı asla okumaz.
