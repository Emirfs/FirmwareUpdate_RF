/*
 * boot_flow.c — Bootloader Ana Akis ve Uygulama Atlama
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * Alici cihazin iki temel islevini icerir:
 *
 * 1) jump_to_application()
 *    Flash'ta gecerli bir uygulama varsa kontrolu ona devreder.
 *    - APP_ADDRESS'deki MSP degerini dogrular (0x2000xxxx olmali)
 *    - Uygulamanin ilk 192 byte'ini RAM'e kopyalar (SYSCFG remap icin)
 *    - SYSCFG'yi RAM'den boot yapacak sekilde ayarlar
 *    - Tum interrupt'lari devre disi birakir, HAL'i deinitialize eder
 *    - MSP'yi ayarlar ve uygulama reset handler'ina zıplar
 *
 * 2) Bootloader_Main()
 *    RF uzerinden firmware guncellemesini gerceklestirir:
 *    a. Si4432 init + kontrol
 *    b. Resume durumunu oku (kaldigi yer), BOOT_ACK payload'inda gonderici'ye bildir
 *    c. CMD_METADATA bekle → metadata ACK → Resume_Init (ilk kez ise)
 *    d. FLASH_ERASE_DONE gonder (hemen — on silme yok, sayfa sayfa silinecek)
 *    e. DATA_CHUNK paketlerini al → 4 chunk biriktir
 *    f. Her 4 chunk'ta: CRC-32 dogrula → AES-256 sifre coz
 *       → Sayfa sinirinda Flash_Erase_Page → Flash_Write_Data
 *       → Her tamamlanan sayfada Resume_SavePageDone
 *    g. Final CRC dogrulama → UPDATE_COMPLETE veya UPDATE_FAILED
 *    h. Basarili ise uygulamaya atla
 *
 * ─── AES SIFRELEME ────────────────────────────────────────────────────────
 * Her 128 byte sifresiz veri PC tarafindan AES-256-CBC ile sifrelenir:
 *   [IV:16][Encrypted:128][CRC32_of_Encrypted:4] = 148 byte paket
 * AES key DEFAULT_AES_KEY sabiti ile eslesmeli (PC tarafinda ayni key).
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - DEFAULT_AES_KEY: PC'deki key ile ayni olmali (su an "1234567890..." ASCII)
 * - RF_UPDATE_TIMEOUT: Paket bekleme suresi (rf_bootloader.h, 60 sn)
 * - fw_assembly_buf boyutu: 200 byte (en az FW_FULL_PACKET_SIZE = 148 olmali)
 *
 * ─── BAGIMLILIKLAR ────────────────────────────────────────────────────────
 * boot_rf.c      → RF_SendPacket, RF_WaitForPacket
 * boot_storage.c → Flash_Erase_Page, Flash_Write_Data, Calculate_CRC32
 *                  Resume_Init, Resume_GetStartPacket, Resume_SavePageDone
 * boot_led.c     → LED_Bootloader, LED_Error, LED_Success, LED_Transfer
 * aes.c          → AES_init_ctx_iv, AES_CBC_decrypt_buffer
 * sha256.c        → SHA256_Init/Update/Final — firmware hash (sade SHA-256)
 * ed25519_verify.c → ed25519_verify() — RFC 8032 Ed25519 imza dogrulama
 * sha512.c         → SHA512_* — ed25519_verify icinde dahili kullanim
 */

#include "boot_flow.h"

#include "boot_led.h"
#include "boot_rf.h"
#include "boot_storage.h"

#include "aes.h"
#include "ed25519_verify.h"
#include "iwdg.h"
#include "main.h"
#include "neopixel.h"
#include "rf_bootloader.h"
#include "sha256.h"
#include "si4432.h"
#include <string.h>

/* ─── Stack Canary ─────────────────────────────────────────────────────────
 * Linker scriptten gelen _stack_bottom sembolunun adresine 0xDEADBEEF yaz.
 * Eger stack buraya kadar buyurse deger bozulur — IWDG refresh noktalari
 * bu degeri kontrol eder ve bozulursa sistemi resetler.
 * Cortex-M0'da MPU olmadigi icin bu yazilimsal koruma tek secenek. */
extern uint32_t _stack_bottom;
#define STACK_CANARY_MAGIC 0xDEADBEEFUL

static void stack_canary_init(void) {
    *(volatile uint32_t *)&_stack_bottom = STACK_CANARY_MAGIC;
}

static void stack_canary_check(void) {
    if (*(volatile uint32_t *)&_stack_bottom != STACK_CANARY_MAGIC) {
        /* Stack tasması tespit edildi — acil reset */
        NVIC_SystemReset();
    }
}

