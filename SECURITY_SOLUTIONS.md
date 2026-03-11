# FirmwareUpdate_RF — Güvenlik Çözüm Rehberi

**Tarih:** 2026-03-10
**Bağlı Belge:** SECURITY_REPORT.md
**Amaç:** Tespit edilen 20 güvenlik bulgusunun teknik çözüm yolları ve uygulama adımları

> Bu belge, her bulgu için somut kod örnekleri, uygulama adımları ve doğrulama
> yöntemlerini içermektedir. Çözümler bağımlılık sırasına göre düzenlenmiştir;
> bir çözüm uygulanmadan sonrakine geçilmemesi önerilir.

---

## UYGULAMA SIRASI

```
[Hafta 1]
  K5 → K4 → K3 → Y1 → Y3 → Y5 → O7 → D3

[Hafta 2]
  K1 → K2 → O3

[Hafta 3–4]
  Y2 → Y4 → O1 → O2 → O4 → O6

[Ay 2–3]
  D1 → D2 → O5 (üretim) → uzun vadeli mimari
```

---

## KRİTİK ÇÖZÜMLER

---

## K5 — Özel Anahtar Dosyasını Git Geçmişinden Temizle

**Neden önce bu?** En az çaba, en yüksek aciliyet. Diğer her şeyden önce yapılmalı.

### Adım 1: .gitignore Güncelle

Projenin kök dizinindeki `.gitignore` dosyasına (yoksa oluştur):

```gitignore
# Kriptografik anahtar dosyaları — ASLA commit etme
*.pem
*.key
*.p12
*.pfx
*_private*.txt
private_key*.txt
public_key_bytes.txt

# Google servis hesabı
*.json
!package.json
!requirements*.json

# Config dosyaları (şifreli olsa bile)
config.enc
credentials.enc
```

### Adım 2: Mevcut Dosyaları Depozitordan Kaldır

```bash
# Disk'ten SILMEDEN sadece git takibinden çıkar
git rm --cached Uploader/private_key.pem
git rm --cached Uploader/public_key_bytes.txt

# Commit et
git commit -m "security: kriptografik anahtar dosyaları git takibinden çıkarıldı"
```

### Adım 3: Git Geçmişini Temizle (Eğer Daha Önce Commit Edildiyse)

```bash
# BFG Repo-Cleaner (Java gerektirir, git filter-branch'tan daha hızlı)
# https://rtyley.github.io/bfg-repo-cleaner/

java -jar bfg.jar --delete-files private_key.pem
java -jar bfg.jar --delete-files public_key_bytes.txt
git reflog expire --expire=now --all
git gc --prune=now --aggressive
git push --force  # DİKKAT: Tüm takımı bilgilendir
```

**Alternatif — git filter-branch (BFG yoksa):**

```bash
git filter-branch --force --index-filter \
  "git rm --cached --ignore-unmatch Uploader/private_key.pem" \
  --prune-empty --tag-name-filter cat -- --all

git push origin --force --all
```

### Adım 4: Yeni Anahtar Üret, Eski Anahtarı İptal Et

Eski `private_key.pem` artık güvensiz kabul edilmelidir. Yeni anahtar çifti üret:

```bash
cd Uploader
python key_gen.py
# Yeni public_key_bytes.txt içeriğini rf_bootloader.h'e yapıştır
# Yeni private_key.pem güvenli konumda sakla (aşağıya bak)
```

### Adım 5: Güvenli Anahtar Saklama

```
Windows:  %APPDATA%\FirmwareUpdater\private_key.pem  (sadece o kullanıcıya özel)
Linux:    ~/.config/firmware-updater/private_key.pem  (chmod 600)
MacOS:    ~/Library/Application Support/FirmwareUpdater/private_key.pem
```

`uploder.py`'de yol:

```python
import os
import platform

def get_private_key_path():
    system = platform.system()
    if system == "Windows":
        base = os.environ.get("APPDATA", os.path.expanduser("~"))
        return os.path.join(base, "FirmwareUpdater", "private_key.pem")
    elif system == "Darwin":
        return os.path.expanduser("~/Library/Application Support/FirmwareUpdater/private_key.pem")
    else:
        return os.path.expanduser("~/.config/firmware-updater/private_key.pem")
```

---

## K4 + O5 — Donanım Okuma ve Yazma Korumasını Etkinleştir

**Uyarı:** Bu işlem geri alınabilir (Level 1) ve geri alınamaz (Level 2) seçenekleri içerir.
Geliştirme ve test aşamasında Level 1 yeterlidir.

### STM32CubeProgrammer ile RDP Level 1

```
1. STM32CubeProgrammer'ı aç
2. Cihazı ST-Link ile bağla
3. Sol menü → "OB" (Option Bytes) sekmesi
4. "Read Out Protection" bölümü:
   - Mevcut: BB (Level 0 - korumasız)
   - Değiştir: CC (Level 1 - okuma korumalı)
5. "Apply" → Onayla
```

### Bootloader Yazma Koruması (WRP)

```
OB → Write Protection → Pages seçimi:
  STM32F030CC flash düzeni:
    Sayfa 0-3  → 0x08000000–0x08001FFF (8KB) = Bootloader başı
    Sayfa 4-7  → 0x08002000–0x08003FFF (8KB) = Bootloader sonu

  "Pages 0-7" için WRP'yi etkinleştir → Apply
```

### Programatik RDP Kontrolü (Boot'ta Doğrula)

`boot_flow.c`'ye eklenecek güvenlik kontrolü:

```c
/* boot_flow.c başına ekle — main() çağrılmadan önce */
static void check_rdp_level(void) {
#ifndef DEBUG
    /* Üretim derlemesinde RDP Level 1 zorunlu */
    FLASH_OBProgramInitTypeDef ob;
    HAL_FLASHEx_OBGetConfig(&ob);

    if (ob.RDPLevel == OB_RDP_LEVEL_0) {
        /* RDP aktif değil — hata LED'i yak, dur */
        /* Bu noktada cihaz üretim için hazır değil */
        NeoPixel_SetAll(255, 0, 0);
        NeoPixel_Show();
        while (1) {
            HAL_IWDG_Refresh(&hiwdg);
            HAL_Delay(500);
        }
    }
#endif
}
```

### RDP Sonrası Geliştirme Akışı

```
Test/Geliştirme:
  - RDP Level 0 (SWD açık)
  - DEBUG sembolü tanımlı
  - flash ve debug tam erişim

Üretim Öncesi Test:
  - RDP Level 1
  - NDEBUG tanımlı
  - Firmware yükleme yalnızca RF ile

Üretim:
  - RDP Level 1 (zorunlu)
  - WRP bootloader sayfaları
  - RF firmware güncelleme aktif
```

---

## K3 — Metadata Boyut Doğrulaması ve Sınır Kontrolü

### boot_flow.c — Metadata Alım Bloğu (Mevcut)

