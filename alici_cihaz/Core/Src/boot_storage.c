/*
 * boot_storage.c — Flash Bellek ve Boot Flag Yonetimi
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * STM32F030CC Flash bellegini yoneten tum dusuk seviye fonksiyonlari icerir.
 *
 * Flash Duzen (STM32F030CC, 256KB):
 *   0x08000000 – 0x08007FFF : Bootloader    (32KB, sayfa 0-15)
 *   0x08008000 – 0x0803F7FF : Uygulama      (222KB, sayfa 16-126)
 *   0x0803F800 – 0x0803FFFF : Boot Flag     (2KB, sayfa 127 = son sayfa)
 *     [MAGIC:4][FLAG:4][VERSION:4][...]
 *
 * Fonksiyonlar:
 *   CRC hesaplama  : Calculate_CRC32, Calculate_Flash_CRC32
 *   Boot flag      : check_boot_flag, set_boot_flag, clear_boot_flag
 *   Flash silme    : Flash_Erase_Application (119 sayfa)
 *   Flash yazma    : Flash_Write_Data (halfword = 2 byte adimlarla)
 *   Flash dogrulama: Flash_Verify_Data (byte byte karsilastirma)
 *   Versiyon       : Flash_Read_Version, Flash_Write_Version
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - Boot flag adresi: BOOT_FLAG_ADDRESS (rf_bootloader.h, 0x0803F800)
 * - Boot flag magic: BOOT_FLAG_MAGIC (rf_bootloader.h, 0xB007B007)
 * - Uygulama sayfa sayisi: APP_PAGES (rf_bootloader.h, 111 sayfa)
 *
 * ─── ONEMLI NOT ───────────────────────────────────────────────────────────
 * STM32F030'da Flash sadece 16-bit (halfword) olarak programlanabilir.
 * Bu yuzden Flash_Write_Data 2 byte adimlarla yazar.
 * Flash_Write_Version da ayni nedenle iki halfword yazma yapar.
 */

#include "boot_storage.h"

#include "iwdg.h"
#include "main.h"
#include "rf_bootloader.h"
#include <stddef.h>
#include <string.h>

/*
 * Calculate_CRC32 — Yazilim CRC-32 hesaplama (RAM veya dizi icin)
 *
 * CRC-32/ISO-HDLC algoritması (polinom 0xEDB88320, reversed).
 * zlib ve Python'un zlib.crc32() ile ayni sonucu verir.
 * Bu fonksiyon firmware paketinin sifrelenmis bolumunu dogrulamak icin
 * kullanilir (her 128 byte sifrelenmis veri geldiginde).
 */
uint32_t Calculate_CRC32(const uint8_t *data, uint32_t length) {
  uint32_t crc = 0xFFFFFFFF; // Baslangiç degeri

  for (uint32_t i = 0; i < length; i++) {
    crc ^= data[i]; // Byte XOR

    /* 8 bit isleme (bit-by-bit) */
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320; // CRC-32 polinom (reversed)
      } else {
        crc >>= 1;
      }
    }
  }

  return crc ^ 0xFFFFFFFF; // Final XOR
}

/*
 * Calculate_Flash_CRC32 — Flash bellek uzerinde CRC-32 hesapla
 *
 * Flash adresi dogrudan pointer ile okunur (memory-mapped).
 * Uzun Flash okumalarinda (238KB) watchdog sifirlamak icin
 * her 4096 byte'ta bir HAL_IWDG_Refresh cagirilir.
 *
 * Kullanim: Tum transfer bittikten sonra Flash CRC'si metadata
 *           firmware_crc32 ile karsilastirilir.
 */
uint32_t Calculate_Flash_CRC32(uint32_t start_addr, uint32_t length) {
  uint32_t crc = 0xFFFFFFFF;
  uint8_t *ptr = (uint8_t *)start_addr; // Flash dogrudan pointer ile okunabilir

  for (uint32_t i = 0; i < length; i++) {
    crc ^= ptr[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320;
      } else {
        crc >>= 1;
      }
    }

    /* Her 4096 byte'ta watchdog sifirla — CRC uzun sure alabilir */
    if (i % 4096 == 0) {
      HAL_IWDG_Refresh(&hiwdg);
    }
  }

  return crc ^ 0xFFFFFFFF;
}

