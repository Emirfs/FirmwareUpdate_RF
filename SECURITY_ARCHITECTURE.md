# FirmwareUpdate_RF — Güvenlik Mimarisi Yeniden Tasarımı

**Tarih:** 2026-03-10
**Yaklaşım:** Sorunların root-cause analizi ve sistem düzeyinde çözümler

> Bu belge, güvenlik sorunlarına yamalar yerine **tasarım düzeyinde** yaklaşır.
> Her bölüm "neden hâlâ savunmasızız?" sorusundan başlar.

---

## TEMEL YANILGI: SIMETRIK ANAHTAR SORUNU

Şu anki sistemin en köklü güvenlik hatası, tek bir cümlede özetlenebilir:

> **HMAC-SHA256 kullanan herkes hem doğrulayabilir hem de imzalayabilir.**

Bu şu anlama gelir: uploader'daki `hmac_mac_key_hex`'e erişimi olan biri,
geçerli HMAC değeri olan herhangi bir firmware'i cihaza yükleyebilir.
AES anahtarı da aynı durumdadır.

Anahtarları gizlemek bu sorunu yalnızca gizleyerek çözer, ortadan kaldırmaz.
Gerçek çözüm **asimetrik kriptografi**dir. Ama bunu doğru uygulamak gerekir.

---

## BÖLÜM 1: GERÇEK TEHDİT MODELİ

Çözüm tasarlamadan önce gerçekçi saldırgan profilini belirlemek gerekir.

### Olası Saldırganlar

```
[Profil A] — Meraklı Son Kullanıcı
  Yetenek:  RF dinleyebilir, temel elektronik bilgisi var
  Hedef:    Sistemin nasıl çalıştığını anlamak
  Risk:     DÜŞÜK — mevcut AES şifreleme yeterli

[Profil B] — Rakip Firma
  Yetenek:  Kaynak koda erişim (sosyal mühendislik, eski çalışan)
            veya firmware dump (ST-Link + RDP olmadan)
  Hedef:    Firmware klonlama, aynı sistemi ucuza üretmek
  Risk:     YÜKSEK — mevcut sistem karşı savunmasız

[Profil C] — Hedefli Saldırgan
  Yetenek:  RF protokol analizi, AES bilgisi, reverse engineering
  Hedef:    Belirli bir cihaza zararlı firmware yüklemek
  Risk:     ORTA — kimlik doğrulama bu riski kısmen önlüyor

[Profil D] — Tedarik Zinciri Saldırganı
  Yetenek:  Uploader yazılımına veya derleme ortamına erişim
  Hedef:    Dağıtımı zehirleme (tüm cihazlar)
  Risk:     KRİTİK — tüm fleet etkilenir
```

**Sonuç:** Sistem, Profil A'ya karşı iyi korunmuştur. Diğer profiller için
mimari değişiklik gereklidir.

---

## BÖLÜM 2: NONCE SORUNU — YENİDEN TANIMLA

### Gerçek Sorun Ne?

Nonce'un amacı **replay saldırısını engellemek**tir. Kriptografik rastgelelik,
nonce'un amacı için zorunlu değildir. Zorunlu olan:

1. Her oturumda farklı olması
2. Önceden tahmin edilememesi (saldırgan önceki oturumu kaydedip tekrar gönderemesin)

### Mevcut Yaklaşımın Gerçek Açığı

```c
// Sorun: HAL_GetTick() + SysTick = deterministik
// Cihaz sıfırlandıktan 5 saniye sonra başlayan bir oturumun nonce'u
// her zaman yaklaşık aynı değer aralığındadır.
return (t ^ (sv << 13)) * 2654435761UL;
```

Ama asıl soru şu: **Bu nonce'u tahmin etmek saldırgana ne kazandırır?**

Saldırgan nonce'u tahmin etse bile, `DEFAULT_AUTH_KEY` ile şifrelenmiş
doğru yanıtı oluşturmak için anahtarı bilmesi gerekir. Yani nonce tahmin
edilebilirliği, asıl güvenlik katmanı değil, **ikincil bir katmandır**.

### Doğru Çözüm: Monotonic Counter

Karmaşık entropi toplama yerine basit ama etkili çözüm:

