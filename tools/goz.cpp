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

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <set>
#include <string>
#include <map>

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


/* --------------------------------------------------------------- calistir */

struct Ayar { float esik; int is; };

static int calistir(ncnn::Net& net, const char* resim, const Ayar& a,
                    std::vector<Kutu>& kutular, double* ms, int* gw, int* gh)
{
    const int BOYUT = 352;
    int gk;
    unsigned char* piksel = stbi_load(resim, gw, gh, &gk, 3);
    if (!piksel) return -1;

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(piksel, ncnn::Mat::PIXEL_RGB,
                                                 *gw, *gh, BOYUT, BOYUT);
    const float norm[3] = {1/255.f,1/255.f,1/255.f};
    in.substract_mean_normalize(0, norm);

    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    ncnn::Extractor ex = net.create_extractor();
    ex.input("input.1", in);
    ncnn::Mat o16, o32;
    ex.extract("794", o16);
    ex.extract("796", o32);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    *ms = (t1.tv_sec-t0.tv_sec)*1000.0 + (t1.tv_nsec-t0.tv_nsec)/1e6;

    kutular.clear();
    cozumle(o16, 0, BOYUT, a.esik, kutular);
    cozumle(o32, 6, BOYUT, a.esik, kutular);
    nms(kutular, 0.45f);
    stbi_image_free(piksel);
    return 0;
}

static void json_yaz(FILE* f, const char* dosya, int gw, int gh, double ms,
                     const Ayar& a, const std::vector<Kutu>& k, long zaman)
{
    fprintf(f,"{\"zaman\":%ld,\"dosya\":\"%s\",\"genislik\":%d,\"yukseklik\":%d,"
              "\"sure_ms\":%.2f,\"is_parcacigi\":%d,\"esik\":%.2f,\"tespit\":[",
            zaman,dosya,gw,gh,ms,a.is,a.esik);
    for(size_t i=0;i<k.size();i++)
        fprintf(f,"%s{\"sinif\":\"%s\",\"skor\":%.3f,\"kutu\":[%d,%d,%d,%d]}",
                i?",":"", SINIFLAR[k[i].sinif], k[i].skor,
                (int)(k[i].x1*gw),(int)(k[i].y1*gh),(int)(k[i].x2*gw),(int)(k[i].y2*gh));
    fprintf(f,"]}\n");
}

/* ---------------------------------------------------------------- gozet
 *
 * Bir dizini izle, yeni gelen kareyi isle, ilginc bir sey varsa OLAY yaz.
 *
 * Neden dizin izliyor da kamerayi kendisi acmiyor: bu telefonda kamera yolu
 * hala belirsiz (dahili sensor Samsung'a ozel bir HAL'in arkasinda, UVC ise
 * cekirdekte hazir ama kablo bekliyor).  Kare kaynagini disarida birakinca
 * tespit tarafi o kararla hic degismiyor - screencap, UVC yakalayici, scp ile
 * gelen dosya, hepsi ayni borudan gecer.
 *
 * Uc sey bilerek sinirli:
 *   - SOGUMA: ayni sinif icin belirli sure gecmeden ikinci olay yazilmaz.
 *     Yoksa kadraja giren bir kisi saniyede bes olay uretir ve olay akisi
 *     kullanilmaz hale gelir.
 *   - TAVAN: saklanan kare sayisi sabit; en eski silinir.  8 GB'lik bir
 *     telefonda sinirsiz kayit, yer bitince cihazi kaybetmek demektir.
 *   - DURUM DISKTE: son olay zamanlari dosyaya yazilir, yani surec yeniden
 *     baslayinca soguma sifirlanip olay firtinasi olmaz.
 */
static long dosya_zamani(const std::string& yol) {
    struct stat st;
    return stat(yol.c_str(), &st) == 0 ? (long)st.st_mtime : 0;
}

static void eski_sil(const std::string& dizin, int tavan) {
    std::vector<std::pair<long,std::string> > liste;
    DIR* d = opendir(dizin.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0]=='.') continue;
        std::string yol = dizin + "/" + e->d_name;
        if (yol.size()>4 && yol.compare(yol.size()-4,4,".jpg")==0)
            liste.push_back(std::make_pair(dosya_zamani(yol), yol));
    }
    closedir(d);
    if ((int)liste.size() <= tavan) return;
    std::sort(liste.begin(), liste.end());
    for (size_t i=0; i + tavan < liste.size(); i++) unlink(liste[i].second.c_str());
}

