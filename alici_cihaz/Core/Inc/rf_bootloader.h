#ifndef RF_BOOTLOADER_H
#define RF_BOOTLOADER_H

#include <stdint.h>

// =========================================================================
// STM32F030CC Flash Memory Layout — Dual-Slot OTA
// =========================================================================
//
//  0x08000000 ┌─────────────────────┐
//             │  BOOTLOADER CODE    │  Sayfa 0–11   (12 × 2KB = 24KB)
//  0x08006000 ├─────────────────────┤
//             │  SHARED RF LIB      │  Sayfa 12–14  ( 3 × 2KB =  6KB)
//             │  0x08006000: ROM API Table (64B, sabit adres)
//             │  0x08006040: si4432 + boot_rf kodu
//  0x08007800 ├─────────────────────┤
//             │  KEY_STORE          │  Sayfa 15     ( 1 × 2KB =  2KB)
//  0x08008000 ├─────────────────────┤
//             │  SLOT A (aktif)     │  Sayfa 16–70  (55 × 2KB = 110KB)
//             │  App her zaman buradan çalışır                         │
//  0x08023800 ├─────────────────────┤
//             │  SLOT B (yedek)     │  Sayfa 71–125 (55 × 2KB = 110KB)
//             │  Güncelleme öncesi Slot A yedeklenir; rollback kaynağı │
//  0x0803F000 ├─────────────────────┤
//             │  SLOT METADATA      │  Sayfa 126    ( 1 × 2KB =  2KB)
//             │  SlotMeta_t struct (52 byte); state machine + CRC bilgisi
//  0x0803F800 ├─────────────────────┤
//             │  BOOT FLAG          │  Sayfa 127    ( 1 × 2KB =  2KB)
//             │  set/clear_boot_flag + resume bitmap                   │
//  0x0803FFFF └─────────────────────┘
//
// =========================================================================

// STM32F030 sayfa boyutu
#define FLASH_PAGE_SIZE  2048U
#define FLASH_PAGE_SHIFT 11U      // 2^11 = 2048

// ── Slot A (aktif uygulama) ───────────────────────────────────────────────
#define SLOT_A_ADDRESS   0x08008000U   // sayfa 16 başı
#define SLOT_A_PAGES     55U           // sayfa 16–70
#define SLOT_A_SIZE      (SLOT_A_PAGES * FLASH_PAGE_SIZE) // 112640 = 110KB

// ── Slot B (yedek / staging) ──────────────────────────────────────────────
#define SLOT_B_ADDRESS   0x08023800U   // sayfa 71 başı
#define SLOT_B_PAGES     55U           // sayfa 71–125
#define SLOT_B_SIZE      (SLOT_B_PAGES * FLASH_PAGE_SIZE) // 112640 = 110KB

// ── Slot Metadata (sayfa 126) ─────────────────────────────────────────────
#define SLOT_META_ADDRESS  0x0803F000U
#define SLOT_META_PAGE     126U
#define SLOT_META_MAGIC    0x5107CAFEU

// ── Geriye dönük uyumluluk takma adları ──────────────────────────────────
// boot_flow.c APP_ADDRESS / APP_AREA_SIZE / APP_PAGES kullanıyor; bunlar
// artık Slot A'yı gösteriyor — her zaman yazma hedefi Slot A'dır.
#define APP_ADDRESS   SLOT_A_ADDRESS
#define APP_AREA_SIZE SLOT_A_SIZE
#define APP_PAGES     SLOT_A_PAGES

// Bootloader bölgesi sayfa sayısı (sayfa 0-15, keystore dahil)
#define BOOTLOADER_PAGES 16U