/*
 * check_boot_flag — Boot flag sayfasini kontrol et
 *
 * BOOT_FLAG_ADDRESS'de MAGIC + REQUEST varsa 1 don.
 * main.c bu fonksiyon ile uygulama mı, bootloader mı calısacak karar verir.
 */
uint8_t check_boot_flag(void) {
  volatile uint32_t *ptr = (volatile uint32_t *)BOOT_FLAG_ADDRESS;
  if (ptr[0] == BOOT_FLAG_MAGIC && ptr[1] == BOOT_FLAG_REQUEST) {
    return 1; // Boot flag set — bootloader moduna gec
  }
  return 0; // Flag yok — normal boot
}

/*
 * set_boot_flag — Boot flag sayfasina MAGIC + REQUEST yaz
 *
 * Uygulama kodu bu fonksiyon ile bir sonraki reset'te bootloader'a
 * gecmesini isteyebilir. Şu an kullanılmıyor ama ileride remote trigger icin.
 */
void set_boot_flag(void) {
  HAL_FLASH_Unlock();

  /* Once sayfayi sil (yazabilmek icin silinmis olmali) */
  FLASH_EraseInitTypeDef erase;
  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = BOOT_FLAG_ADDRESS;
  erase.NbPages = 1;
  uint32_t error;
  HAL_FLASHEx_Erase(&erase, &error);

  /* MAGIC yaz (32-bit degeri iki 16-bit yazimiyla) */
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_FLAG_ADDRESS,
                    (uint16_t)(BOOT_FLAG_MAGIC & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_FLAG_ADDRESS + 2,
                    (uint16_t)((BOOT_FLAG_MAGIC >> 16) & 0xFFFF));

  /* REQUEST flag yaz */
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_FLAG_ADDRESS + 4,
                    (uint16_t)(BOOT_FLAG_REQUEST & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_FLAG_ADDRESS + 6,
                    (uint16_t)((BOOT_FLAG_REQUEST >> 16) & 0xFFFF));

  HAL_FLASH_Lock();
}

/*
 * clear_boot_flag — Boot flag sayfasini sil (0xFF ile doldur)
 *
 * Flash silme sonrası sayfa 0xFF ile dolu olur.
 * check_boot_flag 0xFF != MAGIC sartini saglamayacagindan flag temizlenmis sayilir.
 * Basarili guncellemenin sonunda cagrilir.
 */
void clear_boot_flag(void) {
  HAL_FLASH_Unlock();

  FLASH_EraseInitTypeDef erase;
  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = BOOT_FLAG_ADDRESS;
  erase.NbPages = 1;
  uint32_t error;
  HAL_FLASHEx_Erase(&erase, &error); // Sayfayi sil — 0xFF ile dolar

  HAL_FLASH_Lock();
}

/* ── İç yardımcı: sayfa silme (sınır kontrolü yok, yalnız iç kullanım) ─── */
static void flash_erase_page_raw(uint32_t page_addr)
{
  HAL_FLASH_Unlock();
  FLASH_EraseInitTypeDef erase;
  erase.TypeErase   = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = page_addr;
  erase.NbPages     = 1U;
  uint32_t error;
  HAL_FLASHEx_Erase(&erase, &error);
  HAL_IWDG_Refresh(&hiwdg);
  HAL_FLASH_Lock();
}

/*
 * Flash_Erase_Page — Slot A veya Slot B içindeki tek sayfayı sil
 *
 * Geçerli aralık: sayfa 16–125 (Slot A: 16-70, Slot B: 71-125).
 * Sayfa 126 (SlotMeta) ve 127 (Boot Flag) bu fonksiyonla silinemez;
 * bunlar kendi yönetim fonksiyonları tarafından korunur.
 */