```c
// MEVCUT — GÜVENSİZ
if (rx_type == RF_CMD_METADATA && rx_pld_len >= 12) {
    memcpy(&metadata, rx_pld, sizeof(Firmware_Metadata_t));
    total_packets = (metadata.firmware_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE;
    got_metadata = 1;
    RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
}
```

### Güvenli Versiyon

```c
// YENİ — KAPSAMLI DOĞRULAMA
#define RF_ERR_INVALID_METADATA 0x20
#define RF_ERR_VERSION_TOO_OLD  0x21
#define RF_ERR_SIZE_TOO_LARGE   0x22
#define RF_ERR_SIZE_ZERO        0x23

/* Mevcut kurulu firmware versiyonunu oku */
static uint32_t get_installed_version(void) {
    uint32_t ver = *(volatile uint32_t *)VERSION_ADDRESS;
    /* Flash silinmişse 0xFFFFFFFF döner — 0 kabul et */
    return (ver == 0xFFFFFFFF) ? 0 : ver;
}

/* Metadata doğrulama fonksiyonu — TRUE: geçerli, FALSE: reddet */
static uint8_t validate_metadata(const Firmware_Metadata_t *meta,
                                  uint8_t *err_code) {
    /* 1. Boyut sıfır kontrolü */
    if (meta->firmware_size == 0) {
        *err_code = RF_ERR_SIZE_ZERO;
        return 0;
    }

    /* 2. Boyut üst sınır — uygulama alanını aşmamalı */
    if (meta->firmware_size > APP_AREA_SIZE) {
        *err_code = RF_ERR_SIZE_TOO_LARGE;
        return 0;
    }

    /* 3. Minimum mantıklı boyut — vektör tablosu en az 192 byte */
    if (meta->firmware_size < 192) {
        *err_code = RF_ERR_INVALID_METADATA;
        return 0;
    }

    /* 4. Versiyon sıfır kontrolü */
    if (meta->firmware_version == 0) {
        *err_code = RF_ERR_INVALID_METADATA;
        return 0;
    }

    /* 5. Sürüm geri alma koruması */
    uint32_t current_ver = get_installed_version();
    if (current_ver > 0 && meta->firmware_version < current_ver) {
        *err_code = RF_ERR_VERSION_TOO_OLD;
        return 0;
    }

    /* 6. Taşma kontrolü — toplam_paket hesabı integer overflow'a sebep olmamalı */
    /* (firmware_size / FW_PACKET_SIZE) + 1 uint32_t'yi aşmamalı */
    if (meta->firmware_size > (0xFFFFFFFFUL - FW_PACKET_SIZE + 1)) {
        *err_code = RF_ERR_SIZE_TOO_LARGE;
        return 0;
    }

    return 1;  /* Geçerli */
}

/* Metadata alım bloğu — güvenli versiyon */
if (rx_type == RF_CMD_METADATA && rx_pld_len >= 12) {
    Firmware_Metadata_t rx_meta;
    memcpy(&rx_meta, rx_pld, sizeof(Firmware_Metadata_t));

    uint8_t meta_err = 0;
    if (!validate_metadata(&rx_meta, &meta_err)) {
        RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                      &meta_err, 1);
        LED_Error();
        return;  /* Veya NVIC_SystemReset() */
    }

    memcpy(&metadata, &rx_meta, sizeof(Firmware_Metadata_t));
    total_packets = (metadata.firmware_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE;
    got_metadata = 1;
    RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
}
```

### Resume Bitmap Taşma Koruması

```c
/* Resume_SavePageDone çağrısı öncesi sınır kontrolü */
if (packets_received % PACKETS_PER_PAGE == 0) {
    uint32_t page_done = (packets_received / PACKETS_PER_PAGE) - 1;

    /* Bitmap sınırı: MAX_PAGES = APP_AREA_SIZE / FLASH_PAGE_SIZE = 111 */
    if (page_done < MAX_FIRMWARE_PAGES) {
        Resume_SavePageDone(page_done);
    }
    /* Sınır dışı → sessizce görmezden gel (bu olmamalı ama güvenli tarafta kal) */
}
```

---

## Y1 — Firmware Sürüm Geri Alma Koruması

Yukarıdaki `validate_metadata()` fonksiyonuna dahil edilmiştir. Ek olarak sürüm
sıfırlanamaz (monotonic) sayaç mimarisi:

### Flash'ta Monotonic Sürüm Sayacı

```c
/* rf_bootloader.h'e ekle */
#define ANTI_ROLLBACK_ADDRESS  (RESUME_STATE_ADDRESS + 256)  /* Ayrı offset */
#define ANTI_ROLLBACK_MAGIC    0xA07B3C5DUL
#define MAX_VERSION_SLOTS      16  /* Her slot bir sürüm artışı */

/* Yapı: [MAGIC(4)][version_slot_0(4)][version_slot_1(4)]...[slot_15(4)] */
/* Her slot: 0x00000000 = kullanıldı, 0xFFFFFFFF = boş */
/* Minimum versiyon = dolu slot sayısı */

uint32_t Anti_Rollback_GetMinVersion(void) {
    uint32_t magic = *(volatile uint32_t *)ANTI_ROLLBACK_ADDRESS;
    if (magic != ANTI_ROLLBACK_MAGIC) {
        return 0;  /* Henüz başlatılmamış */
    }

    uint32_t min_ver = 0;
    for (int i = 0; i < MAX_VERSION_SLOTS; i++) {
        uint32_t slot = *(volatile uint32_t *)(ANTI_ROLLBACK_ADDRESS + 4 + i * 4);
        if (slot == 0x00000000) {
            min_ver++;  /* Bu slot kullanıldı */
        }
    }
    return min_ver;
}

void Anti_Rollback_Increment(void) {
    /* Bir sonraki boş slotu sıfırla (0xFFFFFFFF → 0x00000000) */
    for (int i = 0; i < MAX_VERSION_SLOTS; i++) {
        uint32_t *slot = (uint32_t *)(ANTI_ROLLBACK_ADDRESS + 4 + i * 4);
        if (*slot == 0xFFFFFFFF) {
            HAL_FLASH_Unlock();
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              (uint32_t)slot, 0x00000000);
            HAL_FLASH_Lock();
            return;
        }
    }
    /* Tüm slotlar doldu → sayfa sil ve yeniden başlat (16 sürüm limiti) */
}
```

### validate_metadata() içinde Kullanım

```c
/* Monotonic counter kontrolü */
uint32_t min_version = Anti_Rollback_GetMinVersion();
if (meta->firmware_version < min_version) {
    *err_code = RF_ERR_VERSION_TOO_OLD;
    return 0;
}
```

### Başarılı Güncelleme Sonrası

```c
/* UPDATE_COMPLETE gönderilmeden hemen önce */
if (metadata.firmware_version > Anti_Rollback_GetMinVersion()) {
    Anti_Rollback_Increment();
}
```

---