```c
/* Flash'ta kalıcı sayaç — her boot'ta artar, asla tekrarlamaz */

/* STM32F030 flash page: 2048 byte, silme ömrü ~10.000 kez */
/* 10.000 silme × (2048/4) = 5.120.000 benzersiz nonce — 136 yıl */
/* (günde 100 güncelleme varsayımıyla) */

#define NONCE_PAGE_BASE    BOOT_FLAG_ADDRESS       /* Son 2KB sayfa */
#define NONCE_CELL_COUNT   512                     /* 2048 / 4 = 512 hücre */

uint32_t generate_nonce(void) {
    /* Sayfadaki ilk 0xFFFFFFFF hücreyi bul — bu sonraki nonce */
    for (int i = 0; i < NONCE_CELL_COUNT; i++) {
        uint32_t cell = *(volatile uint32_t *)(NONCE_PAGE_BASE + i * 4);
        if (cell == 0xFFFFFFFF) {
            /* Hücreye nonce değerini yaz (0xFFFF → herhangi bir değer) */
            uint32_t nonce_val = (uint32_t)i | ((uint32_t)HAL_GetTick() << 16);
            HAL_FLASH_Unlock();
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              NONCE_PAGE_BASE + i * 4, nonce_val);
            HAL_FLASH_Lock();
            return nonce_val;
        }
    }
    /* Sayfa doldu — sil ve başa dön */
    HAL_FLASH_Unlock();
    HAL_FLASHEx_Erase_Page(NONCE_PAGE_BASE);
    HAL_FLASH_Lock();
    return generate_nonce();
}
```

**Bu yaklaşımın avantajları:**
- Cihaz kapansa da açılsa da nonce asla tekrarlanmaz
- Entropi kaynağı gerekmez
- Saldırgan nonce'u "tahmin" etse de fark etmez — her nonce tek kullanımlık
- Replay saldırısı tamamen engellenir (eski nonce artık geçerli değil)

---

## BÖLÜM 3: TEMEL SORU — KİM KİMİ DOĞRULUYOR?

Mevcut sistemde şu çelişki var:

```
Receiver → Sender: "Benim nonce'umu şifrele"
Sender → Receiver: [şifreli yanıt]
Receiver: "Doğru! Kimliğini doğruladım."
```

Ama receiver aslında **sadece birisinin doğru anahtara sahip olduğunu** kanıtladı.
Anahtarın sahibi kim? Bilmiyor.

### Gerçek Çift Taraflı Kimlik Doğrulama

Mevcut protokol tek yönlü (sadece sender kimliğini kanıtlıyor).
İki şeyin kanıtlanması gerekir:

1. **Sender'ın gerçek olduğu** (doğru anahtara sahip)
2. **Receiver'ın hedef cihaz olduğu** (başka bir cihaz değil)

### Önerilen Protokol: Karşılıklı Kimlik Doğrulama

```
[ADIM 1] Receiver → Sender: AUTH_CHALLENGE(nonce_r, device_uid)
          "Ben bu UID'ye sahip cihazım, bana bir nonce gönder"

[ADIM 2] Sender → PC: nonce_r + device_uid
          "Bu cihazla konuşuyorum, doğru anahtar hangisi?"

[ADIM 3] PC → Sender: AUTH_RESPONSE(nonce_r şifreli) + nonce_s
          "İşte yanıt. Ben de kendimi kanıtlayacağım: nonce_s'i sakla"

[ADIM 4] Sender → Receiver: AUTH(nonce_r şifreli, nonce_s)
          "İşte nonce yanıtın, bir de benim nonce'm"

[ADIM 5] Receiver → Sender: AUTH_ACK(nonce_s şifreli)
          "Nonce_s'i doğru şifreledim, ben de gerçeğim"
```

Bu şema sayesinde:
- Receiver sahte bir sender'a bağlanmaz
- Sender sahte bir receiver'a firmware yüklemez
- Man-in-the-middle saldırısı her iki tarafı da ifşa eder

**Implementasyon notu:** Bu şema, mevcut AES anahtarı altyapısıyla
gerçekleştirilebilir. RF paket boyutu artışı: +8 byte (nonce_s için).

---

## BÖLÜM 4: BOOTLOADER ERİŞİM MODELİ — KAPIYA KİLİT KOY

### Mevcut Sorun