static int gozet(ncnn::Net& net, const Ayar& a, const std::string& izlenen,
                 const std::string& olay_dizin, int soguma, int tavan,
                 const std::set<int>& istenen)
{
    mkdir(olay_dizin.c_str(), 0755);
    std::string durum_yol = olay_dizin + "/son-olay.txt";
    std::string akis_yol  = olay_dizin + "/olaylar.jsonl";

    std::map<int,long> son_olay;
    { FILE* f = fopen(durum_yol.c_str(),"r");
      if (f) { int c; long t; while (fscanf(f,"%d %ld",&c,&t)==2) son_olay[c]=t; fclose(f); } }

    std::set<std::string> gorulen;
    fprintf(stderr,"gozet: %s -> %s  esik=%.2f soguma=%ds tavan=%d\n",
            izlenen.c_str(), olay_dizin.c_str(), a.esik, soguma, tavan);

    for (;;) {
        DIR* d = opendir(izlenen.c_str());
        if (!d) { sleep(5); continue; }
        std::vector<std::string> yeni;
        struct dirent* e;
        while ((e = readdir(d))) {
            if (e->d_name[0]=='.') continue;
            std::string ad = e->d_name;
            if (ad.size()<4) continue;
            std::string uz = ad.substr(ad.size()-4);
            if (uz!=".jpg" && uz!=".png" && uz!="jpeg") continue;
            if (gorulen.count(ad)) continue;
            yeni.push_back(ad);
        }
        closedir(d);
        std::sort(yeni.begin(), yeni.end());

        for (size_t i=0;i<yeni.size();i++) {
            std::string yol = izlenen + "/" + yeni[i];
            gorulen.insert(yeni[i]);
            std::vector<Kutu> k; double ms; int gw,gh;
            if (calistir(net, yol.c_str(), a, k, &ms, &gw, &gh) != 0) continue;
            long simdi = (long)time(NULL);

            /* Olay olcutu: istenen siniflardan biri esigin ustunde VE o sinif
             * icin soguma dolmus.  Ikisi de saglanmazsa kare islenir ama olay
             * yazilmaz - islem bosa degil, gurultu yazilmiyor. */
            std::set<int> tetikleyen;
            for (size_t j=0;j<k.size();j++) {
                int c = k[j].sinif;
                if (!istenen.empty() && !istenen.count(c)) continue;
                if (simdi - son_olay[c] < soguma) continue;
                tetikleyen.insert(c);
            }
            if (tetikleyen.empty()) continue;

            for (std::set<int>::iterator it=tetikleyen.begin(); it!=tetikleyen.end(); ++it)
                son_olay[*it] = simdi;

            char hedef[512];
            snprintf(hedef,sizeof hedef,"%s/%ld-%s", olay_dizin.c_str(), simdi, yeni[i].c_str());
            { FILE* g=fopen(yol.c_str(),"rb"); FILE* h=fopen(hedef,"wb");
              if (g&&h){ char b[8192]; size_t n; while((n=fread(b,1,sizeof b,g))>0) fwrite(b,1,n,h); }
              if (g) fclose(g); if (h) fclose(h); }

            FILE* f = fopen(akis_yol.c_str(),"a");
            if (f) { json_yaz(f, hedef, gw, gh, ms, a, k, simdi); fclose(f); }
            json_yaz(stdout, hedef, gw, gh, ms, a, k, simdi);
            fflush(stdout);

            { FILE* g=fopen(durum_yol.c_str(),"w");
              if (g){ for(std::map<int,long>::iterator it=son_olay.begin(); it!=son_olay.end(); ++it)
                        fprintf(g,"%d %ld\n", it->first, it->second); fclose(g); } }
            eski_sil(olay_dizin, tavan);
        }
        /* Islenmis dosya adlarini sinirsiz tutmak sizinti olurdu; dizin zaten
         * doner (kare ureten taraf eskisini siliyor), o yuzden set buyuyunce
         * artik var olmayanlari at. */
        if (gorulen.size() > 4096) {
            std::set<std::string> kalan;
            for (std::set<std::string>::iterator it=gorulen.begin(); it!=gorulen.end(); ++it)
                if (access((izlenen + "/" + *it).c_str(), F_OK) == 0) kalan.insert(*it);
            gorulen.swap(kalan);
        }
        sleep(2);
    }
    return 0;
}