## Y3 — DoS Koruması: Auth Başarısız Sayacı

### boot_flow.c — ADIM 1.5 Auth Döngüsü Güncellemesi

```c
/* Kalıcı başarısız sayaç — flash'ta saklanır */
#define AUTH_FAIL_COUNT_ADDRESS  (ANTI_ROLLBACK_ADDRESS + 128)
#define AUTH_FAIL_MAGIC          0xF41LC0DEUL
#define MAX_AUTH_FAILURES        5       /* Max başarısız deneme */
#define AUTH_LOCKOUT_SECONDS     300     /* 5 dakika kilitleme */

static uint32_t get_auth_fail_count(void) {
    uint32_t magic = *(volatile uint32_t *)AUTH_FAIL_COUNT_ADDRESS;
    if (magic != AUTH_FAIL_MAGIC) return 0;
    return *(volatile uint32_t *)(AUTH_FAIL_COUNT_ADDRESS + 4);
}

static void increment_auth_fail(void) {
    uint32_t count = get_auth_fail_count() + 1;
    HAL_FLASH_Unlock();
    /* Sayfayı sil ve yeniden yaz (bu alan için ayrı sayfa olmalı) */
    FLASH_PageErase(AUTH_FAIL_COUNT_ADDRESS);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, AUTH_FAIL_COUNT_ADDRESS, AUTH_FAIL_MAGIC);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, AUTH_FAIL_COUNT_ADDRESS + 4, count);
    HAL_FLASH_Lock();
}

static void reset_auth_fail(void) {
    HAL_FLASH_Unlock();
    FLASH_PageErase(AUTH_FAIL_COUNT_ADDRESS);
    HAL_FLASH_Lock();
}

/* ADIM 1.5 başında kontrol */
uint32_t fail_count = get_auth_fail_count();
if (fail_count >= MAX_AUTH_FAILURES) {
    /* Kilitleme süresi geçti mi? RTC yoksa IWDG sayacıyla yaklaşık hesap */
    /* Basit çözüm: X saniye bekle, sonra devam et */
    NeoPixel_SetAll(255, 0, 0);
    NeoPixel_Show();
    uint32_t lockout_start = HAL_GetTick();
    while ((HAL_GetTick() - lockout_start) < (AUTH_LOCKOUT_SECONDS * 1000UL)) {
        HAL_IWDG_Refresh(&hiwdg);
        HAL_Delay(1000);
    }
    reset_auth_fail();  /* Kilitleme süresi bitti, sıfırla */
}

/* ... auth döngüsü ... */

/* Auth başarısız olduğunda: */
else {
    RF_SendPacket(RF_CMD_AUTH_NACK, rx_seq, NULL, 0);
    nonce = generate_nonce();
    /* Sayacı artır */
    increment_auth_fail();
    /* Sayaç limiti aştıysa hemen kilitle */
    if (get_auth_fail_count() >= MAX_AUTH_FAILURES) {
        /* Hemen uygulamaya dön, bootloader'da bekleme */
        jump_to_application();
        return;
    }
}

/* Auth başarılı olduğunda: */
reset_auth_fail();
```

### main.c — AUTH_REQUEST Spam Koruması

```c
/* Uygulama döngüsü AUTH_REQUEST dinlerken spam koruması */
static uint8_t auth_req_count = 0;
static uint32_t auth_req_window_start = 0;

if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
    if (rx_type == RF_CMD_BOOT_REQUEST || rx_type == RF_CMD_AUTH_REQUEST) {

        /* Pencere kontrolü — 60 saniyede 3'ten fazla istek: reddet */
        uint32_t now = HAL_GetTick();
        if ((now - auth_req_window_start) > 60000) {
            /* Pencereyi yenile */
            auth_req_window_start = now;
            auth_req_count = 0;
        }

        auth_req_count++;
        if (auth_req_count > 3) {
            /* Çok fazla istek — uygulamaya geç, bootloader'a girme */
            jump_to_application();
        } else {
            Bootloader_Main();
            NVIC_SystemReset();
        }
    }
}
```

---

## Y5 — Metadata RF Paketi Bütünlük Denetimi

### Protokol Değişikliği

Metadata paketi boyutu 12 → 16 byte olacak: son 4 byte CRC-32.

**Sender (sender_fw_update.c):**

```c
/* ADIM 2: Metadata hazırla — CRC ekle */
uint8_t meta_buf[16];  /* 12 byte veri + 4 byte CRC */
if (HAL_UART_Receive(&huart1, meta_buf, 12, 10000) != HAL_OK) {
    /* ... hata ... */
}

/* CRC-32 hesapla ve ekle */
uint32_t meta_crc = 0xFFFFFFFF;
for (int i = 0; i < 12; i++) {
    meta_crc ^= meta_buf[i];
    for (int b = 0; b < 8; b++)
        meta_crc = (meta_crc >> 1) ^ ((meta_crc & 1) ? 0xEDB88320U : 0);
}
meta_crc ^= 0xFFFFFFFF;
memcpy(&meta_buf[12], &meta_crc, 4);  /* CRC'yi ekle */

/* RF ile gönder — artık 16 byte */
RF_SendPacket(RF_CMD_METADATA, rf_seq_counter, meta_buf, 16);
```

**Receiver (boot_flow.c):**

```c
if (rx_type == RF_CMD_METADATA && rx_pld_len >= 16) {  /* 12→16 */
    /* CRC doğrula */
    uint32_t recv_crc;
    memcpy(&recv_crc, &rx_pld[12], 4);

    uint32_t calc_crc = 0xFFFFFFFF;
    for (int i = 0; i < 12; i++) {
        calc_crc ^= rx_pld[i];
        for (int b = 0; b < 8; b++)
            calc_crc = (calc_crc >> 1) ^ ((calc_crc & 1) ? 0xEDB88320U : 0);
    }
    calc_crc ^= 0xFFFFFFFF;

    if (calc_crc != recv_crc) {
        /* CRC uyuşmazlığı — RF bozulması */
        RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0);
        continue;  /* BOOT_ACK döngüsünde tekrar bekle */
    }

    /* CRC geçerli → metadata doğrula */
    Firmware_Metadata_t rx_meta;
    memcpy(&rx_meta, rx_pld, 12);
    uint8_t meta_err = 0;
    if (!validate_metadata(&rx_meta, &meta_err)) {
        RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++, &meta_err, 1);
        return;
    }

    memcpy(&metadata, &rx_meta, 12);
    total_packets = (metadata.firmware_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE;
    got_metadata = 1;
    RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
}
```

**uploder.py — Metadata gönderme:**

```python
# Metadata: firmware_size(4) + version(4) + crc32(4) = 12 byte
meta_data_12 = (
    firmware_size.to_bytes(4, 'little') +
    firmware_version.to_bytes(4, 'little') +
    firmware_crc.to_bytes(4, 'little')
)
# CRC-32 hesapla ve ekle
meta_crc = zlib.crc32(meta_data_12) & 0xFFFFFFFF
metadata = meta_data_12 + meta_crc.to_bytes(4, 'little')  # 16 byte
ser.write(metadata)
```

