/*
 * boot_storage.h — Flash Bellek ve Boot Flag Yonetimi Arayuzu
 *
 * CRC hesaplama:
 *   Calculate_CRC32       : Bellekteki veri icin CRC-32 (her paket dogrulamasi)
 *   Calculate_Flash_CRC32 : Flash adresinden okuyarak CRC-32 (final dogrulama)
 *
 * Boot flag (BOOT_FLAG_ADDRESS — Flash son sayfasi):
 *   check_boot_flag : MAGIC + REQUEST var mi?
 *   set_boot_flag   : Bootloader istek bayragi yaz
 *   clear_boot_flag : Bayragi sil (guncelleme sonrasi)
 *
 * Flash islemleri:
 *   Flash_Erase_Application : 111 sayfa x 2KB = 222KB uygulama alanini sil
 *   Flash_Write_Data        : Halfword (2 byte) adimlarla yaz
 *   Flash_Verify_Data       : Yazilan veriyi dogrula (byte byte karsilastir)
 *
 * Versiyon:
 *   Flash_Read_Version  : Boot flag sayfasindaki versiyon numarasini oku
 *   Flash_Write_Version : Versiyon numarasini kaydet
 */
#ifndef BOOT_STORAGE_H
#define BOOT_STORAGE_H

#include <stdint.h>

/* CRC-32 (ISO-HDLC, zlib ile uyumlu) */
uint32_t Calculate_CRC32(const uint8_t *data, uint32_t length);
uint32_t Calculate_Flash_CRC32(uint32_t start_addr, uint32_t length);

/* Boot flag yonetimi */
uint8_t check_boot_flag(void); // 1=flag set, 0=yok
void set_boot_flag(void);      // MAGIC + REQUEST yaz
void clear_boot_flag(void);    // Sayfayi sil

/* Flash islemleri */
void Flash_Erase_Application(void);
void Flash_Erase_Page(uint32_t page_addr);  // Tek sayfa sil (paket paket silme icin)
void Flash_Write_Data(uint32_t addr, const uint8_t *data, uint32_t len);
uint8_t Flash_Verify_Data(uint32_t addr, const uint8_t *data, uint32_t len);

/* Resume (kaldigi yerden devam) yonetimi */
void Resume_Init(uint32_t total_packets);    // Resume durumunu bashlat
uint32_t Resume_GetStartPacket(void);        // Kaldigi yer: basta gonderilecek paket numarasi
void Resume_SavePageDone(uint32_t page_idx); // Bir sayfa tamamlandi olarak isaretle

/* Versiyon numarasi */
uint32_t Flash_Read_Version(void);
void Flash_Write_Version(uint32_t version);

/* KEY_STORE — Kalici AES master key (page 15, 0x08007800)
 *   KeyStore_Write : Yeni key'i Flash'a yaz (sayfa sil + magic + key + crc8)
 *   KeyStore_Read  : Mevcut key'i oku (magic + crc8 dogrula)
 *                    Donus: 1 = gecerli key kopyalandi, 0 = kayitli key yok */
void KeyStore_Write(const uint8_t *key);
uint8_t KeyStore_Read(uint8_t *key_out);

#endif /* BOOT_STORAGE_H */
