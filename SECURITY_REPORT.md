# FirmwareUpdate_RF — Güvenlik Analiz Raporu

**Tarih:** 2026-03-10
**Kapsam:** Tüm proje bileşenleri (alici_cihaz, uart_rf_gonderici, Uploader)
**Platform:** STM32F030CC / STM32F030C8 — Si4432 433 MHz RF, 9.6 kbps GFSK
**Analiz Türü:** Statik Kod Analizi + Manuel İnceleme

---

## YÖNETİCİ ÖZETİ

Bu rapor, RF tabanlı STM32 firmware güncelleme sisteminin tüm güvenlik risklerini ele almaktadır.
Sistem; kimlik doğrulama (AES-256-CBC nonce challenge-response), bütünlük denetimi (HMAC-SHA256)
ve şifreli transfer (AES-256-CBC) katmanlarına sahip olsa da, aşağıda belgelenen açıklıklar
üretim dağıtımından önce mutlaka giderilmelidir.

**Özet Sayımı:**

| Önem Derecesi | Adet |
|---------------|------|
| KRİTİK        | 5    |
| YÜKSEK        | 5    |
| ORTA          | 7    |
| DÜŞÜK         | 3    |
| **TOPLAM**    | **20** |

---

## RİSK MATRİSİ

```
ETKİ
  ^
  |  [K1] Sabit Anahtarlar         [K3] Firmware Boyutu Kontrolsüz
  |  [K2] Zayıf Nonce              [K4] RDP/WRP Devre Dışı
  |  [Y1] Rollback Yok             [K5] Özel Anahtar Proje İçinde
  |  [Y2] PBKDF2 Zayıf            [Y3] Metadata Doğrulama Eksik
  |  [O1] Rate Limit Yok          [Y4] Credential Zayıflığı
  |  [O2] Resume Bitmap            [Y5] CRC-32 Yeterli Değil
  |  [O3] Nonce Entropi            [O4] Servis Hesabı Açıkta
  |  [O5] SWD Açık                 [O6] Güvenli Boot Yok
  |  [D1] Hata Mesajları          [D2] Log Tutulmuyor
  |  [D3] Zaman Aşımı Değerleri
  +-------------------------------------------------> OLASALIK
```

---

## KRİTİK BULGULAR

---

### K1 — Gömülü Kriptografik Anahtarlar (Plaintext)

**Etkilenen Dosyalar:**
- `alici_cihaz/Core/Inc/rf_bootloader.h` — satır 219–257
- `alici_cihaz/Core/Src/boot_flow.c` — satır 96–99
- `Uploader/config_manager.py` — satır 57–60

**Teknik Detay:**

Sistemdeki tüm kriptografik anahtarlar kaynak kodunda açık metin olarak bulunmaktadır:

```c
// rf_bootloader.h — AES-256 Auth Anahtarı (32 byte, AÇIK METİN)
static const uint8_t DEFAULT_AUTH_KEY[32] = {
    0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18, ...
};

// rf_bootloader.h — Auth Şifresi (16 byte, AÇIK METİN)
static const uint8_t DEFAULT_AUTH_PASSWORD[16] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE, ...
};

// rf_bootloader.h — HMAC-SHA256 MAC Anahtarı (32 byte, AÇIK METİN)
static const uint8_t HMAC_MAC_KEY[32] = {
    0xF1, 0xE2, 0xD3, 0xC4, 0xB5, 0xA6, 0x97, 0x88, ...
};

// boot_flow.c — Firmware Şifreleme AES Anahtarı (ASCII "12345678901234..."!)
static const uint8_t DEFAULT_AES_KEY[32] = {
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, ...
};
```

```python
# config_manager.py — Aynı anahtarlar Python tarafında da düz metin
"auth_key_hex":    "A1B2C3D4E5F60718293A4B5C6D7E8F900112233445566778899AABBCCDDEEFF0",
"auth_password_hex": "DEADBEEFCAFEBABE123456789ABCDEF0",
"hmac_mac_key_hex":  "F1E2D3C4B5A69788796A5B4C3D2E1F00112233445566778899AABBCCDDEEFF01",
```