int main(int argc,char**argv){
    if(argc<4){
        fprintf(stderr,
          "kullanim:\n"
          "  tek kare : %s <param> <bin> <resim.jpg> [esik] [is_parcacigi]\n"
          "  gozetle  : %s <param> <bin> --gozet <dizin> [esik] [is] [siniflar] [soguma_sn] [tavan]\n"
          "\nornek:\n"
          "  %s yolo-int8.param yolo-int8.bin kare.jpg 0.40 2\n"
          "  %s yolo-int8.param yolo-int8.bin --gozet /data/kare 0.45 2 person,car 60 200\n"
          "\n'siniflar' bos ya da '-' ise her sinif olay sayilir.\n", argv[0],argv[0],argv[0],argv[0]);
        return 1;
    }
    const char* param=argv[1]; const char* bin=argv[2];
    int gozet_kip = strcmp(argv[3],"--gozet")==0;

    Ayar a;
    a.esik = 0.35f; a.is = 2;
    const char* hedef = argv[3];
    std::string izlenen, siniflar_s="-";
    int soguma=60, tavan=200;
    if (gozet_kip) {
        if (argc<5){ fprintf(stderr,"--gozet icin dizin lazim\n"); return 1; }
        izlenen = argv[4];
        if(argc>5) a.esik=atof(argv[5]);
        if(argc>6) a.is=atoi(argv[6]);
        if(argc>7) siniflar_s=argv[7];
        if(argc>8) soguma=atoi(argv[8]);
        if(argc>9) tavan=atoi(argv[9]);
    } else {
        if(argc>4) a.esik=atof(argv[4]);
        if(argc>5) a.is=atoi(argv[5]);
    }

    ncnn::Net net;
    /* Bu cihazda olculdu: winograd ve sgemm bellek trafigini artiriyor,
     * lightmode ara bloblari erken birakiyor.  Varsayilanlar hesabin pahali
     * oldugu makineler icin secilmis; burada tavan bellek. */
    net.opt.lightmode = true;
    net.opt.num_threads = a.is;
    net.opt.use_int8_inference = true;
    net.opt.use_vulkan_compute = false;
    if(net.load_param(param)||net.load_model(bin)){ fprintf(stderr,"model yuklenemedi\n"); return 1; }

    if (gozet_kip) {
        std::set<int> istenen;
        if (siniflar_s!="-" && !siniflar_s.empty()) {
            std::string p; 
            for (size_t i=0;i<=siniflar_s.size();i++) {
                if (i==siniflar_s.size() || siniflar_s[i]==',') {
                    for (int c=0;c<80;c++) if (p==SINIFLAR[c]) istenen.insert(c);
                    p.clear();
                } else p += siniflar_s[i];
            }
            if (istenen.empty()) { fprintf(stderr,"tanimadigim sinif: %s\n", siniflar_s.c_str()); return 1; }
        }
        /* Olay dizini izlenen dizinin KARDESI, "/../" ile degil dogrudan:
         * uretilen JSON'daki yollar bir insanin ve bir toplayicinin okuyacagi
         * sey ve icinde ".." gecen bir yol ikisini de yavaslatiyor. */
        std::string olay = izlenen;
        while (!olay.empty() && olay[olay.size()-1] == '/') olay.erase(olay.size()-1);
        olay += "-olay";
        return gozet(net, a, izlenen, olay, soguma, tavan, istenen);
    }

    std::vector<Kutu> k; double ms; int gw,gh;
    if (calistir(net, hedef, a, k, &ms, &gw, &gh) != 0) {
        fprintf(stderr,"resim okunamadi: %s\n", hedef); return 1;
    }
    json_yaz(stdout, hedef, gw, gh, ms, a, k, (long)time(NULL));
    return 0;
}