---

## O7 — Hata Mesajlarını Debug/Production Ayrımıyla Kontrol Et

### sender_fw_update.c — Debug Flag

```c
/* sender_fw_update.c başına ekle */
#ifdef DEBUG
  #define FW_PRINT(msg)  Print(msg)
  #define FW_PRINTHEX(v) PrintHex(v)
#else
  #define FW_PRINT(msg)  /* üretimde log yok */
  #define FW_PRINTHEX(v)
#endif

/* Tüm Print("[FW]...") çağrılarını FW_PRINT() ile değiştir */
FW_PRINT("[FW] HATA: Auth CRC gecersiz!\r\n");  /* Artık üretimde görünmez */
```

### STM32CubeIDE'de Debug/Release Ayrımı

```
Project → Properties → C/C++ Build → Settings
  → MCU GCC Compiler → Preprocessor

Debug config:   Defined symbols: DEBUG
Release config: Defined symbols: NDEBUG  (DEBUG ekleme!)
```

---

## D3 — UART Arabelleği Temizleme (Race Condition Düzeltmesi)

### sender_fw_update.c — BOOT_ACK Onayından Sonra

```c
/* Alici hazir — PC'ye ACK gonder */
HAL_UART_Transmit(&huart1, &ack, 1, 100);

/* UART'ı temizle: RF işlemleri sırasında UART'a gelen baytlar
 * overrun (ORE) hatasına neden olmuş olabilir.
 * HAL_UART_Receive çağrısından önce hata bayraklarını sıfırla. */
{
    /* ORE (Overrun Error) bayrağını sıfırla */
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_OREFLAG(&huart1);
    }
    /* NE (Noise Error) sıfırla */
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_NE)) {
        __HAL_UART_CLEAR_NEFLAG(&huart1);
    }
    /* FE (Framing Error) sıfırla */
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_FE)) {
        __HAL_UART_CLEAR_FEFLAG(&huart1);
    }
    /* HAL hata kodunu sıfırla */
    huart1.ErrorCode = HAL_UART_ERROR_NONE;

    /* RXNE set ise (bir bayt bekliyor), oku ve at */
    while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE)) {
        volatile uint8_t dummy = (uint8_t)(huart1.Instance->RDR & 0xFF);
        (void)dummy;
    }
}

/* Şimdi güvenli şekilde metadata al */
uint8_t meta_buf[16];
if (HAL_UART_Receive(&huart1, meta_buf, 16, 10000) != HAL_OK) {
    /* ... */
}
```

---

## YÜKSEK ÖNCELİKLİ ÇÖZÜMLER

---

## K1 — Cihaza Özgü Anahtar Türetme (En Kritik Çözüm)

Bu çözüm; K1, K2, O3'ün temel mimarisini oluşturur.

### Konsept

```
[Üretim Hattında Bir Kez]
  → Master Secret (32 byte, rastgele) oluştur
  → Her cihaza ST-Link ile yaz (OTP veya şifreli flash sayfası)

[Cihaz Başına Benzersiz Anahtar Türetme]
  device_key_auth  = HMAC-SHA256(master_secret, UID + "auth")
  device_key_fw    = HMAC-SHA256(master_secret, UID + "fw")
  device_key_hmac  = HMAC-SHA256(master_secret, UID + "mac")

[Uploader Tarafında]
  → Cihaz UID'sini boot handshake'de al
  → Aynı türetmeyi yap
  → Cihaza özgü anahtarlarla şifrele/doğrula
```

### STM32 Benzersiz Cihaz ID (UID)

STM32F030'da 12-byte (96-bit) benzersiz fabrika ID'si vardır:

```c
/* rf_bootloader.h'e ekle */
#define STM32_UID_ADDRESS    0x1FFFF7ACU  /* STM32F0 UID adresi */
#define STM32_UID_SIZE       12           /* 96-bit = 12 byte */

/* UID'yi oku */
static void read_device_uid(uint8_t uid_out[12]) {
    memcpy(uid_out, (const void *)STM32_UID_ADDRESS, 12);
}
```

### Anahtar Türetme (HMAC tabanlı KDF)

```c
/* boot_flow.c'ye ekle */
#define MASTER_SECRET_ADDRESS  (BOOTLOADER_END - FLASH_PAGE_SIZE)  /* Son bootloader sayfası */
#define MASTER_SECRET_MAGIC    0x4D535443UL  /* "MSTC" */

/* Master secret + cihaz UID'den cihaza özgü anahtar türet */
static void derive_device_key(const uint8_t master[32],
                               const char  *context,
                               uint8_t      out_key[32]) {
    /* HMAC-SHA256(master, UID || context) */
    uint8_t uid[12];
    read_device_uid(uid);

    uint8_t msg[12 + 32];  /* UID + context (max 32 char) */
    memcpy(msg, uid, 12);

    size_t ctx_len = strlen(context);
    if (ctx_len > 32) ctx_len = 32;
    memcpy(msg + 12, context, ctx_len);

    /* HMAC-SHA256 hesapla */
    uint8_t k_ipad[64], k_opad[64];
    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5C, 64);
    for (int i = 0; i < 32; i++) {
        k_ipad[i] ^= master[i];
        k_opad[i] ^= master[i];
    }

    SHA256_CTX ctx;
    uint8_t inner[32];

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, k_ipad, 64);
    SHA256_Update(&ctx, msg, 12 + ctx_len);
    SHA256_Final(inner, &ctx);

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, k_opad, 64);
    SHA256_Update(&ctx, inner, 32);
    SHA256_Final(out_key, &ctx);
}

/* Kullanım */
void Bootloader_Main(void) {
    /* Master secret'ı flash'tan oku */
    const uint8_t *master = (const uint8_t *)MASTER_SECRET_ADDRESS;
    if (*(uint32_t *)MASTER_SECRET_ADDRESS != MASTER_SECRET_MAGIC) {
        /* Master secret yoksa hata — üretim hatası */
        LED_Error();
        return;
    }
    master += 4;  /* Magic'i atla */

    /* Cihaza özgü anahtarları türet */
    uint8_t device_auth_key[32];
    uint8_t device_fw_key[32];
    uint8_t device_hmac_key[32];

    derive_device_key(master, "auth-key-v1",   device_auth_key);
    derive_device_key(master, "firmware-v1",   device_fw_key);
    derive_device_key(master, "hmac-mac-v1",   device_hmac_key);

    /* Artık sabit DEFAULT_AUTH_KEY yerine device_auth_key kullan */
    /* ... */
}
```

### Üretim Hattı Programcısı (Python)

