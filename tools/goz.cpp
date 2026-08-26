/*
 * goz - bu telefonu "bir seye bakip olay ureten" bir cihaz haline getiren parca.
 *
 * NEDEN OLAY, NEDEN AKIS DEGIL
 *
 * Bu donanimin olculen profili sunu soyluyor: tek cekirdek bellek yolunu
 * neredeyse dolduruyor (STREAM Triad 574 -> 661 MB/s, iki is parcacigi %15
 * ekliyor), yani surekli akis islemek icin yanlis makine.  Buna karsilik
 * yolo-fastestv2 int8 ile ~5 fps tespit yapiyor ve KENDI PILI var - elektrik
 * kesildiginde router'la birlikte olmuyor.  Dogru kullanim bu ikisinin
 * kesisimi: seyrek bakip, bir sey oldugunda olay uretmek.
 *
 * Bu yuzden goz bir video isleyici degil, bir dosya isleyici: kendisine
 * verilen goruntuyu isler ve JSON dokuyor.  Kareyi kim uretiyorsa (kamera
 * betigi, UVC yakalayici, baska bir sey) onunla borulanir.  Boylece kamera
 * yolu degistiginde tespit tarafi hic degismez.
 *
 * AKTIVASYONLAR MODELIN ICINDE
 *
 * Bu .param dosyasindaki agin cikisi HAM LOGIT DEGIL.  Bos bir goruntude
 * olculdu: [0:12] kutu degerleri 0,299-0,691 arasinda, [12:15] nesnellik tam
 * 0,000, [15:95] sinif olasiliklari 0,001-0,148.  Yani sigmoid/softmax dis
 * ihracatta agin icine katilmis.
 *
 * Ilk surumde bunlarin ustune bir kez daha sigmoid uygulanmisti.  Sonuc
 * "calisiyor" gibi gorunuyordu - iki kase brokoli olan bir karede gercekten
 * "broccoli" ve "dining table" buluyordu - ama butun skorlar 0,61 civarinda
 * kumelenmisti, cunku sigmoid(0)=0,5 ve sqrt(0,5 x 0,75)=0,61.  Yani nesnellik
 * bilgisi tamamen kayipti ve "guven" diye basilan sayi bir sabitti.
 *
 * Dogru sinifi bulan ama guveni uyduran bir dedektor, yanlis olandan daha
 * tehlikelidir: esik koymaya calisan herkesi yaniltir.
 *
 * COZUMLEME HAKKINDA DURUSTLUK
 *
 * yolo-fastestv2'nin cikis duzeni belgelenmis degil; asagidaki cozumleme
 * cikis seklinden (2 olcek: 22x22 ve 11x11, hucre basina 95 deger = 3x4 kutu
 * + 3 nesnellik + 80 sinif) turetildi ve GERCEK bir COCO goruntusuyle
 * dogrulandi - iki kase brokoli iceren bir kareye "bowl" ve "broccoli"
 * dedigi gorulerek.  Dogru sinifi bulmayan bir cozumleme, makul gorunen ama
 * yanlis kutular uretir; o yuzden dogrulama goze dayali degil isme dayali.
 */
#include <net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <vector>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

static const char* SINIFLAR[] = {
"person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
"fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
"elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
"skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard",
"tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
"sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch",
"potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone",
"microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear",
"hair drier","toothbrush"};

/* yolo-fastestv2, COCO.  Ilk uc cift stride 16, son uc cift stride 32. */
static const float CAPA[12] = {12.64f,19.39f, 37.88f,51.48f, 55.71f,138.31f,
                               126.91f,78.23f, 131.57f,214.55f, 279.92f,258.87f};

struct Kutu { float x1,y1,x2,y2,skor; int sinif; };

static inline float sigmoid(float x){ return 1.f/(1.f+expf(-x)); }

static float ortusme(const Kutu&a,const Kutu&b){
    float x1=std::max(a.x1,b.x1), y1=std::max(a.y1,b.y1);
    float x2=std::min(a.x2,b.x2), y2=std::min(a.y2,b.y2);
    float w=x2-x1, h=y2-y1;
    if(w<=0||h<=0) return 0.f;
    float kesisim=w*h;
    float birlesim=(a.x2-a.x1)*(a.y2-a.y1)+(b.x2-b.x1)*(b.y2-b.y1)-kesisim;
    return birlesim>0? kesisim/birlesim : 0.f;
}

/* Sinif bazli NMS: iki farkli sinifin ust uste binmesi normaldir (kasenin
 * icindeki brokoli), ayni sinifin ikinci kez sayilmasi degil. */
static void nms(std::vector<Kutu>& k, float esik){
    std::sort(k.begin(),k.end(),[](const Kutu&a,const Kutu&b){return a.skor>b.skor;});
    std::vector<bool> at(k.size(),false);
    std::vector<Kutu> kalan;
    for(size_t i=0;i<k.size();i++){
        if(at[i]) continue;
        kalan.push_back(k[i]);
        for(size_t j=i+1;j<k.size();j++)
            if(!at[j] && k[j].sinif==k[i].sinif && ortusme(k[i],k[j])>esik) at[j]=true;
    }
    k.swap(kalan);
}