void Flash_Erase_Page(uint32_t page_addr)
{
  /* Hizalama kontrolü */
  if ((page_addr % FLASH_PAGE_SIZE) != 0U) { return; }

  /* Geçerli slot aralığı: Slot A (0x08008000) veya Slot B (0x08023800),
   * toplam: 0x08008000 – 0x0803EFFF (sayfa 16–125) */
  if (page_addr < SLOT_A_ADDRESS
      || page_addr >= (SLOT_B_ADDRESS + SLOT_B_SIZE)) {
    return; /* Bootloader, SlotMeta veya BootFlag alanı — reddet */
  }

  flash_erase_page_raw(page_addr);
}

/* =========================================================================
 * SlotMeta Fonksiyonları — Sayfa 126 (0x0803F000)
 * =========================================================================
 *
 * SlotMeta_Read  : SlotMeta_t oku, magic + meta_crc32 doğrula.
 *                  Dönüş: 1=geçerli, 0=geçersiz/boş
 *
 * SlotMeta_Write : meta_crc32 hesapla, sayfayı sil, struct yaz.
 *                  Yalnız bootloader tarafından çağrılır.
 *
 * SlotMeta_ConfirmBoot : confirm_flag alanını 0x00000000 yap.
 *                        Sayfa silinmez — uygulama güvenle çağırabilir.
 * ========================================================================= */

uint8_t SlotMeta_Read(SlotMeta_t *out)
{
  const SlotMeta_t *flash = (const SlotMeta_t *)SLOT_META_ADDRESS;

  /* Magic kontrolü */
  if (flash->magic != SLOT_META_MAGIC) { return 0U; }

  /* Struct kopyala */
  *out = *flash;

  /* meta_crc32: struct'ın ilk (sizeof - 4) byte'ının CRC'si */
  uint32_t expected = Calculate_CRC32((const uint8_t *)flash,
                                      sizeof(SlotMeta_t) - sizeof(uint32_t));
  if (out->meta_crc32 != expected) { return 0U; }

  return 1U;
}

void SlotMeta_Write(const SlotMeta_t *meta)
{
  SlotMeta_t copy = *meta;
  copy.magic = SLOT_META_MAGIC;

  /* meta_crc32 hesapla (son alan hariç tüm struct) */
  copy.meta_crc32 = Calculate_CRC32((const uint8_t *)&copy,
                                    sizeof(SlotMeta_t) - sizeof(uint32_t));

  /* Sayfa 126'yı sil */
  flash_erase_page_raw(SLOT_META_ADDRESS);

  /* Struct'ı halfword adımlarla yaz */
  Flash_Write_Data(SLOT_META_ADDRESS, (const uint8_t *)&copy, sizeof(SlotMeta_t));
}

void SlotMeta_ConfirmBoot(void)
{
  /* confirm_flag alanının Flash adresi */
  uint32_t addr = SLOT_META_ADDRESS
                  + (uint32_t)offsetof(SlotMeta_t, confirm_flag);

  /* 0xFFFFFFFF → 0x00000000 (yalnız 1→0 bit geçişi — silme gerekmez) */
  HAL_FLASH_Unlock();
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr,      0x0000U);
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + 2U, 0x0000U);
  HAL_FLASH_Lock();
}

/* =========================================================================
 * Slot Kopyalama — 55 sayfa × 2KB = 110KB
 * =========================================================================
 *
 * Slot_Copy(src_base, dst_base):
 *   Her sayfada: dst sil → src'den 1024 halfword oku ve yaz → IWDG besle.
 *   Toplam süre: ~7-8 saniye (55 × (5ms erase + ~128ms write)).
 *   IWDG her sayfada beslenir.
 *
 * Dönüş: 1=başarılı, 0=Flash hatası
 * ========================================================================= */