**Saldırı Senaryosu:**
1. Saldırgan GitHub/kaynak kod deposuna erişir (ya da ikili dosyayı tersine çevirir).
2. `rf_bootloader.h`'deki tüm anahtarları okur.
3. Kendi RF modülüyle geçerli auth paketi oluşturur.
4. Herhangi bir hedef cihaza yetkisiz firmware yükler.
5. `DEFAULT_AES_KEY`'in ASCII `"1234567890..."` olması saldırıyı daha da kolaylaştırır.

**Ek Risk:** Aynı anahtarlar tüm cihaz filosuna gömülüdür. Tek bir cihazdan anahtar sızması tüm filoyu etkiler (sıfır izolasyon).

**Önerilen Çözüm:**
- Cihaz başına benzersiz anahtar: `device_key = KDF(master_secret, device_serial_number)`
- Master secret OTP (One-Time Programmable) alanına veya RDP Level 2 korumalı sayfaya yaz
- Firmware içinde anahtar saklamak yerine **Protected Storage** veya **eFuse** kullan
- Minimum geçici çözüm: AES_KEY'i ASCII'den gerçek rastgele 32 byte'a değiştir

---

### K2 — Kriptografik Olarak Zayıf Nonce Üretimi

**Etkilenen Dosya:** `alici_cihaz/Core/Src/boot_flow.c` — satır 86–91

**Teknik Detay:**

```c
static uint32_t generate_nonce(void) {
    uint32_t t  = HAL_GetTick();    // Sistem çalışma süresi (ms) — tahmin edilebilir
    uint32_t sv = SysTick->VAL;     // SysTick sayacı — deterministik init değeri
    return (t ^ (sv << 13) ^ (t >> 7)) * 2654435761UL;  // Knuth çarpımsal hash
}
```

**Sorunlar:**

1. **Donanım RNG Yok:** STM32F030, True RNG (TRNG) donanımı içermez. Nonce tamamen yazılım tabanlıdır.
2. **Tahmin Edilebilir Başlangıç Değeri:** Cihaz her açılışta sabit SysTick değeriyle başlar. `HAL_GetTick()` reset'ten bu yana geçen milisaniyeleri sayar — bu değer güç açılış zamanlamasıyla ilişkilidir.
3. **Yalnızca 32-bit Uzay:** 4.294.967.296 olası nonce değeri; pasif RF dinleme yeterli veriyle uzayı sınırlandırabilir.
4. **Tekrar Kullanım Riski:** Aynı güç döngüsü zamanlamasıyla aynı nonce üretilir.

**Pratik Saldırı:**
- Saldırgan 10 oturumdan nonce değerlerini kaydeder.
- Boot zamanını (±5 saniye) tahmin ederek nonce uzayını ~10.000 değere indirir.
- Brute-force ile geçerli auth paketi üretir.

**Önerilen Çözüm:**
- **Kısa Vadeli:** Birden fazla entropi kaynağını birleştir:
  ```c
  nonce ^= ADC_Read_Noise_Channel();  // ADC pin gürültüsü
  nonce ^= Flash_ReadUID();           // STM32 benzersiz ID (96 bit)
  nonce ^= RTC_GetSubSeconds();       // RTC alt-saniye (varsa)
  ```
- **Uzun Vadeli:** STM32F0'ın TRNG yokluğunda harici ATECC608 gibi güvenli eleman kullan
- Nonce'u 32-bit'ten 64-bit'e genişlet

---

### K3 — Firmware Boyutu ve Metadata Sınır Kontrolü Yok

**Etkilenen Dosya:** `alici_cihaz/Core/Src/boot_flow.c` — satır 354–376, 490, 586

**Teknik Detay:**