static void cozumle(const ncnn::Mat& cikti, int capa_ofset, int giris_boyut,
                    float esik, std::vector<Kutu>& out)
{
    const int izgara_y = cikti.c;
    const int izgara_x = cikti.h;
    const int ozellik  = cikti.w;          /* 95 */
    if (ozellik < 15 + 80) return;

    for(int gy=0; gy<izgara_y; gy++){
        const float* duzlem = cikti.channel(gy);
        for(int gx=0; gx<izgara_x; gx++){
            const float* v = duzlem + gx*ozellik;

            /* Sinif tahmini uc capa arasinda PAYLASILIYOR - yolo-fastestv2'nin
             * kucuk olmasinin sebeplerinden biri bu.  Bir kez hesapla. */
            int en_sinif=0; float sinif_p=-1e9f;
            for(int c=0;c<80;c++){
                float s=v[15+c];
                if(s>sinif_p){ sinif_p=s; en_sinif=c; }
            }

            for(int a=0;a<3;a++){
                float nesne = v[12+a];
                /* Skor iki olasiligin geometrik ortalamasi: nesnellik tek
                 * basina arka plani eliyor ama neyin oldugunu soylemiyor. */
                float skor = sqrtf(nesne * sinif_p);
                if(skor < esik) continue;

                const float* r = v + a*4;
                float cx = (r[0] + gx) / izgara_x;
                float cy = (r[1] + gy) / izgara_y;
                float bw = r[2] * CAPA[capa_ofset+a*2+0] / giris_boyut;
                float bh = r[3] * CAPA[capa_ofset+a*2+1] / giris_boyut;

                Kutu k;
                k.x1=cx-bw/2; k.y1=cy-bh/2; k.x2=cx+bw/2; k.y2=cy+bh/2;
                k.skor=skor; k.sinif=en_sinif;
                out.push_back(k);
            }
        }
    }
}

int main(int argc,char**argv){
    if(argc<4){
        fprintf(stderr,
          "kullanim: %s <param> <bin> <resim.jpg> [esik] [is_parcacigi]\n"
          "ornek   : %s yolo-int8.param yolo-int8.bin kare.jpg 0.35 2\n", argv[0],argv[0]);
        return 1;
    }
    const char* param=argv[1]; const char* bin=argv[2]; const char* resim=argv[3];
    float esik = argc>4 ? atof(argv[4]) : 0.35f;
    int is     = argc>5 ? atoi(argv[5]) : 2;
    const int BOYUT = 352;

    int gw,gh,gk;
    unsigned char* piksel = stbi_load(resim,&gw,&gh,&gk,3);
    if(!piksel){ fprintf(stderr,"resim okunamadi: %s\n",resim); return 1; }

    ncnn::Net net;
    /* Bu cihazda olculdu: winograd ve sgemm bellek trafigini artiriyor,
     * lightmode ara bloblari erken birakiyor.  Varsayilanlar hesabin pahali
     * oldugu makineler icin secilmis; burada tavan bellek. */
    net.opt.lightmode = true;
    net.opt.num_threads = is;
    net.opt.use_int8_inference = true;
    net.opt.use_vulkan_compute = false;
    if(net.load_param(param)||net.load_model(bin)){ fprintf(stderr,"model yuklenemedi\n"); return 1; }

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(piksel, ncnn::Mat::PIXEL_RGB,
                                                 gw, gh, BOYUT, BOYUT);
    const float norm[3] = {1/255.f,1/255.f,1/255.f};
    in.substract_mean_normalize(0, norm);

    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    ncnn::Extractor ex = net.create_extractor();
    ex.input("input.1", in);
    ncnn::Mat o16, o32;
    ex.extract("794", o16);      /* 22x22, stride 16 */
    ex.extract("796", o32);      /* 11x11, stride 32 */
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double ms=(t1.tv_sec-t0.tv_sec)*1000.0+(t1.tv_nsec-t0.tv_nsec)/1e6;

    std::vector<Kutu> kutular;
    cozumle(o16, 0, BOYUT, esik, kutular);
    cozumle(o32, 6, BOYUT, esik, kutular);
    nms(kutular, 0.45f);

    printf("{\"dosya\":\"%s\",\"genislik\":%d,\"yukseklik\":%d,\"sure_ms\":%.2f,"
           "\"is_parcacigi\":%d,\"esik\":%.2f,\"tespit\":[", resim,gw,gh,ms,is,esik);
    for(size_t i=0;i<kutular.size();i++){
        const Kutu&k=kutular[i];
        printf("%s{\"sinif\":\"%s\",\"skor\":%.3f,\"kutu\":[%d,%d,%d,%d]}",
               i?",":"", SINIFLAR[k.sinif], k.skor,
               (int)(k.x1*gw),(int)(k.y1*gh),(int)(k.x2*gw),(int)(k.y2*gh));
    }
    printf("]}\n");
    stbi_image_free(piksel);
    return 0;
}