```python
# tools/provision_device.py — Üretim hattında çalıştırılır
import os
import struct
from cryptography.hazmat.primitives.hmac import HMAC
from cryptography.hazmat.primitives import hashes

MASTER_SECRET_MAGIC = 0x4D535443  # "MSTC"

def generate_master_secret():
    """32-byte kriptografik olarak güçlü master secret üret."""
    return os.urandom(32)

def provision_device(st_link_sn, master_secret):
    """
    Cihaza master secret yükle.
    st_link_sn: hangi ST-Link kullanılacak
    master_secret: 32-byte master key
    """
    # Veri: [MAGIC(4)][master_secret(32)]
    data = struct.pack('<I', MASTER_SECRET_MAGIC) + master_secret

    # STM32CubeProgrammer CLI ile yaz
    import subprocess
    hex_data = data.hex()
    result = subprocess.run([
        "STM32_Programmer_CLI",
        "-c", f"port=SWD sn={st_link_sn}",
        "-w", f"0x{MASTER_SECRET_ADDRESS:08X}", hex_data,
        "-v"  # Doğrula
    ], capture_output=True)

    return result.returncode == 0

def derive_device_key(master_secret, uid_hex, context):
    """Uploader tarafında cihaza özgü anahtar türet."""
    uid = bytes.fromhex(uid_hex)
    msg = uid + context.encode('utf-8')

    h = HMAC(master_secret, hashes.SHA256())
    h.update(msg)
    return h.finalize()

# Kullanım:
# master = generate_master_secret()
# provision_device("066FFF...", master)
# auth_key = derive_device_key(master, device_uid, "auth-key-v1")
```

---

## K2 — Geliştirilmiş Nonce Üretimi

### Mevcut Sorun ve Çözüm

STM32F030 gerçek RNG içermez. Birden fazla entropi kaynağı birleştirilerek
tahmin edilemezlik artırılabilir.

```c
/* boot_flow.c — Gelişmiş nonce üretimi */

/* 1. Flash'ta kalıcı nonce sayacı (reboot arasında artar) */
#define NONCE_COUNTER_ADDRESS  (ANTI_ROLLBACK_ADDRESS + 64)
#define NONCE_COUNTER_MAGIC    0x4E4F4E43UL  /* "NONC" */

static uint32_t get_and_increment_nonce_counter(void) {
    uint32_t magic = *(volatile uint32_t *)NONCE_COUNTER_ADDRESS;
    uint32_t counter = 0;

    if (magic == NONCE_COUNTER_MAGIC) {
        counter = *(volatile uint32_t *)(NONCE_COUNTER_ADDRESS + 4);
    }
    counter++;

    HAL_FLASH_Unlock();
    /* Sayfayı sil ve yeniden yaz */
    HAL_FLASHEx_Erase_Page(NONCE_COUNTER_ADDRESS);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, NONCE_COUNTER_ADDRESS,
                      NONCE_COUNTER_MAGIC);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, NONCE_COUNTER_ADDRESS + 4,
                      counter);
    HAL_FLASH_Lock();
    return counter;
}

/* 2. ADC gürültüsünden entropi */
static uint32_t adc_noise_entropy(void) {
    uint32_t entropy = 0;
    /* Bağlantısız ADC kanalını birkaç kez oku — termal gürültü */
    for (int i = 0; i < 16; i++) {
        HAL_ADC_Start(&hadc);
        HAL_ADC_PollForConversion(&hadc, 10);
        uint16_t val = HAL_ADC_GetValue(&hadc);
        HAL_ADC_Stop(&hadc);
        entropy = (entropy << 2) ^ val;  /* Alt 2 bit en gürültülü */
    }
    return entropy;
}

/* 3. STM32 Unique ID'den entropi */
static uint32_t uid_entropy(void) {
    const uint32_t *uid = (const uint32_t *)STM32_UID_ADDRESS;
    return uid[0] ^ uid[1] ^ uid[2];  /* 3 × 32-bit XOR */
}

/* Gelişmiş nonce üretici */
static uint32_t generate_nonce(void) {
    uint32_t t       = HAL_GetTick();
    uint32_t sv      = SysTick->VAL;
    uint32_t counter = get_and_increment_nonce_counter();
    uint32_t adc     = adc_noise_entropy();
    uint32_t uid_e   = uid_entropy();

    /* Tüm kaynakları birleştir */
    uint32_t mixed = t ^ (sv << 13) ^ (t >> 7) ^ counter ^ adc ^ uid_e;

    /* SHA-256 ile karıştır — daha iyi dağılım için */
    SHA256_CTX ctx;
    uint8_t digest[32];
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, (uint8_t *)&mixed, 4);
    SHA256_Final(digest, &ctx);

    /* İlk 4 byte'ı nonce olarak kullan */
    uint32_t nonce;
    memcpy(&nonce, digest, 4);
    return nonce;
}
```

---

## Y2 — Config Şifrelemesini Güçlendir

### config_manager.py — Güvenli Versiyon

```python
import os
import platform
import hashlib
from Crypto.Cipher import AES
from Crypto.Random import get_random_bytes
from Crypto.Util.Padding import pad, unpad

# PBKDF2 iterasyonunu artır (2026 NIST önerisi)
PBKDF2_ITERATIONS = 310_000  # 100_000 → 310_000

def _get_machine_secret():
    """
    OS'e özgü makine sırrı — statik _CRED_ENC_KEY yerine.
    Windows: DPAPI ile makineye bağlı
    Linux/Mac: /etc/machine-id + kullanıcı ev dizini hash'i
    """
    system = platform.system()

    if system == "Windows":
        try:
            import win32crypt
            # Boş veriyi şifrele → makineye özgü anahtar türet
            dummy = win32crypt.CryptProtectData(b"FirmwareUpdater_v1", None,
                                                 None, None, None, 0)
            key = hashlib.sha256(dummy).digest()
            return key
        except ImportError:
            pass  # win32crypt yoksa fallback

    # Linux/Mac: /etc/machine-id ve kullanıcıya özgü yol
    machine_id = b""
    try:
        with open("/etc/machine-id", "rb") as f:
            machine_id = f.read().strip()
    except FileNotFoundError:
        machine_id = os.environ.get("HOSTNAME", "unknown").encode()

    user_path = os.path.expanduser("~").encode()
    combined = machine_id + b":" + user_path + b":FirmwareUpdater_v1"
    return hashlib.sha256(combined).digest()

def _derive_key(password: str, salt: bytes) -> bytes:
    """PBKDF2-HMAC-SHA256 ile güçlü anahtar türet."""
    # Şifreyi makine sırrıyla güçlendir (pepper)
    pepper = _get_machine_secret()[:16]
    strengthened = password.encode('utf-8') + pepper

    return hashlib.pbkdf2_hmac(
        'sha256',
        strengthened,
        salt,
        PBKDF2_ITERATIONS,
        dklen=32
    )

def save_config(config: dict, admin_password: str):
    """Config'i güçlü şifrelemeyle kaydet."""
    # Her kayıtta yeni salt
    salt = get_random_bytes(32)  # 16 → 32 byte
    key = _derive_key(admin_password, salt)

    # AES-256-GCM (authenticated encryption — CBC'den daha güvenli)
    cipher = AES.new(key, AES.MODE_GCM)
    plaintext = json.dumps(config, ensure_ascii=False).encode('utf-8')
    ciphertext, tag = cipher.encrypt_and_digest(plaintext)

    # Format: [salt(32)][nonce(16)][tag(16)][ciphertext]
    with open(CONFIG_FILE, 'wb') as f:
        f.write(salt + cipher.nonce + tag + ciphertext)

def load_config(admin_password: str) -> dict:
    """Config'i çöz ve doğrula."""
    if not config_exists():
        return DEFAULT_CONFIG.copy()

    with open(CONFIG_FILE, 'rb') as f:
        data = f.read()

    salt = data[:32]
    nonce = data[32:48]
    tag = data[48:64]
    ciphertext = data[64:]

    key = _derive_key(admin_password, salt)

    try:
        cipher = AES.new(key, AES.MODE_GCM, nonce=nonce)
        plaintext = cipher.decrypt_and_verify(ciphertext, tag)
    except Exception:
        # Şifre yanlış veya veri bozuk — default döndürme, hata fırlat
        raise ValueError("Config çözülemedi: yanlış şifre veya bozuk dosya")

    loaded = json.loads(plaintext.decode('utf-8'))
    config = DEFAULT_CONFIG.copy()
    config.update(loaded)
    return config
```