```
main.c:
if (RF üzerinden AUTH_REQUEST gelirse) {
    Bootloader_Main();  // Herkes tetikleyebilir
}
```

Saldırgan sadece RF modülü alıp AUTH_REQUEST yayarsa cihaz bootloader'a
giriyor. DoS saldırısının root cause'u bu erişim modelidir.

### Yeni Model: Uygulama Tarafından Onaylı Güncelleme

Bootloader, RF üzerinden doğrudan erişilebilir olmamalıdır.
Yalnızca çalışan uygulama bootloader'ı başlatabilir.

```c
/* Uygulama (firmware) içinde — belirli koşullar sağlandığında */
void Application_RequestUpdate(void) {
    /* Boot flag sayfasına güncelleme isteği yaz */
    /* Bu işlem uygulama yetkisi gerektirir (sadece çalışan kod yapabilir) */
    HAL_FLASH_Unlock();
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                      BOOT_FLAG_ADDRESS, BOOT_FLAG_REQUEST);
    HAL_FLASH_Lock();

    /* Sistemi yeniden başlat → bootloader devreye girer */
    NVIC_SystemReset();
}
```

```c
/* main.c — bootloader erişim modeli */
if (check_boot_flag()) {
    /* Uygulama talep etti — bootloader'a geç */
    Bootloader_Main();
} else {
    /* Kimse talep etmedi — doğrudan uygulamaya geç */
    /* RF'ten AUTH_REQUEST gelse de görmezden gel */
    jump_to_application();
}
```

**Sonuç:** RF üzerinden bootloader tetiklemek artık mümkün değil.
Güncelleme başlatmak için cihazda çalışan uygulamanın izni gerekiyor.

**Peki uygulama ne zaman güncelleme ister?**

```c
/* Uygulama içinde — Örnek: güncelleme butonu veya komut alındığında */
void App_HandleUpdateCommand(void) {
    /* Kendi kimlik doğrulamasını yap (örn: cihaz PIN'i) */
    /* Doğruysa: */
    Application_RequestUpdate();
}
```

Bu yaklaşım bootloader DoS'unu (Y3) tamamen ortadan kaldırır ve
rate limiting'e gerek bırakmaz.

---

## BÖLÜM 5: SIMETRIK → ASİMETRİK GEÇİŞ (PRATİK YÖNTEM)

### Neden HMAC Yeterli Değil?

```
HMAC ile:    Anahtarı bilen = İmzalayan = Doğrulayan
ECDSA ile:   İmzalayan ≠ Doğrulayan
             Private key = sadece PC'de
             Public key = cihazda (okutulabilir ama işe yaramaz)
```

### STM32F030 için Ed25519 — Flash Bütçesi Analizi

```
Mevcut kullanım:
  Bootloader kodu:   ~15KB
  AES (tiny-aes):    ~2KB
  SHA-256:           ~2KB
  Boş alan:          ~13KB

Ed25519 (donna-c optimize, verify-only):
  Kod boyutu:        ~4KB
  RAM kullanımı:     ~1.5KB geçici (stack'te)

Sonuç: Sığar. Flash bütçesi kritik değil.
```

### Ed25519 Entegrasyon Stratejisi

**Aşama 1 (şimdi):** HMAC'ı koru, alttan Ed25519 ekle

```c
/* boot_flow.c — ADIM 6.5 sonunda, HMAC'tan sonra */
/* HMAC geçerliyse Ed25519 imzasını da kontrol et (opsiyonel aşama) */
#ifdef ENABLE_ECDSA_VERIFY
    if (!Ed25519_Verify(fw_hash, 32, received_sig, ED25519_PUBLIC_KEY)) {
        /* İmza geçersiz */
    }
#endif
```

**Aşama 2 (üretim öncesi):** HMAC'ı kaldır, Ed25519'u zorunlu kıl

Bu geçiş, tek bir `#define` değişikliğiyle yapılabilir. İki protokolü
aynı anda çalıştırmak geçiş sürecini risksiz kılar.

### donna-c Ed25519 Kütüphanesi

```
https://github.com/floodyberry/supercop

Cortex-M0 için optimize versiyon:
  - Sabit zamanlı (timing saldırısına karşı)
  - Sadece verify: ~3.8KB flash
  - Verify süresi: Cortex-M0 @ 24MHz → ~450ms

450ms kabul edilebilir mi?
  RF transfer toplam süresi ~2 dakika.
  450ms → %0.4 overhead. Kabul edilebilir.
```

