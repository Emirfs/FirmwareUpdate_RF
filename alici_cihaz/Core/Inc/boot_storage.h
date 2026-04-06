/*
 * boot_storage.h — Flash Bellek, Slot Yönetimi ve Boot Flag Arayüzü
 *
 * CRC hesaplama:
 *   Calculate_CRC32       : RAM/dizi verisi için CRC-32
 *   Calculate_Flash_CRC32 : Flash adresinden okuyarak CRC-32
 *
 * Slot Metadata (sayfa 126, 0x0803F000):
 *   SlotMeta_Read         : SlotMeta_t oku + doğrula (CRC32 kontrolü)
 *   SlotMeta_Write        : SlotMeta_t sil + yaz (meta_crc32 otomatik hesaplanır)
 *   SlotMeta_ConfirmBoot  : App onay bayrağını sil olmadan yaz (0→confirm)
 *
 * Slot kopyalama (110KB, ~7-8 sn, IWDG beslenir):
 *   SlotA_BackupToB       : Slot A → Slot B (güncelleme öncesi yedek)
 *   SlotA_RestoreFromB    : Slot B → Slot A (rollback)
 *
 * Boot flag (sayfa 127, 0x0803F800):
 *   check_boot_flag : MAGIC + REQUEST var mı?
 *   set_boot_flag   : Bootloader istek bayrağı yaz
 *   clear_boot_flag : Sayfayı sil (resume bitmap de temizlenir)
 *
 * Flash temel işlemleri:
 *   Flash_Erase_Page  : Tek sayfa sil (Slot A veya B aralığı)
 *   Flash_Write_Data  : Halfword (2 byte) adımlarla yaz
 *   Flash_Verify_Data : Yazılan veriyi doğrula
 *
 * Resume (kaldığı yerden devam — boot flag sayfasında):
 *   Resume_Init          : Resume durumunu başlat
 *   Resume_GetStartPacket: Kaldığı paket numarasını döndür
 *   Resume_SavePageDone  : Sayfa tamamlandı olarak işaretle
 *
 * KEY_STORE (sayfa 15, 0x08007800):
 *   KeyStore_Write : Yeni AES master key yaz
 *   KeyStore_Read  : Mevcut key oku ve doğrula
 */
#ifndef BOOT_STORAGE_H
#define BOOT_STORAGE_H

#include "rf_bootloader.h"
#include <stdint.h>

/* CRC-32 (ISO-HDLC, zlib ile uyumlu) */
uint32_t Calculate_CRC32(const uint8_t *data, uint32_t length);
uint32_t Calculate_Flash_CRC32(uint32_t start_addr, uint32_t length);

/* ── Slot Metadata ────────────────────────────────────────────────────────── */
uint8_t SlotMeta_Read(SlotMeta_t *out);     /* 1=geçerli, 0=geçersiz/boş  */
void    SlotMeta_Write(const SlotMeta_t *meta); /* erase sayfa 126 + yaz  */
void    SlotMeta_ConfirmBoot(void);         /* confirm_flag=0 (silmesiz)  */

/* ── Slot kopyalama ───────────────────────────────────────────────────────── */
uint8_t SlotA_BackupToB(void);    /* Slot A → Slot B (tüm 55 sayfa) */
uint8_t SlotA_RestoreFromB(void); /* Slot B → Slot A (rollback)      */

/* ── Boot flag ────────────────────────────────────────────────────────────── */
uint8_t check_boot_flag(void);
void    set_boot_flag(void);
void    clear_boot_flag(void);

/* ── Flash temel işlemleri ───────────────────────────────────────────────── */
void    Flash_Erase_Page(uint32_t page_addr);
void    Flash_Write_Data(uint32_t addr, const uint8_t *data, uint32_t len);
uint8_t Flash_Verify_Data(uint32_t addr, const uint8_t *data, uint32_t len);

/* ── Resume ───────────────────────────────────────────────────────────────── */
void     Resume_Init(uint32_t total_packets);
uint32_t Resume_GetStartPacket(void);
void     Resume_SavePageDone(uint32_t page_idx);

/* ── KEY_STORE ────────────────────────────────────────────────────────────── */
void    KeyStore_Write(const uint8_t *key);
uint8_t KeyStore_Read(uint8_t *key_out);

#endif /* BOOT_STORAGE_H */
