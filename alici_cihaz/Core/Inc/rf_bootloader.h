#ifndef RF_BOOTLOADER_H
#define RF_BOOTLOADER_H

#include <stdint.h>

// =========================================================================
// STM32F030CC Flash Memory Layout
// =========================================================================
//
//  0x08000000 ┌─────────────────────┐
//             │   BOOTLOADER (48KB) │  Sayfa 0–15 (16 × 2KB)
//  0x0800C000 ├─────────────────────┤
//             │   APPLICATION       │  Sayfa 16–126 (111 × 2KB = 222KB)
//  0x0803F800 ├─────────────────────┤
//             │   BOOT FLAG (2KB)   │  Sayfa 127 (son sayfa)
//  0x0803FFFF └─────────────────────┘
//
// =========================================================================

// Uygulama başlangıç adresi (bootloader'dan sonra)
#define APP_ADDRESS 0x0800C000

// Uygulama alanı boyutu (222KB = 111 sayfa × 2KB)
#define APP_AREA_SIZE (206 * 1024)

// STM32F030 sayfa boyutu (HAL zaten tanımlıyorsa kullan)
#ifndef FLASH_PAGE_SIZE
#define FLASH_PAGE_SIZE 2048
#endif

// Bootloader sayfa sayısı
#define BOOTLOADER_PAGES 16

// Uygulama sayfa sayısı
#define APP_PAGES 111

// Boot flag saklama adresi — Flash'ın son sayfası (sayfa 127)
// Format: [MAGIC:4][FLAG:4] = 8 byte
#define BOOT_FLAG_ADDRESS 0x0803F800
#define BOOT_FLAG_PAGE 127
#define BOOT_FLAG_MAGIC 0xB007B007   // "BOOTBOOT"
#define BOOT_FLAG_REQUEST 0x00000001 // Güncelleme talep edildi
#define BOOT_FLAG_NONE 0xFFFFFFFF    // Normal boot (flash silinmiş)

// Firmware versiyonu saklama adresi (boot flag sayfasında, offset +8)
#define VERSION_ADDRESS (BOOT_FLAG_ADDRESS + 8)

// =========================================================================
// Resume (Kaldığı Yerden Devam) Sabitleri
// =========================================================================
//
// Boot flag sayfasının (0x0803F800) kullanılmayan alanına yazılır:
//   +0  [4 byte] : BOOT_FLAG_MAGIC
//   +4  [4 byte] : BOOT_FLAG_REQUEST
//   +8  [4 byte] : VERSION
//   +12 [4 byte] : RESUME_MAGIC  ← resume durumu geçerli mi?
//   +16 [4 byte] : Toplam paket sayısı
//   +20 [222 byte]: Sayfa bitti bitmap (111 halfword, her biri 0x0000 = tamam)
//
// Nasıl çalışır:
//   - Her sayfa (16 paket = 2KB) tamamlandığında, o sayfanın bitmap girişi
//     0xFFFF → 0x0000 yazılır (Flash 1→0 yazımı, silmeden yapılabilir).
//   - Cihaz resetlenince bootloader resume_start_packet'i okur ve
//     BOOT_ACK payload'ında gönderici'ye bildirir.
//   - Gönderici başa döndüğünde ilk N paketi RF'e iletmeden geçer (UART'tan
//     okur, PC'ye ACK verir, RF'e göndermez).
// =========================================================================

// Her firmware paketinin boyutu 128 byte; Flash sayfa boyutu 2048 byte
// → her sayfada 16 firmware paketi bulunur
#define PACKETS_PER_PAGE (FLASH_PAGE_SIZE / FW_PACKET_SIZE) // 16

// Resume durumu kayıt adresleri (boot flag sayfasında)
#define RESUME_MAGIC            0x12345678        // Resume verisi geçerli
#define RESUME_STATE_ADDRESS    (BOOT_FLAG_ADDRESS + 12) // Magic (4 byte)
#define RESUME_TOTAL_OFFSET     (BOOT_FLAG_ADDRESS + 16) // Toplam paket (4 byte)
#define RESUME_PAGE_MAP_ADDRESS (BOOT_FLAG_ADDRESS + 20) // Bitmap (111 x 2 byte)

// =========================================================================
// AES-256 Ayarları
// =========================================================================

// Varsayılan AES key (Custom bootloader ile aynı)
#define AES_KEY_SIZE 32
#define AES_IV_SIZE 16
#define AES_BLOCK_SIZE 16

// Firmware paket boyutu (şifreli)
#define FW_PACKET_SIZE 128