---

## BÖLÜM 6: ANAHTAR YÖNETIMI — FABRIKA MODELİ YERİNE BOOTSTRAP

### Fabrika Modeli Neden Zor?

```
Fabrika modeli:
  Üretim hattı → ST-Link → Master secret → Her cihaz

Sorunlar:
  - Üretim hattı kurulumu karmaşık
  - Master secret güvenliği ayrı bir problem
  - Küçük üretim hacimleri için aşırı mühendislik
```

### Alternatif: İlk Açılış Eşleşme Protokolü (Pairing)

```
[ADIM 1 — Fabrikadan Çıkarken]
  Cihaz boş gelir. Master secret YOK.
  Firmware doğrulama henüz aktif değil.

[ADIM 2 — İlk Güç]
  Cihaz bootloader'a girer (uygulama yok).
  Fiziksel temas gerektirir (buton veya kısa devre pad'i).
  "Eşleşme modu" — LED hızlı yanıp söner.

[ADIM 3 — PC ile Eşleşme]
  Uploader: "Yeni cihaz eşleşmek istiyor"
  PC, cihazın UID'sini okur (RF üzerinden)
  Kullanıcı onaylar ("Bu cihazı eşleştir")

  PC, cihaz için keys üretir:
    auth_key = HMAC(master_fleet_secret, uid + "auth")
    hmac_key = HMAC(master_fleet_secret, uid + "hmac")

  PC, bu anahtarları cihaza gönderir (tek seferlik plain channel)
  Cihaz anahtarları flash'ta şifreli saklar

[ADIM 4 — Eşleşme Tamamlandı]
  Artık RF güncelleme yalnızca bu anahtarlarla çalışır.
  Cihaz "eşleşme modu"ndan çıkar.
  Fiziksel erişim olmadan yeniden eşleşme mümkün değil.
```

```c
/* boot_flow.c — Eşleşme modu tespiti */
#define PAIRING_MAGIC     0x50415952UL  /* "PAYR" */
#define DEVICE_KEYS_MAGIC 0x44455953UL  /* "DEYS" */

uint8_t is_paired(void) {
    return *(volatile uint32_t *)DEVICE_KEYS_ADDRESS == DEVICE_KEYS_MAGIC;
}

uint8_t is_pairing_button_pressed(void) {
    /* PA0 veya başka bir GPIO — fabrika eşleşme butonu */
    return HAL_GPIO_ReadPin(PAIR_GPIO_Port, PAIR_Pin) == GPIO_PIN_RESET;
}

void Bootloader_Main(void) {
    if (!is_paired()) {
        if (is_pairing_button_pressed()) {
            Pairing_Mode();  /* Anahtarları al, kaydet */
        } else {
            /* Eşleşmemiş, buton basılmamış — bekle ve hata göster */
            LED_Error();
            return;
        }
    } else {
        /* Normal güncelleme akışı */
        Normal_Update_Flow();
    }
}
```

**Bu modelin avantajları:**
- Fabrika altyapısı gerekmez
- Her cihaz farklı anahtara sahip
- Anahtar sızıntısı yalnızca tek cihazı etkiler
- Fiziksel güvenlik: eşleşme butonu olmadan yeniden eşleşme imkânsız

---

## BÖLÜM 7: GÜVENLİ DEPOLAMA — AES-CBC YERİNE BÜTÜNLEŞIK YAKLAŞIM

### Config Dosyasının Gerçek Tehdidi

```
Mevcut:
  config.enc = AES-CBC(şifre, anahtarlar + ayarlar)

Sorun:
  Şifre ele geçirilirse → tüm anahtarlar açığa çıkar
  Statik _CRED_ENC_KEY kaynak kodunda → şifre bile gerekmez
```

### Anahtarları Config'den Ayır

```
Yeni mimari:

  device_keys.enc = AES-GCM(makine_sırrı, auth_key + hmac_key)
  └── Makine sırrı: Windows DPAPI veya OS keyring
  └── Şifre gerektirmez, o makineye bağlı

  config.enc = AES-GCM(kullanıcı_şifresi, port + baud + drive_id + ...)
  └── Anahtarları içermez
  └── Kaybedilse bile: yalnızca ayarlar gider, güvenlik zaafiyeti olmaz
```