// =========================================================================
// Slot State Machine
// =========================================================================
//
// EMPTY    → Metadata sayfası hiç yazılmamış (ilk kurulum)
// NORMAL   → Slot A geçerli, normal çalışma
// BACKED_UP→ Slot A, Slot B'ye yedeklendi; güncelleme Slot A'ya yazılıyor
// TRIAL    → Yeni firmware Slot A'da, uygulama henüz onaylamamış
// ROLLBACK → Slot B'den Slot A'ya geri yükleme bekliyor (main.c yapar)
// CONFIRMED→ Uygulama sağlıklı boot onayladı
//
// ─── Geçiş diyagramı ────────────────────────────────────────────────────
//
//  EMPTY/NORMAL ──(güncelleme başlar)──► BACKED_UP
//       ↑                                    │
//       │                                    │ (yedekleme + yazma + CRC OK)
//       │                                    ▼
//   ROLLBACK ◄──(trial > max)──────── TRIAL
//       │                                    │
//       │                                    │ (app confirm_boot çağırır)
//       │                                    ▼
//       └────────────────────────────── CONFIRMED → NORMAL
//
// =========================================================================

#define SLOT_STATE_EMPTY      0xFFU  // sayfa silinmiş — veri yok
#define SLOT_STATE_NORMAL     0x01U  // normal çalışma
#define SLOT_STATE_BACKED_UP  0x02U  // yedekleme tamam, yazma devam ediyor
#define SLOT_STATE_TRIAL      0x03U  // yeni fw deneme aşamasında
#define SLOT_STATE_ROLLBACK   0x04U  // geri yükleme gerekiyor
#define SLOT_STATE_CONFIRMED  0x05U  // app onayladı

// =========================================================================
// SlotMeta_t — Sayfa 126 (0x0803F000) formatı
// =========================================================================
//
// Boyut: 52 byte
// Son alan meta_crc32: önceki tüm alanların CRC-32'si (kendisi hariç)
// confirm_flag: 0xFFFFFFFF = onay yok, 0x00000000 = app onayladı
//   (Uygulama bu alanı silmeden 0x00000000 yazabilir: yalnız 1→0 geçişi)
//
typedef struct {
    uint32_t magic;          // SLOT_META_MAGIC = 0x5107CAFE
    uint8_t  state;          // SLOT_STATE_xxx
    uint8_t  trial_count;    // Onaysız boot sayısı (her reset +1)
    uint8_t  max_trials;     // Rollback eşiği (varsayılan 3)
    uint8_t  reserved;
    uint32_t slot_a_version; // Slot A firmware versiyonu
    uint32_t slot_b_version; // Slot B firmware versiyonu (0 = boş)
    uint32_t slot_a_size;    // Slot A firmware boyutu (byte)
    uint32_t slot_b_size;    // Slot B firmware boyutu (byte) — kopyada her zaman SLOT_A_SIZE
    uint32_t slot_a_crc32;   // Slot A firmware CRC-32
    uint32_t slot_b_crc32;   // Slot B firmware CRC-32 (backup doğrulama için)
    uint32_t confirm_flag;   // 0xFFFFFFFF=onay yok, 0x00000000=app onayladı
    uint32_t meta_crc32;     // Bu struct'ın CRC-32'si (meta_crc32 hariç, 48 byte)
} __attribute__((packed)) SlotMeta_t; // 52 byte

// =========================================================================
// Boot Flag — Sayfa 127 (0x0803F800)
// =========================================================================

#define BOOT_FLAG_ADDRESS  0x0803F800U
#define BOOT_FLAG_PAGE     127U
#define BOOT_FLAG_MAGIC    0xB007B007U  // "BOOTBOOT"
#define BOOT_FLAG_REQUEST  0x00000001U  // Güncelleme talep edildi
#define BOOT_FLAG_NONE     0xFFFFFFFFU  // Normal boot (flash silinmiş)

