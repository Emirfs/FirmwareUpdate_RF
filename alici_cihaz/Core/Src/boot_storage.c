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

/*
 * Flash_Erase_Application — Uygulama Flash alanini sil (tüm alan)
 *
 * APP_PAGES (111) sayfayi tek tek siler.
 * Her sayfa silme sonrasi watchdog sifirlanir.
 * Toplam: 111 x ~5ms = ~555ms minimum (donanim hizina gore degisir).
 *
 * UYARI: Bu fonksiyon dondugunde uygulama kod alani bos olur!
 *        Kesinlikle yeni firmware yazilmadan calistirilmamali.
 *
 * NOT: Paket paket silme moduyla kullanildiginda bu fonksiyon cagrilmaz;
 *      bunun yerine Flash_Erase_Page her sayfa basinda cagrilir.
 */
void Flash_Erase_Application(void) {
  HAL_FLASH_Unlock();

  for (uint32_t i = 0; i < APP_PAGES; i++) {
    FLASH_EraseInitTypeDef single_erase;
    single_erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    single_erase.PageAddress = APP_ADDRESS + (i * FLASH_PAGE_SIZE); // Her sayfa 2KB ilerler
    single_erase.NbPages     = 1;

    uint32_t error;
    HAL_FLASHEx_Erase(&single_erase, &error);
    HAL_IWDG_Refresh(&hiwdg); // Her sayfada watchdog sifirla
  }

  HAL_FLASH_Lock();
}

/*
 * Flash_Erase_Page — Tek bir Flash sayfasini sil
 *
 * Paket paket silme modunda, her yeni sayfanin basina gelindiginde
 * (current_addr % FLASH_PAGE_SIZE == 0 oldugunda) bu fonksiyon cagrilir.
 * Yalnizca o sayfayi siler; diger sayfalara dokunmaz.
 *
 * page_addr: silinecek sayfanin baslangic adresi (2KB hizali olmali)
 *
 * Kullanim yeri: boot_flow.c — Data chunk yazma dongusu
 */
void Flash_Erase_Page(uint32_t page_addr) {
  HAL_FLASH_Unlock();

  FLASH_EraseInitTypeDef erase;
  erase.TypeErase   = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = page_addr;
  erase.NbPages     = 1;

  uint32_t error;
  HAL_FLASHEx_Erase(&erase, &error);
  HAL_IWDG_Refresh(&hiwdg); // Sayfa silme sirasinda watchdog sifirla

  HAL_FLASH_Lock();
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

/*
 * Flash_Read_Version — Boot flag sayfasindaki versiyon numarasini oku
 *
 * VERSION_ADDRESS = BOOT_FLAG_ADDRESS + 8 (MAGIC + FLAG'tan sonra)
 */
uint32_t Flash_Read_Version(void) {
  return *(volatile uint32_t *)VERSION_ADDRESS;
}

/*
 * Flash_Write_Version — Versiyon numarasini Flash'a kaydet
 *
 * Boot flag sayfasindaki VERSION_ADDRESS konumuna yazar.
 * Sayfa silinmemis olmali — clear_boot_flag() cagrilirsa versiyon silinir!
 * Bu yuzden Bootloader_Main'de clear_boot_flag ONCA cagrilir, SONRA versiyon yazilir.
 */
void Flash_Write_Version(uint32_t version) {
  HAL_FLASH_Unlock();
  /* 32-bit degeri iki halfword yazmasi ile kaydet */
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, VERSION_ADDRESS,
                    (uint16_t)(version & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, VERSION_ADDRESS + 2,
                    (uint16_t)((version >> 16) & 0xFFFF));
  HAL_FLASH_Lock();
}