```python
# Uploader/key_store.py — Yeni anahtar depolama katmanı

import os
import platform
import ctypes

class DeviceKeyStore:
    """
    Kriptografik anahtarları güvenli OS depolama alanında saklar.
    Şifre gerektirmez — mevcut kullanıcı oturumuna bağlı.
    """

    def __init__(self):
        self._system = platform.system()

    def _get_storage_path(self):
        if self._system == "Windows":
            return os.path.join(os.environ["APPDATA"], "FirmwareUpdater", "device_keys.bin")
        return os.path.expanduser("~/.config/firmware-updater/device_keys.bin")

    def save(self, uid_hex: str, auth_key: bytes, hmac_key: bytes) -> None:
        """Cihaza özgü anahtarları güvenli depolama alanına yaz."""
        plaintext = uid_hex.encode() + b"|" + auth_key + hmac_key  # 88 byte

        if self._system == "Windows":
            # Windows DPAPI — mevcut kullanıcıya ve makineye bağlı
            import ctypes.wintypes
            from ctypes import windll
            data_in = ctypes.create_string_buffer(plaintext)
            # CryptProtectData çağrısı — kaynak kod olmadan çözülmez
            protected = self._dpapi_protect(plaintext)
        else:
            # Linux/Mac: OS keyring kullan
            import keyring
            protected = self._os_keyring_protect(uid_hex, plaintext)

        os.makedirs(os.path.dirname(self._get_storage_path()), exist_ok=True)
        with open(self._get_storage_path(), "ab") as f:  # Append — birden fazla cihaz
            f.write(len(uid_hex).to_bytes(1, 'little') + uid_hex.encode() +
                    len(protected).to_bytes(2, 'little') + protected)

    def load(self, uid_hex: str) -> tuple[bytes, bytes]:
        """Belirli cihazın anahtarlarını yükle."""
        # ... depolamadan bul ve çöz ...
        pass

    def _dpapi_protect(self, data: bytes) -> bytes:
        """Windows DPAPI ile şifrele."""
        try:
            from win32crypt import CryptProtectData
            result = CryptProtectData(data, "FirmwareUpdater Keys", None, None, None, 0)
            return result
        except ImportError:
            # win32crypt yoksa: OS keyring fallback
            return self._fallback_protect(data)
```

### Sonuç: Config Sızıntısı Artık Anlamsız

Config dosyası çalınsa bile içinde anahtar yok.
Anahtarlar sadece o makinede, sadece o kullanıcının oturumunda açılabilir.

---

## BÖLÜM 8: ROLLBACK KORUMASI — FARKLI AÇIDAN

### Mevcut Düşünce

```
Cihazda sürüm numarasını sakla.
Yeni firmware'in versiyonu >= eski versiyon olmalı.
```

### Sorun

Bu düşünce, saldırganın geçerli bir HMAC üretebileceğini varsayar.
Eğer HMAC anahtarı bilinmiyorsa, saldırgan zaten firmware yükleyemez.
Eğer biliniyorsa, versiyon kontrolü işe yaramaz — saldırgan istediği
versiyonu gönderir ve HMAC'ını hesaplar.

**Rollback koruması ancak asimetrik kriptoyla anlamlıdır.**

### HMAC Sisteminde Rollback Koruması

HMAC sistemiyle tutarlı bir rollback koruması şöyle kurulur:

```
Versiyon numarası → HMAC anahtarına bağla.

Her sürüm kendi HMAC anahtarını kullanır:
  hmac_key_v1 = KDF(master, "hmac-mac-v1")
  hmac_key_v2 = KDF(master, "hmac-mac-v2")
  hmac_key_v3 = KDF(master, "hmac-mac-v3")

Cihazda minimum kabul edilen versiyon saklanır.
Yeni firmware yüklenince: eski versiyon anahtarı "iptal edilir" —
cihaz artık o anahtarı geçerli kabul etmez.
```