/* ─── Nonce Üreteci ────────────────────────────────────────────────────────
 * STM32F030'da donanim TRNG yok. SysTick + HAL_GetTick + bir mixer
 * ile tahmin edilemez (ama kriptografik degil) rastgelelik saglanir.
 * Nonce, replay atagi onleme icin kullanilir — kriptografik kalite
 * gerekmiyor, sadece oturum basina benzersiz olmasi yeterli. */
static uint32_t generate_nonce(void) {
    uint32_t t  = HAL_GetTick();
    uint32_t sv = SysTick->VAL;
    /* Knuth multiplicative hash ile karıştır */
    return (t ^ (sv << 13) ^ (t >> 7)) * 2654435761UL;
}

/* AES-256 key: PC'deki rf_uploader.py'daki key ile birebir ayni olmali.
 * "1234567890123456789012345678901 2" ASCII = 32 byte
 * DEGISTIRMEK ICIN: PC'deki key ile ayni hex degerlerini kullan. */
static const uint8_t DEFAULT_AES_KEY[32] = {
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
    0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32};

static uint8_t AES_KEY[32]; // Aktif AES key (DEFAULT_AES_KEY'den kopyalanir)

/* Gelen 4 RF chunk'un biriktirilecegi tampon.
 * [IV:16][Encrypted:128][CRC32:4] = 148 byte */
static uint8_t fw_assembly_buf[FW_FULL_PACKET_SIZE];

/* Mevcut 148-byte paket icin alinan chunk sayisi (0..3, 4'te sifirlanir) */
static uint8_t fw_chunks_received;

/* RF paket sequence numarasi */
static uint16_t rf_seq_counter = 0;

/*
 * jump_to_application — Flash'taki uygulamaya atla
 *
 * STM32 bootloader'dan uygulama koduna gecis icin ozel hazirlık gerekir:
 *
 * 1. MSP dogrulamasi: APP_ADDRESS'deki ilk deger gecerli RAM adresi olmali
 *    (0x2000xxxx). Degilse uygulama yoktur, geri don.
 *
 * 2. Vektör tablosu remapping: STM32F030 'VTOR' registeri yoktur.
 *    Bunun yerine SYSCFG ile adres 0x00000000 → SRAM'e yonlendirilir.
 *    Bunun icin uygulama Flash'inin ilk 192 byte'i (48 x uint32) RAM'e
 *    kopyalanir (vektör tablosu + MSP + reset handler).
 *
 * 3. HAL/SysTick/NVIC temizligi: Bootloader'ın interrupt'larini ve
 *    donanim ayarlarini sifirla ki uygulama temiz baslayabilsin.
 *
 * 4. MSP ayarla ve reset handler'a atla (uygulama baslıyor).
 */
void jump_to_application(void) {
  uint32_t app_msp = *(volatile uint32_t *)APP_ADDRESS; // APP_ADDRESS ilk word = MSP degeri

  /* Gecerli uygulama var mi? MSP 0x2000xxxx olmali (SRAM bolgesi) */
  if ((app_msp & 0xFFF00000) != 0x20000000) {
    return; // Uygulama yok — bootloader'da kal
  }

  /* Uygulama reset handler adresi (APP_ADDRESS+4'te sakli) */
  uint32_t jump_addr = *(volatile uint32_t *)(APP_ADDRESS + 4);
  void (*app_reset_handler)(void) = (void (*)(void))jump_addr;

  /* STM32F030'da VTOR yok: vektör tablosunu RAM'e kopyala
   * SYSCFG ile 0x00000000 → SRAM yonlendirmesi yapilacak */
  volatile uint32_t *dst = (volatile uint32_t *)0x20000000; // RAM basi
  volatile uint32_t *src = (volatile uint32_t *)APP_ADDRESS; // App Flash basi
  for (uint32_t i = 0; i < 48; i++) { // 48 x 4 byte = 192 byte (vektör tablosu)
    dst[i] = src[i];
  }

  /* SYSCFG saatini ac ve MEM_MODE=11 (SRAM remap) ayarla */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE) |
                  (0x03 << SYSCFG_CFGR1_MEM_MODE_Pos);

  /* Interrupt'lari engelle — gecis sirasinda kesme olmamali */
  __disable_irq();

  /* HAL ve saat deinitialize: uygulama kendi saatini ayarlayacak */
  HAL_RCC_DeInit();
  HAL_DeInit();

  /* SysTick durdur: uygulama kendisi baslatiyor */
  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL  = 0;

  /* Tum NVIC interrupt'larini devre disi birak ve bekleyen olanlari temizle */
  for (uint8_t i = 0; i < 8; i++) {
    NVIC->ICER[i] = 0xFFFFFFFF; // Interrupt disable
    NVIC->ICPR[i] = 0xFFFFFFFF; // Pending temizle
  }

  /* MSP'yi uygulamanin MSP'sine ayarla, reset handler'a atla */
  __set_MSP(app_msp);
  app_reset_handler(); // Artik uygulama caliyor

  while (1) {} // Buraya hic ulasılmaz
}