### İlk Çalıştırma Şifre Zorunluluğu

```python
# gui_uploader_qt.py'de veya başlatma sırasında

DEFAULT_PASSWORD_SENTINEL = "__NOT_SET__"

def check_first_run():
    """İlk çalıştırmada güçlü şifre oluşturmayı zorla."""
    creds = load_credentials()
    if creds.get("is_default_password", True):
        # Şifre oluşturma diyaloğu göster
        show_password_setup_dialog()

def validate_password_strength(password: str) -> tuple[bool, str]:
    """Şifre karmaşıklık kontrolü."""
    if len(password) < 12:
        return False, "Şifre en az 12 karakter olmalı"
    if not any(c.isupper() for c in password):
        return False, "En az bir büyük harf gerekli"
    if not any(c.isdigit() for c in password):
        return False, "En az bir rakam gerekli"
    if not any(c in "!@#$%^&*()-_=+[]{}|;:,.<>?" for c in password):
        return False, "En az bir özel karakter gerekli"
    return True, ""
```

---

## Y4 — Admin Kimlik Bilgisi Güvenliği

### Sabit Değerleri Kaldır

```python
# config_manager.py — KALDIRILACAK
# _DEFAULT_ADMIN_PASSWORD = "admin"      ← SİL
# _DEFAULT_CRED_SALT = bytes.fromhex("...") ← SİL

# YENİ — İlk kurulumda oluştur
def initialize_credentials():
    """İlk kurulumda rastgele salt ve boş hash oluştur."""
    salt = get_random_bytes(32)
    # Şifre henüz belirlenmedi işaretleyici
    return {
        "salt": salt.hex(),
        "password_hash": None,  # Kurulum tamamlanana kadar None
        "is_setup_complete": False
    }

def set_admin_password(new_password: str, salt_hex: str) -> str:
    """Şifreyi güçlü hash ile kaydet."""
    salt = bytes.fromhex(salt_hex)
    valid, msg = validate_password_strength(new_password)
    if not valid:
        raise ValueError(msg)

    password_hash = hashlib.pbkdf2_hmac(
        'sha256',
        new_password.encode('utf-8') + _get_machine_secret()[:16],  # pepper
        salt,
        PBKDF2_ITERATIONS
    ).hex()
    return password_hash
```

---

## O1 — Auth Rate Limiting (Detaylı)

Yukarıdaki Y3 çözümünün ardından, auth penceresi için exponential backoff:

```c
/* boot_flow.c — ADIM 1.5 içinde */

/* Backoff süresi tablosu (ms) */
static const uint32_t AUTH_BACKOFF_MS[] = {
    0,     /* 0. deneme: bekleme yok */
    1000,  /* 1. başarısız: 1 saniye */
    5000,  /* 2. başarısız: 5 saniye */
    30000, /* 3. başarısız: 30 saniye */
    60000, /* 4. başarısız: 1 dakika */
};

uint32_t fail_count = get_auth_fail_count();
if (fail_count > 0 && fail_count < 5) {
    /* Backoff süresi bekle */
    uint32_t wait_ms = AUTH_BACKOFF_MS[
        (fail_count < 5) ? fail_count : 4
    ];
    uint32_t wait_start = HAL_GetTick();
    while ((HAL_GetTick() - wait_start) < wait_ms) {
        HAL_IWDG_Refresh(&hiwdg);
        /* Neopixel ile backoff göster */
        NeoPixel_SetAll(255, 165 - (fail_count * 30), 0);
        NeoPixel_Show();
        HAL_Delay(500);
        NeoPixel_Clear();
        NeoPixel_Show();
        HAL_Delay(500);
    }
}
```

---

## O2 — Resume Bitmap Bütünlük Kontrolü

### boot_storage.c — Bitmap CRC Koruması

```c
/* boot_storage.c'ye ekle */

#define BITMAP_CRC_OFFSET  (RESUME_PAGE_MAP_SIZE)  /* Bitmap'in hemen sonrası */

/* Bitmap CRC-32 hesapla */
static uint32_t calculate_bitmap_crc(void) {
    const uint8_t *bitmap = (const uint8_t *)RESUME_PAGE_MAP_ADDRESS;
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < RESUME_PAGE_MAP_SIZE; i++) {
        crc ^= bitmap[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
    }
    return crc ^ 0xFFFFFFFF;
}

/* Bitmap'i kaydet — CRC ile birlikte */
void Resume_SavePageDone(uint32_t page_index) {
    if (page_index >= MAX_FIRMWARE_PAGES) return;  /* Sınır kontrolü */

    /* Sayfa bitmap'i güncelle */
    /* ... mevcut bitmap yazma kodu ... */

    /* CRC'yi hesapla ve güncelle */
    uint32_t crc = calculate_bitmap_crc();
    HAL_FLASH_Unlock();
    /* CRC'yi bitmap'in hemen arkasına yaz */
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                      RESUME_PAGE_MAP_ADDRESS + BITMAP_CRC_OFFSET,
                      crc);
    HAL_FLASH_Lock();
}

/* Yükleme sırasında bitmap doğrula */
uint8_t Resume_VerifyBitmap(void) {
    uint32_t stored_crc = *(volatile uint32_t *)(
        RESUME_PAGE_MAP_ADDRESS + BITMAP_CRC_OFFSET
    );

    if (stored_crc == 0xFFFFFFFF) {
        /* CRC hiç yazılmamış — yeni başlangıç */
        return 1;  /* Kabul et */
    }

    uint32_t calc_crc = calculate_bitmap_crc();
    if (calc_crc != stored_crc) {
        /* Bitmap bozuk — tüm transfer'ı yeniden başlat */
        Resume_Clear();
        return 0;  /* Bozulma tespit edildi */
    }
    return 1;  /* Geçerli */
}
```