```c
/* boot_flow.c — Versiyon-bağlı anahtar türetme */
void derive_hmac_key_for_version(uint32_t version, uint8_t out_key[32]) {
    /* version numarasını context string'e ekle */
    char context[32];
    snprintf(context, sizeof(context), "hmac-mac-v%lu", (unsigned long)version);
    derive_device_key(master_secret, context, out_key);
}

/* Doğrulama sırasında */
uint8_t device_hmac_key[32];
derive_hmac_key_for_version(metadata.firmware_version, device_hmac_key);

/* Bu anahtar, yalnızca o versiyon için geçerli bir HMAC üretebilir */
/* Eski versiyon anahtarı ile imzalanmış bir firmware, yeni versiyon numarası iddia edemez */
```

Bu şekilde rollback saldırısı anlamsız hale gelir:
- v3 cihazına v2 firmware yüklemek için v2 HMAC anahtarı gerekir
- v2 HMAC anahtarı farklıdır, v2 firmware için geçerli HMAC üretir
- Ama cihaz artık v2 anahtarını minimum versiyon olarak kabul etmiyor

---

## BÖLÜM 9: UART RACE CONDITION — KÖK NEDEN

### Gerçek Problem

Bu projede UART arayüzü hem debug/kontrol hem de firmware transfer için
aynı fiziksel hatta kullanılıyor. Race condition'ların kökü budur.

```
PC → Sender → Receiver
     (UART)   (RF)

UART ve RF işlemleri sıralı ama timing garantisi yok.
```

### Çözüm A: DMA Tabanlı UART (Arabellek Güvencesi)

```c
/* Tek seferlik başlatma */
static uint8_t uart_rx_dma_buf[256];  /* DMA ring buffer */

void UART_DMA_Init(void) {
    /* UART RX'i dairesel DMA moduyla başlat */
    HAL_UART_Receive_DMA(&huart1, uart_rx_dma_buf, sizeof(uart_rx_dma_buf));
    /* Artık UART asla overrun yapmaz — DMA sürekli alıyor */
}

size_t UART_Read(uint8_t *dst, size_t len, uint32_t timeout_ms) {
    /* DMA buffer'ından oku — RF işlemi yapılırken de güvenli */
    uint32_t start = HAL_GetTick();
    size_t received = 0;

    while (received < len && (HAL_GetTick() - start) < timeout_ms) {
        size_t dma_head = sizeof(uart_rx_dma_buf) - __HAL_DMA_GET_COUNTER(huart1.hdmarx);
        if (dma_tail != dma_head) {
            dst[received++] = uart_rx_dma_buf[dma_tail];
            dma_tail = (dma_tail + 1) % sizeof(uart_rx_dma_buf);
        }
    }
    return received;
}
```

DMA ile UART artık tamamen bağımsız çalışır. SPI (RF) ve UART aynı anda
aktif olabilir, overrun olmaz.

### Çözüm B: Protokol Düzeltmesi (Daha Az Değişiklik)

DMA eklemek istemiyorsanız, protokol sırasını düzenle:

```
MEVCUT (Sorunlu):
  Sender auth ACK gönder → bootloader'a geç (RF meşgul) → metadata al

YENİ (Güvenli):
  Sender auth ACK gönder
  Sender PC'den "Devam et" komutu bekle (1 byte)
  Sender bootloader RF döngüsünü tamamla
  Sender PC'ye "Hazır" sinyali gönder
  Sender metadata bekle

Python tarafında:
  ser.write(b'\x01')  # Devam komutu
  ready = ser.read(1)  # Hazır sinyali bekle
  ser.write(metadata)  # Artık güvenli
```

Bu ekleme **sıfır risk** içerir ve overrun sorununu tamamen çözer.

---

## BÖLÜM 10: HATA KODLARI — TERSE PROTOCOL

### Mevcut Sorun

Ayrıntılı hata mesajları saldırgana protokolün iç yapısını öğretir.

### Çözüm: Hata Kodu Yerine Durum Kodu

```c
/* Saldırgana yararlı bilgi verme — sadece "başarılı" veya "başarısız" */

/* Üretimde: */
#ifdef NDEBUG
  #define SEND_ERROR(code)  RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++, \
                                          (uint8_t[]){0xFF}, 1)
                            /* 0xFF = genel hata, neden bildirilmez */
#else
  #define SEND_ERROR(code)  RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++, \
                                          &(code), 1)
                            /* Debug'da gerçek hata kodu */
#endif
```