static uint8_t Slot_Copy(uint32_t src_base, uint32_t dst_base)
{
  HAL_FLASH_Unlock();

  for (uint32_t p = 0U; p < SLOT_A_PAGES; p++) {
    uint32_t src = src_base + p * FLASH_PAGE_SIZE;
    uint32_t dst = dst_base + p * FLASH_PAGE_SIZE;

    /* Hedef sayfayı sil */
    FLASH_EraseInitTypeDef erase;
    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = dst;
    erase.NbPages     = 1U;
    uint32_t err;
    if (HAL_FLASHEx_Erase(&erase, &err) != HAL_OK) {
      HAL_FLASH_Lock();
      return 0U;
    }

    /* 2048 byte → 1024 halfword kopyala */
    for (uint32_t i = 0U; i < FLASH_PAGE_SIZE; i += 2U) {
      uint16_t hw = *(volatile const uint16_t *)(src + i);
      if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, dst + i, hw) != HAL_OK) {
        HAL_FLASH_Lock();
        return 0U;
      }
    }

    HAL_IWDG_Refresh(&hiwdg); /* Her sayfa sonrası IWDG besle */
  }

  HAL_FLASH_Lock();
  return 1U;
}

/*
 * SlotA_BackupToB — Slot A içeriğini Slot B'ye kopyala
 * Güncelleme başlamadan çağrılır; rollback için yedek oluşturur.
 * Dönüş: 1=başarılı, 0=hata
 */
uint8_t SlotA_BackupToB(void)
{
  return Slot_Copy(SLOT_A_ADDRESS, SLOT_B_ADDRESS);
}

/*
 * SlotA_RestoreFromB — Slot B içeriğini Slot A'ya kopyala (rollback)
 * main.c state==ROLLBACK tespit edince çağırır.
 * Dönüş: 1=başarılı, 0=hata
 */
uint8_t SlotA_RestoreFromB(void)
{
  return Slot_Copy(SLOT_B_ADDRESS, SLOT_A_ADDRESS);
}

/*
 * Resume_Init — Resume durumunu bashlat
 *
 * Boot flag sayfasinin kullanilmayan alanina (offset +12'den itibaren)
 * RESUME_MAGIC ve toplam paket sayisini yazar. Sayfa bitmap alani
 * baslangicta 0xFF (silinmis) oldugundan, yazma icin silmeye gerek yok.
 *
 * DIKKAT: Bu fonksiyon yalnizca ONCE cagrilmali (resume state yokken).
 *         Resume state zaten varsa (RESUME_MAGIC yaziliysa) cagirma.
 *
 * total_packets: metadata'dan gelen toplam 128-byte paket sayisi
 */
void Resume_Init(uint32_t total_packets) {
  HAL_FLASH_Unlock();

  /* RESUME_MAGIC yaz (offset +12): resume durumu artik gecerli */
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, RESUME_STATE_ADDRESS,
                    (uint16_t)(RESUME_MAGIC & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, RESUME_STATE_ADDRESS + 2,
                    (uint16_t)((RESUME_MAGIC >> 16) & 0xFFFF));

  /* Toplam paket sayisini yaz (offset +16) */
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, RESUME_TOTAL_OFFSET,
                    (uint16_t)(total_packets & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, RESUME_TOTAL_OFFSET + 2,
                    (uint16_t)((total_packets >> 16) & 0xFFFF));

  HAL_FLASH_Lock();
}

/*
 * Resume_SavePageDone — Bir Flash sayfasinin tamamen yazildigini isaretle
 *
 * Resume bitmapinde, page_idx numarali sayfanin halfword girisi 0xFFFF'ten
 * 0x0000'a degistirilir. Bu degisim, Flash'a silmeden yapilabilir
 * (STM32F030'da 1→0 yazimi serbest; 0→1 icin silme gerekir).
 *
 * Cagri zamani: Her 16 paket (1 Flash sayfasi = 2KB) tamamlaninca.
 *
 * page_idx: 0..110 (APP_PAGES - 1)
 */
void Resume_SavePageDone(uint32_t page_idx) {
  uint32_t addr = RESUME_PAGE_MAP_ADDRESS + (page_idx * 2); // Her giris 2 byte (halfword)
  HAL_FLASH_Unlock();
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, 0x0000); // 0xFFFF → 0x0000
  HAL_FLASH_Lock();
}