```c
// ADIM 2: Metadata alındı — sınır kontrolü yok!
if (rx_type == RF_CMD_METADATA && rx_pld_len >= 12) {
    memcpy(&metadata, rx_pld, sizeof(Firmware_Metadata_t));
    total_packets = (metadata.firmware_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE;
    // SORUN 1: firmware_size = 0 ise → total_packets = 0, HMAC hiç hesaplanmaz
    // SORUN 2: firmware_size > 222KB ise → bootloader alanına taşar
    // SORUN 3: firmware_size = 0xFFFFFFFF ise → overflow ile total_packets = 0x200000
    got_metadata = 1;
    RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
}
```

```c
// Resume bitmap erişimi — üst sınır kontrolü yok
if (packets_received % PACKETS_PER_PAGE == 0) {
    uint32_t page_done = (packets_received / PACKETS_PER_PAGE) - 1;
    Resume_SavePageDone(page_done);  // page_done >= 111 → bitmap taşması
}
```

**Saldırı Senaryoları:**

| `firmware_size` Değeri | Sonuç |
|------------------------|-------|
| `0x00000000` | `total_packets = 0`, veri döngüsü atlanır, HMAC hesaplanmaz |
| `0xFFFFFFFF` | Integer overflow: `total_packets = 0x200000` (2M paket bekler) |
| `> 0x037800` (222KB) | Flash yazımı bootloader ve boot flag sayfasını siler |
| `= geçerli değer - 1` | Son sayfa yazılmaz, cihaz kısmi firmware ile boot eder |

**Önerilen Çözüm:**

```c
// Eklenmesi gereken doğrulama bloğu
if (metadata.firmware_size == 0 ||
    metadata.firmware_size > APP_AREA_SIZE ||
    metadata.firmware_version == 0) {
    RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                  (uint8_t[]){RF_ERR_INVALID_METADATA}, 1);
    return;
}
// Metadata paketinin kendisi için CRC veya HMAC kontrolü ekle
```

---

### K4 — Donanım Okuma Koruması (RDP) Etkin Değil

**Etkilenen Dosyalar:** `alici_cihaz.ioc`, `uart_rf_gonderici.ioc`

**Teknik Detay:**

STM32F030 üç seviyeli okuma koruması sunar:

| Seviye | Koruma | Durum |
|--------|--------|-------|
| Level 0 (varsayılan) | Yok — JTAG/SWD tam erişim | **MEVCUT DURUM** |
| Level 1 | SWD ile flash okunamaz; silme → Level 0 geçiş mümkün | Etkin değil |
| Level 2 | Kalıcı kilit — geri alınamaz; JTAG tamamen devre dışı | Etkin değil |

**Mevcut Riskler:**
1. **Firmware Okuma:** Herhangi biri ST-Link ile tüm flash içeriğini okuyabilir.
2. **Anahtar Çıkarımı:** `rf_bootloader.h`'deki tüm anahtarlar dump'lanabilir.
3. **Bootloader Değiştirme:** Saldırgan kendi bootloader'ını yazabilir.
4. **Write Protection (WRP) Yok:** Bootloader sayfaları yanlışlıkla veya kasıtlı olarak silinebilir.

**Önerilen Çözüm:**
- Üretim öncesi RDP Level 1'i etkinleştir:
  ```
  STM32CubeProgrammer → OB → Read Out Protection → Level 1 → Apply
  ```
- Bootloader sayfaları (0x08000000–0x08003FFF) için Write Protection etkinleştir:
  ```
  OB → Write Protection → Pages 0-7 → Enable
  ```
- **DİKKAT:** RDP Level 2 kalıcıdır ve test sürecinde kullanılmamalıdır.

---

### K5 — Özel Anahtar Dosyası Proje Dizininde

**Etkilenen Dosya:** `Uploader/private_key.pem`

**Teknik Detay:**

```
Uploader/
├── private_key.pem       ← ELLİPTİK EĞRİ ÖZEL ANAHTARI (şifresiz!)
├── public_key_bytes.txt  ← Public key (bu sorun değil)
└── key_gen.py
```

ECDSA-P256 özel anahtarı proje kök dizininde şifresiz PEM formatında saklanmaktadır.