---

## O4 — Google Drive Kimlik Bilgileri Güvenliği

### config_manager.py ve gui_uploader_qt.py

```python
# config_manager.py — DEFAULT_CONFIG'i güncelle
DEFAULT_CONFIG = {
    # ...
    # Sabit yol yerine ortam değişkeni
    "service_account_json": os.environ.get(
        "FIRMWARE_SA_JSON_PATH",
        ""  # Boş bırak — kullanıcı GUI'den ayarlasın
    ),
}

# drive_manager.py — Güvenli yol doğrulaması
class DriveManager:
    def __init__(self, service_account_json: str):
        if not service_account_json:
            self._client = None
            return

        # Yol geçerlilik ve güvenlik kontrolü
        path = os.path.abspath(service_account_json)

        # Path traversal koruması
        home = os.path.expanduser("~")
        if not path.startswith(home) and not path.startswith("/etc/"):
            raise ValueError(f"Güvensiz servis hesabı yolu: {path}")

        if not os.path.isfile(path):
            raise FileNotFoundError(f"Servis hesabı dosyası bulunamadı: {path}")

        # Dosya izinleri kontrolü (Linux/Mac)
        if os.name != 'nt':
            mode = os.stat(path).st_mode & 0o777
            if mode & 0o044:  # Grup veya herkes okuyabiliyorsa uyar
                import warnings
                warnings.warn(
                    f"Servis hesabı dosyası çok açık izinlere sahip: {oct(mode)}. "
                    "chmod 600 önerilir."
                )
```

---

## O6 — Uygulamaya Atlamadan Önce HMAC Kontrolü

### Konsept

Başarılı her firmware güncellemesinden sonra, firmware'in HMAC değerini
boot flag sayfasında sakla. Her boot'ta bu değeri doğrula.

### boot_storage.c'ye Ekle

```c
#define STORED_HMAC_ADDRESS   (BOOT_FLAG_ADDRESS + 64)
#define STORED_HMAC_MAGIC     0x484D4143UL  /* "HMAC" */

void Boot_StoreVerifiedHMAC(const uint8_t hmac[32]) {
    HAL_FLASH_Unlock();
    /* Magic yaz */
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, STORED_HMAC_ADDRESS,
                      STORED_HMAC_MAGIC);
    /* HMAC'ı yaz (32 byte = 8 × 4 byte) */
    for (int i = 0; i < 8; i++) {
        uint32_t word;
        memcpy(&word, &hmac[i * 4], 4);
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          STORED_HMAC_ADDRESS + 4 + i * 4,
                          word);
    }
    HAL_FLASH_Lock();
}

uint8_t Boot_VerifyAppHMAC(const uint8_t hmac_key[32], uint32_t fw_size) {
    uint32_t magic = *(volatile uint32_t *)STORED_HMAC_ADDRESS;
    if (magic != STORED_HMAC_MAGIC) {
        return 1;  /* HMAC hiç saklanmamış (ilk boot) → geçer */
    }

    const uint8_t *stored_hmac = (const uint8_t *)(STORED_HMAC_ADDRESS + 4);

    /* Saklanan boyutu FW_PACKET_SIZE'a yuvarla */
    uint32_t padded_size = ((fw_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE) * FW_PACKET_SIZE;

    /* HMAC-SHA256 hesapla */
    SHA256_CTX ctx;
    uint8_t k_ipad[64], k_opad[64], inner[32], computed[32];
    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5C, 64);
    for (int i = 0; i < 32; i++) {
        k_ipad[i] ^= hmac_key[i];
        k_opad[i] ^= hmac_key[i];
    }

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, k_ipad, 64);
    SHA256_Update(&ctx, (const uint8_t *)APP_ADDRESS, padded_size);
    SHA256_Final(inner, &ctx);

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, k_opad, 64);
    SHA256_Update(&ctx, inner, 32);
    SHA256_Final(computed, &ctx);

    /* Sabit zamanlı karşılaştırma */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (computed[i] ^ stored_hmac[i]);
    return (diff == 0) ? 1 : 0;
}
```

### main.c'ye Ekle

```c
/* jump_to_application() çağrısından önce */
{
    uint32_t fw_size = *(volatile uint32_t *)VERSION_ADDRESS;
    /* Basit versiyon: sadece magic var mı kontrolü */
    /* Gelişmiş: metadata sayfasından firmware_size oku */
    if (fw_size != 0xFFFFFFFF) {
        uint8_t device_hmac_key[32];
        /* K1 çözümünden: cihaza özgü key türet */
        /* Şimdilik: HMAC_MAC_KEY kullan */
        if (!Boot_VerifyAppHMAC(HMAC_MAC_KEY, fw_size)) {
            /* HMAC başarısız — firmware değiştirilmiş! */
            NeoPixel_SetAll(255, 0, 255);
            NeoPixel_Show();
            /* Kurtarma modu: RF güncelleme bekle */
            Bootloader_Main();
            NVIC_SystemReset();
        }
    }
}
jump_to_application();
```

---

## D1 — Flash Tabanlı Transfer Günlüğü

### Yapı

```c
/* rf_bootloader.h'e ekle */
#define AUDIT_LOG_ADDRESS    (BOOT_FLAG_ADDRESS + 256)
#define AUDIT_LOG_MAGIC      0x4C4F4700UL  /* "LOG\0" */
#define AUDIT_LOG_MAX_ENTRIES 10
#define AUDIT_LOG_ENTRY_SIZE  32  /* Her giriş 32 byte */

typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;       /* HAL_GetTick() */
    uint32_t firmware_version;   /* Hedef versiyon */
    uint32_t firmware_size;      /* Byte sayısı */
    uint8_t  result;             /* 0=başarısız, 1=başarılı */
    uint8_t  error_code;         /* RF_ERR_xxx */
    uint8_t  packets_received;   /* Alınan paket sayısı (0-255) */
    uint8_t  auth_attempts;      /* Auth deneme sayısı */
    uint8_t  source_uid[12];     /* Gönderen cihaz UID (ileride) */
    uint8_t  reserved[8];        /* Gelecek kullanım */
} AuditLog_Entry_t;
```

### Yazma Fonksiyonu