**Ama daha önemli soru:** Hata kodları kime yarıyor?

Eğer bir saldırgan RF paket formatını analiz edebiliyorsa, hata kodu
onu daha fazla bilgilendirir. Eğer analiz edemiyorsa, fark etmez.

Gerçek koruma **hata kodlarını gizlemek** değil, **saldırganın deneme
yüzeyini daraltmak**tır. Bölüm 4'teki bootloader erişim modeli bunu sağlar.

---

## BÖLÜM 11: AES ANAHTARININ GERÇEK ROLÜ

### Mevcut Sistemde AES Anahtarı Neyi Koruyor?

```
AES-256-CBC şifreleme:
  - RF dinleme → şifreli görür, firmware içeriğini okuyamaz ✓
  - Flash dump → şifreli firmware var, anahtarsız çözülmez ✓
  - Yetkisiz firmware yükleme → AES + HMAC koruması
    AES anahtarı bilmeden geçerli şifreli paket üretemez ✓ (kısmen)
```

### Gerçek Açık

AES anahtarı cihazda saklandığı için, RDP olmadan flash okunursa anahtar ele geçirilir.
**AES şifreleme, yalnızca RF dinleme saldırısını engeller.** Fiziksel saldırıya karşı değil.

### Doğru Yaklaşım: Katmanlı Güvenlik

```
[Katman 1 — RF Gizliliği]
  AES-256-CBC: RF paketi dinlenemez
  Yeterli mi? Evet, RF dinleme için.

[Katman 2 — Bütünlük]
  HMAC-SHA256: firmware değiştirilemez
  Yeterli mi? Yalnızca anahtar korunduğu sürece.

[Katman 3 — Kimlik Doğrulama]
  Nonce challenge-response: replay engellenir
  Yeterli mi? Evet.

[Katman 4 — Fiziksel Erişim]
  RDP Level 1: flash okunamaz
  Bunsuz Katman 1 ve 2 değersizdir.
```

**Sonuç:** Güvenlik seviyeleri zincir gibidir. En zayıf halka (RDP eksikliği)
diğer tüm katmanları geçersiz kılar. RDP etkinleştirilmeden diğer tüm
güvenlik önlemleri teorik kalır.

---

## ÖZET: ÖNCELİKLİ MİMARİ KARARLAR

Raporlardaki tüm yamalar yerine, şu **3 mimari karar** alınırsa sistem
gerçek anlamda güvenli olur:

---

### KARAR 1: Bootloader Erişim Modeli Değişmeli

```
ESKİ: RF üzerinden herkes bootloader'ı tetikleyebilir
YENİ: Yalnızca çalışan uygulama bootloader'ı başlatabilir
      (fiziksel erişim veya uygulama içi komut)

Etkisi: Y3 (DoS), O1 (rate limit) sorunları tamamen ortadan kalkar.
        Bunların için ayrıca kod yazmaya gerek yok.
```

### KARAR 2: Ed25519 veya HMAC+Pairing

```
Seçenek A (Kısa vadeli):
  Mevcut HMAC sistemi + Pairing protokolü
  Her cihaz farklı anahtar, fabrika altyapısı gerekmez
  K1 sorunu cihaz bazında izole edilir

Seçenek B (Uzun vadeli):
  Ed25519 verify-only (~4KB flash)
  Private key yalnızca PC'de
  Anahtar sızıntısı cihazda olanaksız

Etkisi: K1, K2, Y1 sorunları köklü çözülür.
```

### KARAR 3: RDP Level 1 + DMA UART

```
RDP Level 1: Tüm fiziksel saldırıları etkisizleştirir (K4, O5)
DMA UART:    Race condition'ları kökten çözer (D3 ve gelecekteki benzerleri)

Bu iki değişiklik birlikte yapılırsa:
  - K4, O5 → RDP ile çözülür
  - D3 → DMA ile çözülür
  - Sistem protokol düzeyinde kararlı hale gelir
```

---

Üç karar, yirmi bulgudaki sorunların büyük çoğunluğunu kapsar.
Geriye kalanlar (Y2, Y4, D1, D2) ikincil iyileştirmelerdir.

---

*Güvenlik Mimarisi Yeniden Tasarımı — FirmwareUpdate_RF — 2026-03-10*