/*
 * Resume_GetStartPacket — Kaldigi yer: kacinci paketten devam edilecek?
 *
 * Resume bitmapini okur. Bastan itibaren "done" (0x0000) olan sayfa sayisini
 * sayar. Ilk "done olmayan" sayfaya ulasinca durur — bu nokta resume baslangici.
 *
 * Donus: resume_start_packet = pages_done * PACKETS_PER_PAGE
 *        Resume state yoksa (RESUME_MAGIC eslesmiyor) → 0 doner
 *
 * Ornek: sayfa 0, 1, 2 bitti (0x0000) → 3. sayfa bitmemis → 3*16=48 doner
 */
uint32_t Resume_GetStartPacket(void) {
  volatile uint32_t *magic_ptr = (volatile uint32_t *)RESUME_STATE_ADDRESS;

  /* Resume state gecerli mi? */
  if (magic_ptr[0] != RESUME_MAGIC) {
    return 0; // Ilk kez baslatiliyor — bastan basla
  }

  /* Bitmap'te basa gore kac sayfa tamamlandi? */
  uint32_t pages_done = 0;
  volatile uint16_t *bitmap = (volatile uint16_t *)RESUME_PAGE_MAP_ADDRESS;

  for (uint32_t i = 0; i < APP_PAGES; i++) {
    if (bitmap[i] == 0x0000) {
      pages_done++; // Bu sayfa tamam
    } else {
      break; // Ilk eksik sayfa = resume noktasi
    }
  }

  return pages_done * PACKETS_PER_PAGE; // Paket numarasina cevir
}

/*
 * Flash_Write_Data — Flash'a veri yaz (halfword = 2 byte adimlarla)
 *
 * STM32F030 yalnizca 16-bit (halfword) yazma destekler.
 * Tek sayida byte varsa son byte 0xFF ile tamamlanir (pad).
 *
 * addr: yazma baslangic adresi (cift olmali)
 * data: yazilacak veri
 * len : byte sayisi
 */
void Flash_Write_Data(uint32_t addr, const uint8_t *data, uint32_t len) {
  HAL_FLASH_Unlock();

  for (uint32_t i = 0; i < len; i += 2) {
    uint16_t half_word;

    if (i + 1 < len) {
      /* Normal: iki byte birlesik (little-endian) */
      half_word = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
    } else {
      /* Son tek byte: yuksek byte 0xFF ile doldur */
      half_word = (uint16_t)data[i] | 0xFF00;
    }

    HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, half_word);
  }

  HAL_FLASH_Lock();
}

/*
 * Flash_Verify_Data — Flash'a yazilan veriyi dogrula
 *
 * Flash adresi dogrudan okunabilir (memory-mapped).
 * Yazma sonrasi her byte karsilastirilir.
 *
 * Donus: 1 = uyusuyor (basarili), 0 = fark var (hata)
 */
uint8_t Flash_Verify_Data(uint32_t addr, const uint8_t *data, uint32_t len) {
  uint8_t *flash_ptr = (uint8_t *)addr; // Flash dogrudan pointer ile okunur

  for (uint32_t i = 0; i < len; i++) {
    if (flash_ptr[i] != data[i]) {
      return 0; // Fark bulundu — yazma basarisiz
    }
  }

  return 1; // Tum byte'lar eslesdi — basarili
}

/* Flash_Read_Version / Flash_Write_Version kaldırıldı.
 * Firmware versiyonu artık SlotMeta_t.slot_a_version alanında saklanır.
 * (boot_flow.c: SlotMeta_Write çağrısında versiyon kaydedilir) */