// Python'dan gelen tam paket: IV(16) + Encrypted(128) + CRC32(4) = 148 byte
#define FW_FULL_PACKET_SIZE 148

// =========================================================================
// RF Firmware Update Paket Tipleri
// =========================================================================

// Komut paketleri
#define RF_CMD_BOOT_REQUEST 0x01     // Gönderici → Alıcı: Bootloader'a geç
#define RF_CMD_BOOT_ACK 0x02         // Alıcı → Gönderici: Bootloader hazır
#define RF_CMD_METADATA 0x03         // Gönderici → Alıcı: Firmware bilgileri
#define RF_CMD_DATA_CHUNK 0x04       // Gönderici → Alıcı: Firmware veri parçası
#define RF_CMD_FLASH_ERASE_DONE 0x05 // Alıcı → Gönderici: Flash silindi
#define RF_CMD_UPDATE_COMPLETE 0x06  // Alıcı → Gönderici: Güncelleme başarılı
#define RF_CMD_UPDATE_FAILED 0x07    // Alıcı → Gönderici: Güncelleme başarısız
#define RF_CMD_ACK 0x10              // Genel onay
#define RF_CMD_NACK 0x11             // Genel ret
#define RF_CMD_PING 0x12             // Bağlantı testi
#define RF_CMD_PONG 0x13             // Bağlantı testi cevabı

// ─── Kimlik Doğrulama (Nonce Challenge-Response) ─────────────────────────────
#define RF_CMD_AUTH_REQUEST   0x14   // Gönderici → Alıcı: Auth başlat
#define RF_CMD_AUTH_CHALLENGE 0x15   // Alıcı → Gönderici: 4-byte nonce
#define RF_CMD_AUTH           0x16   // Gönderici → Alıcı: Şifreli auth paketi
#define RF_CMD_AUTH_ACK       0x17   // Alıcı → Gönderici: Auth başarılı
#define RF_CMD_AUTH_NACK      0x18   // Alıcı → Gönderici: Auth başarısız

// ─── Dijital İmza (ECDSA-P256) ───────────────────────────────────────────────
// 64-byte imza 2 RF paketiyle gönderilir (her biri 32 byte payload)
#define RF_CMD_SIG_CHUNK      0x19   // Gönderici → Alıcı: İmza parçası [idx:1][data:32]

// =========================================================================
// RF Paket Yapısı
// =========================================================================
//
// Her RF paketi: [TYPE:1][SEQ_H:1][SEQ_L:1][PAYLOAD:0-50] + Si4432 HW CRC
//
// Başlık her zaman 3 byte:
//   - TYPE   : Paket tipi (RF_CMD_xxx)
//   - SEQ_H  : Sequence number üst byte
//   - SEQ_L  : Sequence number alt byte
//
// Si4432 donanım CRC otomatik eklenir (CRC-IBM)
// =========================================================================

#define RF_HEADER_SIZE 3  // TYPE(1) + SEQ(2)
#define RF_MAX_PAYLOAD 50 // Si4432 FIFO 64 byte, başlık dahil max 53
#define RF_MAX_PACKET_SIZE (RF_HEADER_SIZE + RF_MAX_PAYLOAD) // 53 byte

// =========================================================================
// Firmware Paketi RF Parçalama
// =========================================================================
//
// Python'dan gelen 148 byte (IV+Encrypted+CRC) → 4 RF DATA_CHUNK paketi:
//   Parça 0: data[0..47]   = 48 byte
//   Parça 1: data[48..95]  = 48 byte
//   Parça 2: data[96..143] = 48 byte
//   Parça 3: data[144..147]=  4 byte
//
// Her DATA_CHUNK payload:
//   [CHUNK_INDEX:1][CHUNK_TOTAL:1][DATA:max 48]
//
// Yani RF paketi:
//   [TYPE:1][SEQ:2][CHUNK_IDX:1][CHUNK_CNT:1][DATA:max 48] = max 53 byte
// =========================================================================

#define RF_CHUNKS_PER_PACKET 4 // 148 byte → 4 RF parça (48+48+48+4)
#define RF_CHUNK_DATA_SIZE 48  // Her parçadaki max veri

// =========================================================================
// Protokol Zamanlayıcıları
// =========================================================================

#define RF_ACK_TIMEOUT_MS 2000       // ACK bekleme süresi
#define RF_BOOT_REQUEST_INTERVAL 500 // Boot request tekrar süresi
#define RF_MAX_RETRIES 5             // Maksimum yeniden deneme
#define RF_FLASH_ERASE_TIMEOUT 30000 // Flash silme timeout
#define RF_UPDATE_TIMEOUT 60000      // Toplam güncelleme timeout (paket başı)