// =========================================================================
// Resume (Kaldığı Yerden Devam) — Boot Flag Sayfasında
// =========================================================================
//
// Boot flag sayfası (0x0803F800) düzeni:
//   +0  [4 byte] : BOOT_FLAG_MAGIC
//   +4  [4 byte] : BOOT_FLAG_REQUEST
//   +8  [4 byte] : (rezerve — versiyon artık SlotMeta'da)
//   +12 [4 byte] : RESUME_MAGIC
//   +16 [4 byte] : Toplam paket sayısı
//   +20 [110 byte]: Sayfa bitti bitmap (55 halfword × 2 byte)
//
// Her sayfa (16 paket = 2KB) tamamlanınca bitmap girişi 0xFFFF→0x0000 yazılır.
// Silme gerekmez (STM32F030: 1→0 yazımı serbesttir).
//
// Her firmware paketinin boyutu 128 byte; Flash sayfa boyutu 2048 byte
// → her sayfada 16 firmware paketi bulunur
#define PACKETS_PER_PAGE (FLASH_PAGE_SIZE / FW_PACKET_SIZE)  // 16

// Resume kayıt adresleri (boot flag sayfasında)
#define RESUME_MAGIC            0x12345678U
#define RESUME_STATE_ADDRESS    (BOOT_FLAG_ADDRESS + 12U)  // Magic  (4 byte)
#define RESUME_TOTAL_OFFSET     (BOOT_FLAG_ADDRESS + 16U)  // Toplam paket (4 byte)
#define RESUME_PAGE_MAP_ADDRESS (BOOT_FLAG_ADDRESS + 20U)  // Bitmap (55 × 2 byte)

// =========================================================================
// KEY_STORE — Kalıcı AES Master Key (Bootloader page 15)
// =========================================================================
//
//  Adres: 0x08007800 (page 15 — bootloader alanının son sayfası)
//  Uygulama firmware güncellemesi bu sayfaya dokunmaz.
//
//  Sayfa düzeni:
//    +0  [4B]: KEY_STORE_MAGIC = 0xAE5CAFE5
//    +4  [32B]: master_key[32]
//    +36 [1B]: key_crc8 (master_key CRC-8)
//
#define KEY_STORE_ADDRESS 0x08007800U
#define KEY_STORE_MAGIC   0xAE5CAFE5U
#define KEY_STORE_PAGE    15U

// =========================================================================
// ECDH — X25519 Key Exchange
// =========================================================================
//
// BOOT_REQUEST payload:  [pub_sender:32]  = 32 byte
// BOOT_ACK payload:      [resume_start:4][pub_receiver:32] = 36 byte
// KEY_UPDATE payload:    [AES_ECB(session_key, new_master_key):32][crc8:1] = 33 byte
//
#define ECDH_KEY_SIZE          32U  // X25519 key boyutu
#define BOOT_REQUEST_PLD_SIZE  32U  // BOOT_REQUEST payload (pub_sender)
#define BOOT_ACK_PLD_SIZE      36U  // BOOT_ACK payload (resume_start + pub_receiver)
#define KEY_UPDATE_PLD_SIZE    33U  // KEY_UPDATE payload (encrypted_key + crc8)

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
#define RF_CMD_KEY_UPDATE 0x08       // Gönderici → Alıcı: Yeni master key (session key ile şifreli)
#define RF_CMD_KEY_UPDATE_ACK 0x09   // Alıcı → Gönderici: Master key Flash'a yazıldı
#define RF_CMD_ACK 0x10              // Genel onay
#define RF_CMD_NACK 0x11             // Genel ret
#define RF_CMD_PING 0x12             // Bağlantı testi
#define RF_CMD_PONG 0x13             // Bağlantı testi cevabı

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

#define RF_ACK_TIMEOUT_MS 2000         // ACK bekleme süresi
#define RF_BOOT_REQUEST_INTERVAL 500   // Boot request tekrar süresi
#define RF_MAX_RETRIES 5               // Maksimum yeniden deneme
#define RF_FLASH_ERASE_TIMEOUT 30000   // Flash silme timeout
#define RF_UPDATE_TIMEOUT 60000        // Toplam güncelleme timeout (paket başı)
#define BOOTLOADER_IDLE_TIMEOUT_MS 60000U // Boşta bekleme: 60s sonra temiz çıkış

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

#endif // RF_BOOTLOADER_H