/* =========================================================================
 * KEY_STORE Fonksiyonlari — Kalici AES Master Key (Page 15, 0x08007800)
 *
 * Sayfa 15 bootloader alani icinde; firmware update sadece app sayfalarini
 * (16-126) sildigi icin bu sayfa asla bozulmaz.
 *
 * Sayfa duzeni:
 *   +0  [4B] : KEY_STORE_MAGIC (0xAE5CAFE5)
 *   +4  [32B]: master_key[32]
 *   +36 [1B] : key_crc8 (32 byte key uzerinde basit CRC-8)
 * ========================================================================= */

/* Yerel yardimci: 8-bit CRC (polinom 0x07, SMBUS uyumlu) */
static uint8_t keystore_crc8(const uint8_t *data, uint32_t len)
{
  uint8_t crc = 0xFF;
  uint32_t i;
  for (i = 0; i < len; i++) {
    uint8_t j;
    crc ^= data[i];
    for (j = 0; j < 8; j++) {
      if (crc & 0x80)
        crc = (crc << 1) ^ 0x07;
      else
        crc <<= 1;
    }
  }
  return crc;
}

/*
 * KeyStore_Write — Yeni AES master key'i Flash'a kaydet
 *
 * key : 32 byte yeni AES-256 anahtari
 *
 * Islem:
 *   1. Sayfa 15'i sil (0xFF ile dolar)
 *   2. KEY_STORE_MAGIC yaz (+0)
 *   3. 32 byte key yaz (+4)
 *   4. CRC-8 yaz (+36)
 */
void KeyStore_Write(const uint8_t *key)
{
  uint32_t addr = KEY_STORE_ADDRESS;
  uint32_t magic = KEY_STORE_MAGIC;
  uint8_t crc = keystore_crc8(key, 32);
  uint32_t i;

  HAL_FLASH_Unlock();

  /* Sayfa 15'i sil */
  FLASH_EraseInitTypeDef erase;
  erase.TypeErase   = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = KEY_STORE_ADDRESS;
  erase.NbPages     = 1;
  uint32_t error;
  HAL_FLASHEx_Erase(&erase, &error);

  /* MAGIC yaz (4 byte = 2 halfword) */
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr,
                    (uint16_t)(magic & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + 2,
                    (uint16_t)((magic >> 16) & 0xFFFF));

  /* 32 byte key yaz (2 byte adimlarla) */
  addr += 4;
  for (i = 0; i < 32; i += 2) {
    uint16_t hw = (uint16_t)key[i] | ((uint16_t)key[i + 1] << 8);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, hw);
  }

  /* CRC-8 yaz (1 byte + 0xFF pad = halfword) */
  addr += 32;
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr,
                    (uint16_t)crc | 0xFF00);

  HAL_FLASH_Lock();
}

/*
 * KeyStore_Read — Kayitli AES master key'i oku
 *
 * key_out : 32 byte cikis tamponu
 *
 * Donus: 1 = gecerli key bulundu ve kopyalandi
 *         0 = KEY_STORE bos veya bozuk (DEFAULT_AES_KEY kullan)
 */
uint8_t KeyStore_Read(uint8_t *key_out)
{
  volatile uint32_t *magic_ptr = (volatile uint32_t *)KEY_STORE_ADDRESS;
  volatile uint8_t  *key_ptr   = (volatile uint8_t *)(KEY_STORE_ADDRESS + 4);
  volatile uint8_t  *crc_ptr   = (volatile uint8_t *)(KEY_STORE_ADDRESS + 36);

  /* Magic kontrol */
  if (magic_ptr[0] != KEY_STORE_MAGIC) {
    return 0; /* KEY_STORE bos veya bozuk */
  }

  /* CRC-8 dogrulama */
  uint8_t stored_crc  = *crc_ptr;
  uint8_t local_buf[32];
  uint32_t i;
  for (i = 0; i < 32; i++) {
    local_buf[i] = key_ptr[i];
  }
  uint8_t computed_crc = keystore_crc8(local_buf, 32);

  if (computed_crc != stored_crc) {
    return 0; /* CRC tutarsizligi — KEY_STORE bozuk */
  }

  /* Gecerli key — kopyala */
  memcpy(key_out, local_buf, 32);
  return 1;
}