// =========================================================================
// Hata Kodları
// =========================================================================

#define RF_ERR_NONE 0x00
#define RF_ERR_CRC_FAIL 0x01
#define RF_ERR_AES_FAIL 0x02
#define RF_ERR_FLASH_WRITE 0x03
#define RF_ERR_FLASH_VERIFY 0x04
#define RF_ERR_FLASH_ERASE 0x05
#define RF_ERR_INVALID_MSP 0x06
#define RF_ERR_FW_CRC_MISMATCH 0x07
#define RF_ERR_TIMEOUT 0x08
#define RF_ERR_SEQ_MISMATCH 0x09
#define RF_ERR_AUTH_FAIL 0x10        // Kimlik doğrulama hatası (yanlış şifre/nonce)
#define RF_ERR_SIG_FAIL  0x11        // ECDSA imza doğrulama hatası

// =========================================================================
// Firmware Metadata Yapısı (12 byte)
// =========================================================================

typedef struct {
  uint32_t firmware_size;    // Firmware boyutu (byte)
  uint32_t firmware_version; // Firmware versiyonu
  uint32_t firmware_crc32;   // Tüm firmware'in CRC-32'si
} __attribute__((packed)) Firmware_Metadata_t;

// =========================================================================
// RF Paket Yapısı
// =========================================================================

typedef struct {
  uint8_t type; // RF_CMD_xxx
  uint16_t seq; // Sequence number
  uint8_t payload[RF_MAX_PAYLOAD];
  uint8_t payload_len; // Gerçek payload uzunluğu
} RF_Packet_t;

// =========================================================================
// Kimlik Doğrulama Sabitleri
// =========================================================================
//
// Auth paketi yapısı (UART: 52 byte, RF: 48 byte):
//   [IV:16][AES256_CBC(AUTH_KEY, IV, nonce[4]+AUTH_PASSWORD[16]+pad[12]):32][CRC32:4]
//
// ÖNEMLİ: Bu değerleri değiştirin! Varsayılan değerler GÜVENLİ DEĞİLDİR.
// uploder.py içindeki auth_key_hex ve auth_password_hex ile EŞLEŞMELİDİR.
// =========================================================================

// Auth şifrelemesi için AES-256 anahtarı (firmware AES key'inden FARKLI olmalı)
// DEĞİŞTİRİN: python key_gen.py ile veya rastgele 32 byte üretin
static const uint8_t DEFAULT_AUTH_KEY[32] = {
    0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
    0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90,
    0x01, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78,
    0x89, 0x9A, 0xAB, 0xBC, 0xCD, 0xDE, 0xEF, 0xF0
};

// Şifre çözüldükten sonra beklenen 16-byte auth şifresi
// DEĞİŞTİRİN: uploder.py auth_password_hex ile AYNI olmalı
static const uint8_t DEFAULT_AUTH_PASSWORD[16] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
    0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0
};

// =========================================================================
// Ed25519 Açık Anahtar (32 byte)
// =========================================================================
//
// Firmware dijital imza doğrulaması için.
// PC özel anahtarıyla (private_key.pem) firmware SHA-256 hash'ini imzalar.
// Bootloader bu açık anahtarla imzayı doğrular (RFC 8032 Ed25519).
//
// Asimetrik: Özel anahtar YALNIZCA PC'de → cihazdan anahtar çıkartılsa bile
// geçerli imza üretilemez.
//
// NASIL ÜRETİLİR:
//   cd Uploader && python key_gen.py
//   Çıktıdaki diziyi buraya yapıştırın.
//
// ÖNEMLİ: Varsayılan (sıfır) değerle tüm imzalar reddedilir!
// =========================================================================

// DEĞİŞTİRİN: python key_gen.py çalıştırıp public_key_bytes.txt içeriğini yapıştırın
static const uint8_t ED25519_PUBLIC_KEY[32] = {
    0xA8, 0x41, 0x24, 0x70, 0x7F, 0x26, 0x81, 0x6B,
    0xF9, 0x0D, 0x97, 0x7E, 0xAE, 0x11, 0x9F, 0x96,
    0xEF, 0xA8, 0xA8, 0x74, 0xC7, 0x50, 0xD4, 0x2C,
    0x68, 0x3A, 0x73, 0x3D, 0x47, 0x1F, 0xE6, 0x17
    /* ↑ PLACEHOLDER — key_gen.py çıktısıyla değiştirin! */
};

#endif // RF_BOOTLOADER_H