/*
 * Bootloader_Main — RF uzerinden firmware guncelleme ana dongusu
 *
 * Bu fonksiyon main.c'den ya boot flag aktif oldugunda ya da gecerli
 * uygulama bulunamadiginda cagrilir.
 *
 * Akis:
 *   ADIM 1: Si4432 baslat ve kontrol et
 *   ADIM 2: BOOT_ACK gonder, CMD_METADATA al (sonsuz dongu)
 *   ADIM 3: Flash sil (238KB, uzun surer)
 *   ADIM 4: FLASH_ERASE_DONE gonder, ACK bekle
 *   ADIM 5: DATA_CHUNK paketlerini al, 4 chunk biriktir
 *   ADIM 6: 4 chunk tamam → CRC-32 dogrula → AES sifre coz → Flash yaz
 *   ADIM 7: Tum paketler bitti → Flash CRC dogrulama
 *   ADIM 8: UPDATE_COMPLETE/FAILED gonder → uygulamaya atla
 */
void Bootloader_Main(void) {
  struct AES_ctx aes_ctx;       // AES context (sifre cozme icin)
  Firmware_Metadata_t metadata; // Gondericiden gelen firmware bilgileri
  uint32_t total_packets = 0;   // Beklenen toplam paket sayisi (metadata'dan)
  SHA256_CTX sha_ctx;           // SHA-256 birikimli hash (imza dogrulamasi icin)

  /* AES key'i default degerle yukle */
  memcpy(AES_KEY, DEFAULT_AES_KEY, 32);

  /* Stack canary'yi yaz — overflow tespiti icin */
  stack_canary_init();

  LED_Bootloader(); // NeoPixel turuncu — bootloader aktif

  /* ===================================================================
   * ADIM 1: Si4432 RF MODUL BASLATMA
   * =================================================================== */
  SI4432_Init();
  HAL_Delay(10);

  uint8_t dev = SI4432_ReadReg(0x00); // Device Type reg — 0x08 olmali
  if (dev != 0x08) {
    /* Si4432 bulunamadi — kirmizi yanıp sonsun, sonsuz dongu */
    LED_Error();
    while (1) {
      HAL_IWDG_Refresh(&hiwdg);
      LED_Error();
    }
  }

  /* ===================================================================
   * ADIM 1.5: KİMLİK DOĞRULAMA (NONCE CHALLENGE-RESPONSE)
   *
   * Gönderici RF_CMD_AUTH_REQUEST gönderene kadar bekle.
   * Bir nonce (4 byte) üret → RF_CMD_AUTH_CHALLENGE olarak gönder.
   * Gönderici nonce'u PC'ye iletir, PC AES-256-CBC ile şifreler:
   *   plaintext: nonce(4) + AUTH_PASSWORD(16) + padding(12) = 32 byte
   *   paket: IV(16) + AES256_CBC(AUTH_KEY, IV, plaintext)(32) = 48 byte
   * RF_CMD_AUTH alındığında çöz → nonce + şifre doğrula.
   * Başarısızsa sistemi durdur (flash'a asla yazmayız).
   * =================================================================== */
  {
    uint32_t nonce = generate_nonce();
    uint8_t  nonce_pld[4];
    nonce_pld[0] = (uint8_t)(nonce       & 0xFF);
    nonce_pld[1] = (uint8_t)((nonce >> 8) & 0xFF);
    nonce_pld[2] = (uint8_t)((nonce >>16) & 0xFF);
    nonce_pld[3] = (uint8_t)((nonce >>24) & 0xFF);

    uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
    uint16_t rx_seq;
    uint8_t rx_pld_len;
    uint8_t auth_ok = 0;
    uint32_t auth_start = HAL_GetTick();

    /* 30 saniye icinde auth basarili olmazsa hata */
    while (!auth_ok && (HAL_GetTick() - auth_start) < 30000) {
      HAL_IWDG_Refresh(&hiwdg);
      stack_canary_check();

      /* AUTH_CHALLENGE yayinla (gonderici AUTH_REQUEST gonderince de cevap) */
      RF_SendPacket(RF_CMD_AUTH_CHALLENGE, rf_seq_counter, nonce_pld, 4);

      /* 2 saniye gelen paket bekle */
      if (!RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 2000)) {
        continue;
      }

      if (rx_type == RF_CMD_AUTH_REQUEST) {
        /* Gonderici hazir — hemen challenge gonder */
        RF_SendPacket(RF_CMD_AUTH_CHALLENGE, rf_seq_counter, nonce_pld, 4);
        continue;
      }

      if (rx_type == RF_CMD_AUTH && rx_pld_len == 48) {
        /* Auth paketi geldi: [IV:16][Encrypted:32]
         * AES-256-CBC ile coz → nonce(4) + AUTH_PASSWORD(16) + pad(12) */
        uint8_t *iv_a  = &rx_pld[0];
        uint8_t  plain[32];
        memcpy(plain, &rx_pld[16], 32);

        struct AES_ctx auth_ctx;
        AES_init_ctx_iv(&auth_ctx, DEFAULT_AUTH_KEY, iv_a);
        AES_CBC_decrypt_buffer(&auth_ctx, plain, 32);

        /* Gelen nonce'u yeniden olustur */
        uint32_t recv_nonce = (uint32_t)plain[0]
                            | ((uint32_t)plain[1] << 8)
                            | ((uint32_t)plain[2] << 16)
                            | ((uint32_t)plain[3] << 24);

        int nonce_ok = (recv_nonce == nonce);
        int pass_ok  = (memcmp(&plain[4], DEFAULT_AUTH_PASSWORD, 16) == 0);

        if (nonce_ok && pass_ok) {
          RF_SendPacket(RF_CMD_AUTH_ACK, rx_seq, NULL, 0);
          auth_ok = 1;
        } else {
          RF_SendPacket(RF_CMD_AUTH_NACK, rx_seq, NULL, 0);
          /* Yeni nonce uret — sonraki deneme icin farkli olsun */
          nonce = generate_nonce();
          nonce_pld[0] = (uint8_t)(nonce       & 0xFF);
          nonce_pld[1] = (uint8_t)((nonce >> 8) & 0xFF);
          nonce_pld[2] = (uint8_t)((nonce >>16) & 0xFF);
          nonce_pld[3] = (uint8_t)((nonce >>24) & 0xFF);
        }
      }
    }

    if (!auth_ok) {
      /* Auth zaman asimi veya basarisiz — bootloader'da kal, flash dokunma */
      LED_Error();
      while (1) { HAL_IWDG_Refresh(&hiwdg); }
    }
  }

  /* ===================================================================
   * ADIM 2: BOOT_ACK GONDER → CMD_METADATA BEKLE
   *
   * Resume durumunu kontrol et: daha onceki transfer yari kesilmis mi?
   * Eger kesilmisse, resume_start_packet > 0 olur.
   * Bu deger BOOT_ACK payload'inda (4 byte) gonderici'ye bildirilir.
   * Gonderici bu sayiyi okuyarak ilk N paketi RF'e iletmeden gecer
   * (PC'den okur, ACK verir, aliciya gondermez — alici zaten yazmis).
   *
   * Gonderici her 2 saniyede BOOT_REQUEST gonderir.
   * Biz BOOT_ACK ile cevap verir, metadata bekliyoruz.
   * Metadata alininca ACK donderip metadata dongusunden cikiyoruz.
   * =================================================================== */
  {
    /* Resume noktasini oku (ilk kez ise 0, kaldigi yerden ise N) */
    uint32_t resume_start_packet = Resume_GetStartPacket();

    /* BOOT_ACK payload: [resume_start: 4 byte, little-endian] */
    uint8_t boot_ack_pld[4];
    boot_ack_pld[0] = (uint8_t)(resume_start_packet & 0xFF);
    boot_ack_pld[1] = (uint8_t)((resume_start_packet >> 8) & 0xFF);
    boot_ack_pld[2] = (uint8_t)((resume_start_packet >> 16) & 0xFF);
    boot_ack_pld[3] = (uint8_t)((resume_start_packet >> 24) & 0xFF);

    uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
    uint16_t rx_seq;
    uint8_t rx_pld_len;
    uint8_t got_metadata = 0;

    while (!got_metadata) {
      HAL_IWDG_Refresh(&hiwdg);
      stack_canary_check();

      /* BOOT_ACK gonder — payload'da kaldigi yer bilgisi var */
      RF_SendPacket(RF_CMD_BOOT_ACK, rf_seq_counter++, boot_ack_pld, 4);

      NeoPixel_SetAll(255, 80, 0); // Turuncu: bootloader bekleme
      NeoPixel_Show();

      /* 1 saniye paket bekle */
      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 1000)) {
        if (rx_type == RF_CMD_METADATA && rx_pld_len >= 12) {
          /* Metadata alindi — firmware_size, version, crc32 doldur */
          memcpy(&metadata, rx_pld, sizeof(Firmware_Metadata_t));
          total_packets =
              (metadata.firmware_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE;
          got_metadata = 1;

          RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Metadata onaylandi
        } else if (rx_type == RF_CMD_BOOT_REQUEST) {
          /* Gonderici BOOT_REQUEST gonderdi — BOOT_ACK ile cevapla ve
           * hemen metadata bekle (dongüye dönüp tekrar BOOT_ACK gönderme,
           * yoksa sender'in metadata RF denemelerini tasiyor). */
          RF_SendPacket(RF_CMD_BOOT_ACK, rf_seq_counter++, boot_ack_pld, 4);

          /* Sender'in metadata gondermesi icin 5 saniye bekle */
          if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
            if (rx_type == RF_CMD_METADATA && rx_pld_len >= 12) {
              memcpy(&metadata, rx_pld, sizeof(Firmware_Metadata_t));
              total_packets =
                  (metadata.firmware_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE;
              got_metadata = 1;
              RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
            }
          }
          HAL_IWDG_Refresh(&hiwdg);
        }
      }

      NeoPixel_Clear(); // LED off
      NeoPixel_Show();
      if (!got_metadata) HAL_Delay(200); // Kisa bekleme sonra tekrar BOOT_ACK gonder
    }

    /* Resume state henuz baslatilmamissa (ilk transfer), simdi baslat.
     * RESUME_MAGIC zaten yaziliysa (kaldigi yerden devam), dokunma. */
    if (*(volatile uint32_t *)RESUME_STATE_ADDRESS != RESUME_MAGIC) {
      Resume_Init(total_packets); // Boot flag sayfasina magic + total yaz
    }
  }

  /* ===================================================================
   * ADIM 3: ANLIK HAZIR BİLDİRİMİ (FLASH_ERASE_DONE)
   *
   * Onceki tasarimda burada tum Flash siliniyordu (~555ms).
   * Artik silme islemini sayfa sayfa yapiyoruz: her 128-byte yazmadan
   * once, o adres sayfa basindaysa (current_addr % FLASH_PAGE_SIZE == 0)
   * sadece o sayfayi siliyoruz. Bu yaklasimla:
   *   - Gonderici cok daha erken veri gondermesine basliyor.
   *   - Kalan sayfalar dokunulmadan kaliyor (resume icin guvenli).
   *   - Enerji kesintisinde sadece son yarim sayfa etkileniyor.
   *
   * FLASH_ERASE_DONE artik "gondermeye hazir" anlamina geliyor.
   * =================================================================== */
  NeoPixel_SetAll(0, 100, 255); // Acik mavi: hazirlaniliyor
  NeoPixel_Show();

  /* ===================================================================
   * ADIM 4: FLASH_ERASE_DONE GONDER → ACK BEKLE
   * Gonderici bu ACK'i alinca paket gondermeye baslar.
   * =================================================================== */
  {
    uint8_t sent = 0;
    for (uint8_t retry = 0; retry < 10 && !sent; retry++) {
      RF_SendPacket(RF_CMD_FLASH_ERASE_DONE, rf_seq_counter, NULL, 0);

      uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
      uint16_t rx_seq;
      uint8_t rx_pld_len;
      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 2000)) {
        if (rx_type == RF_CMD_ACK) {
          sent = 1; // Gonderici hazir mesajini onayladi
        }
      }
      HAL_IWDG_Refresh(&hiwdg);
    }
    rf_seq_counter++;

    if (!sent) {
      /* Gonderici FLASH_ERASE_DONE'i onaylamadi — sistem sıfırlansın,
       * tekrar bootloader'a girince yeni bir transfer baslatilir. */
      LED_Error();
      NVIC_SystemReset();
    }
  }

  /* ===================================================================
   * ADIM 5-6: DATA_CHUNK PAKETLERI AL + ISLE
   *
   * Her 148-byte firmware paketi 4 RF chunk halinde gelir:
   *   chunk 0: data[0..47]    (48 byte)
   *   chunk 1: data[48..95]   (48 byte)
   *   chunk 2: data[96..143]  (48 byte)
   *   chunk 3: data[144..147] ( 4 byte)
   *
   * 4 chunk biriktikten sonra:
   *   fw_assembly_buf[0..15]   = IV (AES baslangic vektoru)
   *   fw_assembly_buf[16..143] = Sifrelenmis veri (128 byte)
   *   fw_assembly_buf[144..147]= CRC-32 (sifrelenmis verinin CRC'si)
   *
   * Islem:
   *   1. CRC-32 dogrula (bozuk paket → UPDATE_FAILED)
   *   2. AES-256-CBC sifre coz (IV + AES_KEY kullanarak)
   *   3. Ilk pakette MSP gecerliligi kontrol et
   *   4. Sayfa basindaysak o sayfayi sil (Flash_Erase_Page)
   *   5. Flash'a yaz ve dogrula
   *   6. Her tamamlanan sayfada Resume_SavePageDone cagir
   *
   * RESUME: packets_received ve current_addr, daha once tamamlanan
   *         noktadan baslar (resume_start_packet). Gonderici de ayni
   *         noktadan gondermesi nedeniyle senkronize calisir.
   * =================================================================== */
  {
    /* Resume noktasini tekrar oku (ADIM 2'de ayni deger, tutarlilik icin) */
    uint32_t resume_start = Resume_GetStartPacket();
    uint32_t packets_received = resume_start; // Resume noktasindan baslat
    uint32_t current_addr = APP_ADDRESS + resume_start * FW_PACKET_SIZE;

    fw_chunks_received = 0;
    memset(fw_assembly_buf, 0, sizeof(fw_assembly_buf));

    /* Sade SHA-256 baslat — Ed25519 imzanin kapsadigi firmware hash'i.
     * NOT: HMAC k_ipad onek YOK — saf SHA-256(padded_firmware).
     * Resume durumunda: zaten yazilmis sayfalar once hash'e dahil edilir. */
    SHA256_Init(&sha_ctx);
    if (resume_start > 0) {
      SHA256_Update(&sha_ctx, (const uint8_t *)APP_ADDRESS,
                   resume_start * FW_PACKET_SIZE);
    }

    while (packets_received < total_packets) {
      HAL_IWDG_Refresh(&hiwdg);
      stack_canary_check();
      LED_Transfer(packets_received); // Mavi/mor yanip sonsun — transfer gostergesi

      uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
      uint16_t rx_seq;
      uint8_t rx_pld_len;

      /* Paket bekle — RF_UPDATE_TIMEOUT (60sn) icinde gelmezse hata */
      if (!RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len,
                            RF_UPDATE_TIMEOUT)) {
        /* Zaman asimi — guncelleme basarisiz */
        RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                      (uint8_t[]){RF_ERR_TIMEOUT}, 1);
        LED_Error();
        return;
      }

      if (rx_type != RF_CMD_DATA_CHUNK) {
        continue; // Baska tip paket geldi — yoksay, tekrar bekle
      }

      /* Minimum payload: chunk_idx(1) + chunk_cnt(1) + data(>=1) = 3 byte */
      if (rx_pld_len < 3) {
        RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0); // Gecersiz paket
        continue;
      }

      uint8_t chunk_idx   = rx_pld[0]; // Bu parcanin numarasi (0, 1, 2, 3)
      uint8_t chunk_cnt   = rx_pld[1]; // Toplam parca sayisi (4 olmali)
      uint8_t data_len    = rx_pld_len - 2; // Asil veri uzunlugu
      uint8_t *chunk_data = &rx_pld[2];     // Asil veri

      /* Chunk sirasini kontrol et: sirayla gelmeli (0,1,2,3) */
      if (chunk_idx != fw_chunks_received || chunk_cnt != RF_CHUNKS_PER_PACKET) {
        RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0); // Yanlis sira veya sayı
        continue;
      }

      /* Tampon tasmasini kontrol et */
      uint32_t offset = chunk_idx * RF_CHUNK_DATA_SIZE;
      if (offset + data_len > sizeof(fw_assembly_buf)) {
        RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0);
        continue;
      }

      /* Chunk'u assembly buffer'ına kopyala */
      memcpy(&fw_assembly_buf[offset], chunk_data, data_len);
      fw_chunks_received++;

      RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Chunk alindi onaylandi

      /* 4 chunk tamam → 148 byte'i isle */
      if (fw_chunks_received >= RF_CHUNKS_PER_PACKET) {
        fw_chunks_received = 0; // Sonraki paket icin sifirla

        /* Assembly buffer yapisi:
         * [0..15]   = IV (AES baslangic vektoru)
         * [16..143] = AES-256-CBC sifrelenmis veri (128 byte)
         * [144..147]= CRC-32 (sifrelenmis verinin CRC'si, little-endian) */
        uint8_t *iv_ptr        = &fw_assembly_buf[0];
        uint8_t *encrypted_ptr = &fw_assembly_buf[16];
        uint32_t received_crc;
        memcpy(&received_crc, &fw_assembly_buf[144], 4); // Little-endian CRC oku

        /* CRC-32 dogrulama: sifrelenmis verinin bozulmadigi kontrol edilir */
        uint32_t computed_crc = Calculate_CRC32(encrypted_ptr, 128);
        if (computed_crc != received_crc) {
          RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                        (uint8_t[]){RF_ERR_CRC_FAIL}, 1);
          LED_Error();
          return;
        }

        /* AES-256-CBC sifre coz: IV + AES_KEY ile encrypted_ptr'yi yerinde coz */
        AES_init_ctx_iv(&aes_ctx, AES_KEY, iv_ptr);
        AES_CBC_decrypt_buffer(&aes_ctx, encrypted_ptr, 128);
        /* NOT: sifre cozme yerinde yapilir — encrypted_ptr artik duz metin */

        /* Ilk pakette MSP gecerliligi kontrol et (yanlis firmware yazma koruması) */
        if (current_addr == APP_ADDRESS) {
          uint32_t msp_val = *(uint32_t *)encrypted_ptr; // Uygulamanin ilk word'u MSP
          if ((msp_val & 0xFFF00000) != 0x20000000) {
            /* Gecersiz MSP → yanlis firmware veya yanlis hedef cihaz */
            RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                          (uint8_t[]){RF_ERR_INVALID_MSP}, 1);
            LED_Error();
            return;
          }
        }

        /* Sayfa basina gelindiyse (her 2KB'de bir) o sayfayi sil.
         * 128 byte paket boyutu ile 2048 byte sayfa boyutu: 2048/128 = 16 paket/sayfa.
         * current_addr % FLASH_PAGE_SIZE == 0 kosulu sayfa sinirlarinda saglanir. */
        if (current_addr % FLASH_PAGE_SIZE == 0) {
          Flash_Erase_Page(current_addr); // Yalnizca bu sayfayi sil (~5ms)
        }

        /* Flash'a yaz (sifre cozulmus 128 byte) */
        Flash_Write_Data(current_addr, encrypted_ptr, 128);

        /* SHA-256 birikimli hash: sifre cozulmus veriyi hash'e ekle.
         * NOT: Flash yazimından SONRA hash'liyoruz — verify hatasindan once
         * hash'e eklemek tutarsizlik yaratirdi. */
        SHA256_Update(&sha_ctx, encrypted_ptr, FW_PACKET_SIZE);

        /* Yaz-oku dogrulama: yazilan veri dogru mu? */
        if (!Flash_Verify_Data(current_addr, encrypted_ptr, 128)) {
          RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                        (uint8_t[]){RF_ERR_FLASH_VERIFY}, 1);
          LED_Error();
          return;
        }

        current_addr += FW_PACKET_SIZE; // Sonraki yazma adresi (128 byte ilerle)
        packets_received++;             // Alinan paket sayacini artir

        /* Her tamamlanan Flash sayfasinda (16 paket = 2KB) resume durumunu kaydet.
         * Bu sayede cihaz resetlenince kaldigi yerden devam edebilir.
         * Ornek: packets_received=16 → sayfa 0 tamam → Resume_SavePageDone(0) */
        if (packets_received % PACKETS_PER_PAGE == 0) {
          uint32_t page_done = (packets_received / PACKETS_PER_PAGE) - 1;
          Resume_SavePageDone(page_done); // Bitmap'e 0x0000 yaz
        }

        memset(fw_assembly_buf, 0, sizeof(fw_assembly_buf)); // Tampon temizle
      }
    }
  }

  /* ===================================================================
   * ADIM 6.5: ED25519 DİJİTAL İMZA DOĞRULAMA (RFC 8032)
   *
   * Akış:
   *   1. Sender 64-byte Ed25519 imzasini 2 RF_CMD_SIG_CHUNK'ta iletir:
   *      chunk 0: [0x00][imza[0..31]]  = 33 byte payload
   *      chunk 1: [0x01][imza[32..63]] = 33 byte payload
   *   2. SHA-256 hash'ini tamamla → fw_hash (32 byte)
   *   3. ed25519_verify(ED25519_PUBLIC_KEY, fw_hash, 32, imza) cagir
   *   4. Basarisizsa UPDATE_FAILED — flash'ta eski firmware kaliyor (resume)
   *
   * Gizlilik: Private key hicbir zaman bu cihaza girmez.
   * =================================================================== */
  {
    uint8_t sig_buf[64];                 /* 64-byte Ed25519 imzası */
    uint8_t sig_received[2] = {0, 0};   /* Her chunk alındı mı? */
    uint32_t sig_start = HAL_GetTick();

    NeoPixel_SetAll(100, 0, 200);        /* Mor: imza bekleniyor */
    NeoPixel_Show();

    while ((!sig_received[0] || !sig_received[1]) &&
           (HAL_GetTick() - sig_start) < 15000) {
      HAL_IWDG_Refresh(&hiwdg);
      stack_canary_check();

      uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
      uint16_t rx_seq;
      uint8_t rx_pld_len;

      if (!RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
        continue;
      }

      /* SIG_CHUNK: [idx:1][data:32] = 33 byte */
      if (rx_type == RF_CMD_SIG_CHUNK && rx_pld_len == 33) {
        uint8_t idx = rx_pld[0];
        if (idx < 2) {
          memcpy(&sig_buf[idx * 32], &rx_pld[1], 32);
          sig_received[idx] = 1;
          RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
        }
      }
    }

    if (!sig_received[0] || !sig_received[1]) {
      RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                    (uint8_t[]){RF_ERR_TIMEOUT}, 1);
      LED_Error();
      return;
    }

    /* SHA-256 hash'ini tamamla → firmware'in kanonik hash'i */
    uint8_t fw_hash[32];
    SHA256_Final(fw_hash, &sha_ctx);

    /* Ed25519 imzasini dogrula (RFC 8032 Bolum 5.1.7) */
    int sig_ok = ed25519_verify(ED25519_PUBLIC_KEY, fw_hash, 32, sig_buf);

    if (sig_ok != 0) {
      /* Imza gecersiz — yetkisiz, degistirilmis veya bozuk firmware */
      for (uint8_t i = 0; i < 10; i++) {
        RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter,
                      (uint8_t[]){RF_ERR_SIG_FAIL}, 1);
        uint8_t rx_type2, rx_pld2[RF_MAX_PAYLOAD];
        uint16_t rx_seq2; uint8_t rx_pld_len2;
        if (RF_WaitForPacket(&rx_type2, &rx_seq2, rx_pld2, &rx_pld_len2, 2000)) {
          if (rx_type2 == RF_CMD_ACK) break;
        }
        HAL_IWDG_Refresh(&hiwdg);
      }
      rf_seq_counter++;
      LED_Error();
      return;
    }
    /* Imza gecerli — devam et */
  }

  /* ===================================================================
   * ADIM 7: FINAL CRC DOGRULAMA
   * Tum Flash'a yazilan veriyi bastan CRC-32 hesapla.
   * metadata.firmware_crc32 ile karsilastir.
   * Uyusmazlik → guncelleme basarisiz.
   * =================================================================== */
  NeoPixel_SetAll(0, 200, 200); // Acik mavi: CRC hesaplaniyor
  NeoPixel_Show();

  uint32_t flash_crc =
      Calculate_Flash_CRC32(APP_ADDRESS, metadata.firmware_size);

  if (flash_crc != metadata.firmware_crc32) {
    /* CRC uyusmazligi — Flash'a yanlis veri yazılmis veya bozulmus */
    for (int i = 0; i < 5; i++) {
      NeoPixel_SetAll(255, 0, 128); // Pembe: CRC hatası
      NeoPixel_Show();
      HAL_Delay(150);
      NeoPixel_Clear();
      NeoPixel_Show();
      HAL_Delay(150);
      HAL_IWDG_Refresh(&hiwdg);
    }

    /* Gondericiye hatayi bildir — 10 kez dene, ACK gelince dur */
    for (uint8_t i = 0; i < 10; i++) {
      RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter,
                    (uint8_t[]){RF_ERR_FW_CRC_MISMATCH}, 1);

      uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
      uint16_t rx_seq;
      uint8_t rx_pld_len;
      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
        if (rx_type == RF_CMD_ACK) {
          break; // Gonderici mesaji aldi
        }
      }
      HAL_IWDG_Refresh(&hiwdg);
    }
    rf_seq_counter++;
    LED_Error();
    return; // Guncelleme basarisiz — bootloader'da kal
  }

  /* ===================================================================
   * ADIM 8: BASARILI TAMAMLAMA
   * - Boot flag'ini temizle (bir sonraki reset'te uygulamaya gecilecek)
   * - Firmware versiyonunu Flash'a kaydet
   * - UPDATE_COMPLETE gonder, uygulamaya atla
   * =================================================================== */
  clear_boot_flag(); // Boot flag sayfasini sil — bir sonraki boot'ta app calisir

  Flash_Write_Version(metadata.firmware_version); // Versiyonu Flash'a kaydet

  /* UPDATE_COMPLETE gonder — 10 kez dene, ACK gelince dur */
  for (uint8_t i = 0; i < 10; i++) {
    RF_SendPacket(RF_CMD_UPDATE_COMPLETE, rf_seq_counter, NULL, 0);

    uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
    uint16_t rx_seq;
    uint8_t rx_pld_len;
    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
      if (rx_type == RF_CMD_ACK) {
        break; // Gonderici tamamlama mesajini aldi
      }
    }
    HAL_IWDG_Refresh(&hiwdg);
  }
  rf_seq_counter++;

  LED_Success(); // Yesil yanip sonsun — basarili guncelleme
  HAL_Delay(1000);

  jump_to_application(); // Yeni firmware'i calistir
}