```c
void AuditLog_Write(const AuditLog_Entry_t *entry) {
    /* Dairesel arabellek — 10 girişten sonra en eskiyi üzerine yaz */
    static uint32_t log_index = 0;

    uint32_t addr = AUDIT_LOG_ADDRESS + 4 +  /* magic */
                    (log_index % AUDIT_LOG_MAX_ENTRIES) * AUDIT_LOG_ENTRY_SIZE;

    HAL_FLASH_Unlock();

    /* İlk log girişinde magic yaz */
    if (log_index == 0) {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, AUDIT_LOG_ADDRESS, AUDIT_LOG_MAGIC);
    }

    /* Girişi yaz — 32 byte = 8 × 4 byte */
    const uint32_t *data = (const uint32_t *)entry;
    for (int i = 0; i < AUDIT_LOG_ENTRY_SIZE / 4; i++) {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i * 4, data[i]);
    }

    HAL_FLASH_Lock();
    log_index++;
}

/* Kullanım — boot_flow.c'de güncelleme sonunda */
AuditLog_Entry_t log_entry = {
    .timestamp_ms      = HAL_GetTick(),
    .firmware_version  = metadata.firmware_version,
    .firmware_size     = metadata.firmware_size,
    .result            = update_success ? 1 : 0,
    .error_code        = last_error_code,
    .packets_received  = (uint8_t)packets_received,
    .auth_attempts     = auth_attempt_count,
};
AuditLog_Write(&log_entry);
```

---

## UZUN VADELİ MİMARİ: ECDSA-P256 İLE GERİ DÖN

HMAC-SHA256 simetrik güvenlik sağlar; anahtara sahip olan hem imzalayabilir hem doğrulayabilir.
Uzun vadede, kaynak koda erişim kısıtlanıp RDP etkinleştirildikten sonra ECDSA-P256'ya geçiş
önerilir. Bu sayede:

- **Private key yalnızca PC'de** — cihaz asla private key görmez
- **Public key cihazda** — imza doğrulama için yeterli
- **RDP ile public key korunur** — okunsa bile, imza oluşturmak için kullanılamaz

### Hafif ECDSA Alternatifi: Ed25519

Ed25519, P-256'ya göre:
- Daha hızlı (Cortex-M0'da önemli)
- Daha küçük kod alanı (~3KB)
- Sabit zamanlı (yan kanal saldırılarına karşı daha dayanıklı)

**Kütüphane önerisi:** `fe25519` veya `donna-c` (açık kaynak, BSD lisansı, Cortex-M0 optimize)

---

## DOĞRULAMA KONTROL LİSTESİ

Her çözümün uygulanmasından sonra:

### K5 Doğrulama
```bash
git log --all --full-history -- "**/*.pem"
# Sonuç boş olmalı
```

### K4 Doğrulama
```
STM32CubeProgrammer → Bağlan → OB Oku
RDP Level: CC (Level 1) olmalı
```

### K3 Doğrulama
```c
/* Test: geçersiz metadata gönder */
uint8_t bad_meta[16] = {0xFF, 0xFF, 0xFF, 0xFF,  /* firmware_size = 0xFFFFFFFF */
                         0x01, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x00, ...};
/* Beklenen: UPDATE_FAILED + RF_ERR_SIZE_TOO_LARGE */
```

### Y1 Doğrulama
```c
/* Test: eski sürüm gönder */
metadata.firmware_version = 0;  /* v0 */
/* Beklenen: UPDATE_FAILED + RF_ERR_VERSION_TOO_OLD */
```

### Y3 Doğrulama
```python
# Test: 6 sahte AUTH_REQUEST gönder
for i in range(6):
    send_fake_auth_request()
    # 5. sonrasında cihaz bootloader'dan çıkmalı
```

### D3 Doğrulama
```
Osziloskop/Mantık Analizörü:
  UART TX (sender→PC) ve RX (PC→sender) izle
  Auth paketi alındıktan sonra:
    ORE bayrağı set olmamalı
    Metadata başarıyla alınmalı
```

---

## GELİŞTİRME ÖNERİLERİ

### Güvenli Geliştirme Ortamı

```bash
# .env dosyası (git'e dahil etme!)
FIRMWARE_MASTER_SECRET=<üretim için gerçek değer>
FIRMWARE_SA_JSON_PATH=/etc/firmware-updater/service-account.json
FIRMWARE_PRIVATE_KEY_PATH=/etc/firmware-updater/private_key.pem

# Python kodu
from dotenv import load_dotenv
load_dotenv()  # .env'den yükle
master = bytes.fromhex(os.environ["FIRMWARE_MASTER_SECRET"])
```

### CI/CD Güvenlik Kontrolleri

```yaml
# .github/workflows/security.yml (örnek)
- name: Gizli anahtar taraması
  uses: trufflesecurity/trufflehog@main
  with:
    path: ./
    base: main

- name: Sabit değer taraması
  run: |
    # Kaynak kodda bilinen test anahtarlarını ara
    grep -r "DEADBEEF" --include="*.py" --include="*.c" && exit 1 || exit 0
    grep -r "1234567890" --include="*.c" && exit 1 || exit 0
```

---

## ÖZET TABLO

| Bulgu | Çözüm | Dosyalar | Zorluk | Süre |
|-------|-------|---------|--------|------|
| K5 | .gitignore + git history temizle | - | Kolay | 30 dk |
| K4+O5 | RDP Level 1 + WRP | STM32CubeProgrammer | Kolay | 1 saat |
| K3 | validate_metadata() + sınır kontrolleri | boot_flow.c | Orta | 2 saat |
| Y1 | Versiyon karşılaştırma | boot_flow.c | Kolay | 1 saat |
| Y3 | Auth fail sayacı + backoff | boot_flow.c, main.c | Orta | 4 saat |
| Y5 | Metadata CRC-32 | boot_flow.c, sender_fw_update.c, uploder.py | Orta | 2 saat |
| O7 | Debug/Release flag | sender_fw_update.c | Kolay | 30 dk |
| D3 | UART ORE temizleme | sender_fw_update.c | Kolay | 30 dk |
| K1 | Cihaza özgü KDF | boot_flow.c, uploder.py, tools/ | Zor | 2 gün |
| K2 | Çoklu entropi kaynağı | boot_flow.c | Orta | 1 gün |
| Y2 | AES-GCM + OS keystore | config_manager.py | Orta | 1 gün |
| Y4 | İlk kurulum şifre zorunluluğu | config_manager.py, gui_uploader_qt.py | Orta | 4 saat |
| O1 | Exponential backoff | boot_flow.c | Orta | 3 saat |
| O2 | Bitmap CRC | boot_storage.c | Orta | 3 saat |
| O4 | Ortam değişkeni + izin kontrolü | config_manager.py, drive_manager.py | Kolay | 1 saat |
| O6 | Boot HMAC doğrulaması | main.c, boot_storage.c | Zor | 1 gün |
| D1 | Flash audit log | boot_flow.c, boot_storage.c | Orta | 4 saat |
| D2 | Timeout değerlerini azalt | uploder.py | Kolay | 30 dk |
| O3 | 64-bit nonce | boot_flow.c, sender_fw_update.c, uploder.py | Orta | 4 saat |

---

*Çözüm Rehberi Sonu — FirmwareUpdate_RF Güvenlik Çözümleri — 2026-03-10*