**Riskler:**
1. Git commit'e dahil edilirse kalıcı olarak geçmişte kalır (`git log`, `git stash`)
2. Proje klasörü paylaşılırsa anahtar da paylaşılır
3. CI/CD pipeline'da açığa çıkabilir
4. Bulut yedeklemesinde (OneDrive, Dropbox) şifresiz yer alır

**Not:** Mevcut sistemde ECDSA kaldırılıp HMAC-SHA256'ya geçilmiş olsa da, `private_key.pem` hâlâ diskte durmaktadır.

**Önerilen Çözüm:**
- `.gitignore`'a ekle: `private_key.pem`, `*.pem`, `*_key.txt`
- Üretim anahtarlarını HSM (Hardware Security Module) veya OS anahtar deposunda sakla (Windows: DPAPI/CNG, Linux: PKCS#11)
- Var olan commit geçmişinden temizle: `git filter-branch` veya BFG Repo-Cleaner

---

## YÜKSEK BULGULAR

---

### Y1 — Firmware Sürüm Geri Alma (Rollback) Koruması Yok

**Etkilenen Dosyalar:** `alici_cihaz/Core/Src/boot_flow.c` — satır 748, `boot_storage.c` — satır 357

**Teknik Detay:**

```c
// boot_flow.c — Sürüm kaydediliyor ama karşılaştırılmıyor
void Flash_Write_Version(uint32_t version) {
    HAL_FLASH_Program(..., VERSION_ADDRESS, version);
}

// Bootloader sürümü kontrol etmiyor:
// if (metadata.firmware_version < current_version) → REDDET
// Bu kontrol YOK.
```

**Saldırı Senaryosu:**
1. v2.0'da düzeltilen bir güvenlik açığı bulunur.
2. Saldırgan v1.0 firmware'ini geçerli HMAC ile gönderir (HMAC anahtarı biliniyorsa).
3. Cihaz v1.0'ı kabul eder, güvenlik açığı geri döner.

**Önerilen Çözüm:**
```c
// Metadata doğrulama bloğuna eklenecek:
uint32_t current_version = Flash_Read_Version();
if (metadata.firmware_version < current_version) {
    RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                  (uint8_t[]){RF_ERR_VERSION_TOO_OLD}, 1);
    return;
}
```

---

### Y2 — Config Şifrelemesinde Zayıf Güvenlik

**Etkilenen Dosya:** `Uploader/config_manager.py` — satır 28–48

**Teknik Detay:**

```python
PBKDF2_ITERATIONS = 100_000     # 2026 için DÜŞÜK — NIST önerisi: 310K+

_DEFAULT_CRED_SALT = bytes.fromhex("a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6")
# TÜM KURULUMLAR AYNI SALT → Rainbow table saldırısı mümkün

_CRED_ENC_KEY = hashlib.sha256(b"SmartHomeFW_CredKey_2026").digest()
# STATİK ŞIFRELEME ANAHTARI — kaynak kodundan türetilebilir
```

**Saldırı Zinciri:**
1. `config.enc` dosyası ele geçirilir (disk erişimi veya yedek).
2. Kaynak kodundan `_CRED_ENC_KEY` türetilir (sabit string).
3. Credentials dosyası doğrudan şifre kırmadan çözülür.
4. İçindeki `auth_key_hex`, `hmac_mac_key_hex` okunur.
5. Tüm güvenlik katmanları atlatılır.

**Önerilen Çözüm:**
- PBKDF2 iterasyonunu 310.000'e yükselt
- Statik şifreleme anahtarı yerine OS anahtar deposunu kullan (Windows DPAPI):
  ```python
  # win32crypt modülü ile
  from win32crypt import CryptProtectData, CryptUnprotectData
  encrypted = CryptProtectData(plaintext, None, None, None, None, 0)
  ```
- Sabit salt'ı kaldır, her kurulum için benzersiz salt üret

---

### Y3 — Kimlik Doğrulama Başarısız Olduğunda Cihaz Bootloader'da Kalıyor (DoS)

**Etkilenen Dosya:** `alici_cihaz/Core/Src/boot_flow.c` — satır 306, `main.c` — satır 123

**Teknik Detay:**

```c
// boot_flow.c — Auth başarısız → bootloader sonsuz döngüde asılı kalır
// (ADIM 1.5 auth loop)
while (!auth_ok && (HAL_GetTick() - auth_start) < 30000) {
    // 30 saniye sonra ne olur? → auth_done = 0 ile devam eder
    // Aşağıdaki kod çalışır:
}
// auth_ok = 0 ise: direkt BOOT_ACK döngüsüne geçer!
// Yani auth başarısız olsa bile bootloader devam ediyor
```

Daha da kötüsü, `main.c`'de:

```c
if (rx_type == RF_CMD_BOOT_REQUEST || rx_type == RF_CMD_AUTH_REQUEST) {
    Bootloader_Main();   // Auth olmadan da bootloader'a girildi
    NVIC_SystemReset();
}
```

**DoS Saldırısı:**
1. Saldırgan sahte AUTH_REQUEST paketleri yayar.
2. Her 3 saniyede bir cihaz bootloader'a girer.
3. Cihaz hiçbir zaman uygulamaya geçemez.
4. Meşru güncelleme de mümkün olmaz.

**Önerilen Çözüm:**
```c
// Auth başarısız sayacı ekle
static uint8_t auth_fail_count = 0;
if (!auth_ok) {
    auth_fail_count++;
    if (auth_fail_count >= 3) {
        // 3 başarısız denemeden sonra bootloader'dan çık
        auth_fail_count = 0;
        jump_to_application();  // veya NVIC_SystemReset()
        return;
    }
}
```

---

### Y4 — PC Yöneticisi Güvenlik Zayıflıkları

**Etkilenen Dosya:** `Uploader/config_manager.py` — satır 32–44

**Teknik Detay:**

```python
_DEFAULT_ADMIN_PASSWORD = "admin"          # Tahmin edilmesi kolay varsayılan
_DEFAULT_ADMIN_HASH = hashlib.pbkdf2_hmac( # Hash kaynakta görünüyor
    'sha256',
    _DEFAULT_ADMIN_PASSWORD.encode(),
    _DEFAULT_CRED_SALT,  # Sabit salt
    PBKDF2_ITERATIONS
).hex()
```

Kaynak koduna erişimi olan herkes:
- Varsayılan şifrenin `"admin"` olduğunu bilir.
- Sistemin kurulu ve hiç şifre değiştirilmemiş olduğu durumda admin erişimi kazanır.
- Şifre değiştirilse bile salt sabitse rainbow table kullanılabilir.

**Önerilen Çözüm:**
- İlk çalıştırmada güçlü şifre oluşturulmasını zorunlu kıl
- Kaynak kodundan `_DEFAULT_ADMIN_PASSWORD` sabitini kaldır
- Şifre karmaşıklık politikası uygula (min 12 karakter, özel karakter vb.)

---

### Y5 — Metadata RF Paketinde Bütünlük Denetimi Yok

**Etkilenen Dosya:** `alici_cihaz/Core/Src/boot_flow.c` — satır 354–362

**Teknik Detay:**

Firmware paketleri (`RF_CMD_DATA_CHUNK`) CRC-32 ile korunurken, kritik metadata paketi herhangi bir bütünlük kontrolüne tabi değildir:

```c
// Metadata paketi — doğrulama YOK
if (rx_type == RF_CMD_METADATA && rx_pld_len >= 12) {
    memcpy(&metadata, rx_pld, sizeof(Firmware_Metadata_t));
    // firmware_size, firmware_version, firmware_crc32 doğrudan kullanılıyor
    // Bit hatası veya RF bozulması tespit edilemiyor
    RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
}
```

**Senaryo:** RF interferans sonucu `firmware_size` yanlış alınırsa:
- Yanlış paket sayısı hesaplanır
- Transfer erken biter veya sonsuz bekler
- CRC-32 final kontrolü başarısız olur (hatayı ancak sonunda anlarız)

**Önerilen Çözüm:**
```c
// Metadata yapısına HMAC ekle (12 byte veri + 8 byte truncated HMAC = 20 byte)
// Ya da basitçe CRC-32 ekle:
uint32_t meta_crc;
memcpy(&meta_crc, &rx_pld[12], 4);
if (crc32(rx_pld, 12) != meta_crc) {
    /* Reddet */
}
```

---

## ORTA BULGULAR

---

### O1 — Auth Girişimlerine Hız Sınırı ve Kilitleme Yok

**Etkilenen Dosya:** `alici_cihaz/Core/Src/boot_flow.c` — satır 253–304

Bootloader, 30 saniyelik auth penceresi boyunca sınırsız denemeyi kabul eder. Her başarısız denemede yeni nonce üretilse de, 32-bit nonce uzayı ile çok sayıda RF modülü paralel saldırı gerçekleştirebilir.

**Önerilen Çözüm:** 5 başarısız denemeden sonra 60 saniye bekleme (exponential backoff), ardından sistem yeniden başlatma.

---

### O2 — Resume Bitmap Bütünlük Kontrolü Yok

**Etkilenen Dosya:** `alici_cihaz/Core/Src/boot_flow.c` — satır 387–391

Transfer devam (resume) durumu bitmap formatında flash'ta saklanmaktadır. Bu bitmap için CRC veya başka bir bütünlük kontrolü yoktur. Güç kesintisi sırasında bitmap bozulursa cihaz yanlış sayfaları yazmamış sayabilir, sonuç olarak kısmi/geçersiz bir firmware boot eder.

**Önerilen Çözüm:** Resume sayfasına 4-byte CRC-32 ekle; yüklemede doğrula.

---

### O3 — 32-bit Nonce Uzayı (Entropi Sınırlı)

**Etkilenen Dosya:** `alici_cihaz/Core/Src/boot_flow.c` — satır 239–248

Nonce değeri 4 byte (32-bit) ile sınırlıdır. Protokol bu nonce'u AES-CBC şifrelemesiyle korusa da, küçük nonce uzayı ilerideki protokol değişikliklerinde zayıflık yaratır.

**Önerilen Çözüm:** Nonce'u 8 byte (64-bit) olarak genişlet.

---

### O4 — Google Drive Servis Hesabı Kimlik Bilgileri

**Etkilenen Dosya:** `Uploader/config_manager.py` — satır 56

```python
"service_account_json": "C:\\Users\\Emir Furkan\\Desktop\\FirmwareUpdate\\eng-name-487012-d5-f4a48c3112a6.json"
```

Google servis hesabı JSON dosyası adı ve mutlak yolu kaynak kodunda görünmektedir. Servis hesabı kimlik bilgileri sızdığında, firma Drive hesabına tam erişim mümkündür.

**Önerilen Çözüm:**
- Yolu ortam değişkenine taşı: `os.environ.get("GOOGLE_SA_JSON_PATH")`
- Servis hesabı yetkilerini minimum düzeye indirge (read-only, belirli klasör)
- Kimlik bilgisi dosyasını `.gitignore`'a ekle

---

### O5 — SWD/JTAG Debug Arayüzü Üretim Cihazlarda Açık

**Etkilenen Dosyalar:** `alici_cihaz.ioc`, `uart_rf_gonderici.ioc`

Debug arayüzü varsayılan olarak etkindir. Fiziksel erişimi olan herkes:
- Tüm flash içeriğini dump edebilir (firmware + anahtarlar)
- RAM'i inceleyebilir (çalışma zamanı verileri)
- Firmware'i değiştirebilir (WRP olmadan)
- Cihazı istediği noktada durdurabilir

**Önerilen Çözüm:** Üretim öncesi RDP Level 1 ve WRP etkinleştir (K4 ile aynı aksiyon).

---

### O6 — Uygulama Başlamadan Önce İmza Denetimi Yok

**Etkilenen Dosya:** `alici_cihaz/Core/Src/main.c` — satır 101–137

```c
uint32_t app_msp = *(volatile uint32_t *)APP_ADDRESS;
if ((app_msp & 0xFFF00000) == 0x20000000) {
    // Sadece MSP kontrolü — imza YOK
    jump_to_application();
}
```

Bootloader, uygulamaya atlamadan önce yalnızca MSP değerinin RAM aralığında olup olmadığını kontrol eder. Kriptografik imza veya HMAC denetimi yapılmaz. Dolayısıyla:
- Eski firmware yüklendikten sonra bootloader yeni doğrulama gereklilikleri olmadan devam eder
- Saldırgan RDP olmadan flash yazabilirse (SWD açık) bootloader bu firmwaredeki değişikliği fark etmez

**Önerilen Çözüm:** Boot öncesi HMAC kontrolü ekle (stored HMAC → flash'taki HMAC değeri ile karşılaştır).

---

### O7 — Hata Mesajları Saldırgana Bilgi Sızdırıyor

**Etkilenen Dosya:** `uart_rf_gonderici/Core/Src/sender_fw_update.c` — çeşitli satırlar

```c
Print("[FW] HATA: Auth CRC gecersiz!\r\n");
Print("[FW] HATA: Auth paketi alinamadi!\r\n");
Print("[FW] HATA: Alici auth reddetti!\r\n");
```

Ayrıntılı hata mesajları UART üzerinden gönderilmektedir. COM porta erişimi olan bir saldırgan:
- Hangi doğrulama adımının başarısız olduğunu öğrenir
- Auth mekanizması hakkında bilgi edinir
- Saldırı stratejisini buna göre ayarlar

**Önerilen Çözüm:** Üretim firmware'inde hata mesajlarını genel ifadelerle değiştir veya tamamen kaldır. Debug modunu derleme zamanı flag ile kontrol et.

---

## DÜŞÜK BULGULAR

---

### D1 — Transfer Günlüğü (Log) Tutulmuyor

Başarılı ve başarısız firmware güncelleme denemeleri hiçbir yerde kalıcı olarak kaydedilmemektedir. Güvenlik olaylarının retrospektif analizi mümkün değildir.

**Önerilen Çözüm:** Flash'ta son 10 transfer denemesinin özet kaydını tut (zaman damgası, sonuç, firmware versiyonu).

---

### D2 — Firmware Transfer Zaman Aşımı Değerleri Çok Yüksek

**Etkilenen Dosya:** `Uploader/uploder.py` — satır 244–250

```python
TIMEOUT_FLASH  = 90  # saniye — çok uzun
TIMEOUT_FINAL  = 90  # saniye — çok uzun
TIMEOUT_PACKET = 30  # saniye — tek paket için çok uzun
```

Yüksek zaman aşımı değerleri:
- Başarısız transfer tespitini geciktirir
- DoS saldırısını kolaylaştırır (kaynak tüketimi)
- Kullanıcı deneyimini olumsuz etkiler

---

### D3 — Sender'da UART Arabelleği Temizlenmiyor

**Etkilenen Dosya:** `uart_rf_gonderici/Core/Src/sender_fw_update.c`

RF işlemleri sırasında UART'a gelen veriler okunmadan birikebilir (overrun). Özellikle auth paketi (52 byte) ve metadata (12 byte) alımlarında race condition riski vardır.

---

## KRİTİK YOL ANALİZİ

Bir saldırganın sistemi tamamen ele geçirmesi için gerekli adımlar:

```
[Aşama 1 — Anahtar Edinimi]
Kaynak kod erişimi VEYA firmware dump (SWD — O5, K4)
    → rf_bootloader.h'den auth/HMAC anahtarları (K1)

[Aşama 2 — Auth Bypass]
Sahte AUTH_CHALLENGE bekle (veya K2 ile nonce tahmin et)
    → Geçerli auth paketi oluştur (K1 anahtarlarıyla)
    → AUTH_ACK al

[Aşama 3 — Firmware Enjeksiyonu]
Kötü amaçlı firmware hazırla (herhangi bir binary)
    → AES-256-CBC ile şifrele (DEFAULT_AES_KEY — K1)
    → HMAC-SHA256 hesapla (HMAC_MAC_KEY — K1)
    → Metadata'da boyut/versiyon kontrolü yok (K3, Y1)
    → Firmware cihaza yüklenir

[Toplam Saldırı Süresi]: ~5 dakika (kaynak koda erişim varsayıldığında)
[Gerekli Donanım]: STM32F030 + Si4432 modülü (~15$)
[Gereken Teknik Bilgi]: Orta (RF protokolü + AES)
```

---

## ÖNCELİKLİ AKSİYON PLANI

### Acil (Üretim Öncesi Zorunlu)

| # | Aksiyon | Süre | Etki |
|---|---------|------|------|
| 1 | `DEFAULT_AES_KEY`'i gerçek rastgele 32 byte ile değiştir | 1 saat | K1 kısmen |
| 2 | Tüm varsayılan anahtarları cihaz SN'ye bağla | 2 gün | K1 tam |
| 3 | RDP Level 1 + WRP bootloader sayfaları | 1 saat | K4, O5 |
| 4 | Metadata boyut doğrulaması ekle | 2 saat | K3 |
| 5 | `private_key.pem` dosyasını git geçmişinden temizle | 30 dak | K5 |
| 6 | Firmware sürüm karşılaştırması (rollback engeli) ekle | 3 saat | Y1 |

### Kısa Vade (1-4 Hafta)

| # | Aksiyon | Süre |
|---|---------|------|
| 7 | Nonce entropisini artır (Flash UID + ADC gürültüsü) | 1 gün |
| 8 | Auth başarısız sayacı ve kilit mekanizması | 4 saat |
| 9 | PBKDF2 iterasyonunu 310.000'e yükselt | 1 saat |
| 10 | Statik `_CRED_ENC_KEY` yerine OS anahtar deposu | 2 gün |
| 11 | Metadata paketi için CRC-32 ekle | 2 saat |
| 12 | Resume bitmap için CRC-32 ekle | 4 saat |

### Uzun Vade (1-3 Ay)

| # | Aksiyon |
|---|---------|
| 13 | Cihaz kimlik sertifikası (manufacturer CA + device cert) |
| 14 | Donanım güvenlik modülü (HSM) ile anahtar yönetimi |
| 15 | Firmware güvenli boot zinciri (bootloader imzalama) |
| 16 | Transfer günlüğü (audit log flash'ta) |
| 17 | Nonce'u 8 byte'a genişlet |
| 18 | ECDSA-P256 imzalamaya geri dön (kaynak kod erişimi kısıtlandıktan sonra) |

---

## MEVCUT GÜVENLİK ARTILARI

Sistemin güçlü yönleri de belgelenmelidir:

| Özellik | Değerlendirme |
|---------|---------------|
| AES-256-CBC firmware şifreleme | İyi — her paket benzersiz IV |
| HMAC-SHA256 bütünlük kontrolü | İyi — sabit zamanlı karşılaştırma |
| Nonce challenge-response auth | Makul — replay saldırısını engeller |
| Stack canary (yazılımsal) | Var — donanım MPU yokluğunu telafi eder |
| Resume desteği | İyi — bant genişliğini verimli kullanır |
| CRC-32 her firmware paketinde | İyi — RF bit hatalarını yakalar |
| Per-packet IV (AES-CBC) | İyi — aynı plaintext farklı ciphertext üretir |

---

## SONUÇ

Bu sistem, temel güvenlik gereksinimlerini karşılayan bir yapıya sahipken, üretim ortamında
konuşlandırılmadan önce özellikle **K1 (gömülü anahtarlar)** ve **K4 (RDP)** bulgularının
giderilmesi zorunludur. Bu iki bulgu giderilmeden sistemin güvenliği teorik olarak sağlam
görünse de pratikte tamamen kırılabilir durumdadır.

Anahtarlar cihaza özgü hale getirilip donanım koruması aktif edildiğinde, sistemin
güvenlik seviyesi endüstriyel RF güncelleme uygulamaları için kabul edilebilir düzeye ulaşacaktır.

---

*Rapor Sonu — FirmwareUpdate_RF Güvenlik Analizi — 2026-03-10*
