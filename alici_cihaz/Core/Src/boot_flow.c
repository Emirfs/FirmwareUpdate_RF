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
 */

#include "boot_flow.h"

#include "boot_led.h"
#include "boot_rf.h"
#include "boot_storage.h"

#include "aes.h"
#include "c25519.h"
#include "entropy.h"
#include "iwdg.h"
#include "main.h"
#include "neopixel.h"
#include "rf_bootloader.h"
#include "si4432.h"
#include <string.h>

/* Derleyici optimizasyonunu atlayan güvenli bellek sıfırlama.
 * Standart memset() "dead store" olarak kaldırılabilir; volatile zorunlu. */
static void secure_zero(void *ptr, uint32_t len) {
	volatile uint8_t *p = (volatile uint8_t *) ptr;
	while (len--) *p++ = 0;
}

/* Fallback AES key — ECDH basarisiz olursa ve KEY_STORE bossa kullanilir.
 * Normal akista bu key HICBIR ZAMAN kullanilmaz; ECDH session key kullanilir. */
static const uint8_t DEFAULT_AES_KEY[32] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
		0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
		0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
		0x31, 0x32 };

static uint8_t AES_KEY[32]; /* Aktif session key (ECDH shared secret) */

/* Gelen 4 RF chunk'un biriktirilecegi tampon.
 * [IV:16][Encrypted:128][CRC32:4] = 148 byte + biraз tolerans */
static uint8_t fw_assembly_buf[200];

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
	uint32_t app_msp = *(volatile uint32_t*) APP_ADDRESS; // APP_ADDRESS ilk word = MSP degeri

	/* Gecerli uygulama var mi? MSP 0x2000xxxx olmali (SRAM bolgesi) */
	if ((app_msp & 0xFFF00000) != 0x20000000) {
		return; // Uygulama yok — bootloader'da kal
	}

	/* Uygulama reset handler adresi (APP_ADDRESS+4'te sakli) */
	uint32_t jump_addr = *(volatile uint32_t*) (APP_ADDRESS + 4);
	void (*app_reset_handler)(void) = (void (*)(void))jump_addr;

	/* STM32F030'da VTOR yok: vektör tablosunu RAM'e kopyala
	 * SYSCFG ile 0x00000000 → SRAM yonlendirmesi yapilacak */
	volatile uint32_t *dst = (volatile uint32_t*) 0x20000000; // RAM basi
	volatile uint32_t *src = (volatile uint32_t*) APP_ADDRESS; // App Flash basi
	for (uint32_t i = 0; i < 48; i++) { // 48 x 4 byte = 192 byte (vektör tablosu)
		dst[i] = src[i];
	}

	/* SYSCFG saatini ac ve MEM_MODE=11 (SRAM remap) ayarla */
	__HAL_RCC_SYSCFG_CLK_ENABLE();
	SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE)
			| (0x03 << SYSCFG_CFGR1_MEM_MODE_Pos);

	/* Interrupt'lari engelle — gecis sirasinda kesme olmamali */
	__disable_irq();

	/* HAL ve saat deinitialize: uygulama kendi saatini ayarlayacak */
	HAL_RCC_DeInit();
	HAL_DeInit();

	/* SysTick durdur: uygulama kendisi baslatiyor */
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;

	/* Tum NVIC interrupt'larini devre disi birak ve bekleyen olanlari temizle */
	for (uint8_t i = 0; i < 8; i++) {
		NVIC->ICER[i] = 0xFFFFFFFF; // Interrupt disable
		NVIC->ICPR[i] = 0xFFFFFFFF; // Pending temizle
	}

	/* MSP'yi uygulamanin MSP'sine ayarla, reset handler'a atla */
	__set_MSP(app_msp);
	app_reset_handler(); // Artik uygulama caliyor

	while (1) {
	} // Buraya hic ulasılmaz
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
void Bootloader_Main(const uint8_t *pub_sender_hint) {
	struct AES_ctx aes_ctx;       // AES context (sifre cozme icin)
	Firmware_Metadata_t metadata; // Gondericiden gelen firmware bilgileri
	uint32_t total_packets = 0;   // Beklenen toplam paket sayisi (metadata'dan)

	/* ── ECDH private/public key uret ──────────────────────────────────────
	 * Her bootloader oturumunda yeni ephemeral key pair olusturulur.
	 * Private key hicbir zaman RF'e gitmez; yalnizca RAM'de yaslanir. */
	uint8_t ecdh_priv[32];
	uint8_t ecdh_pub[32];
	entropy_generate(ecdh_priv, 32); /* UID + ADC + SysTick */
	c25519_prepare(ecdh_priv); /* Bit clamp (Curve25519 gerekliligi) */
	HAL_IWDG_Refresh(&hiwdg); /* c25519_smult ~300-800ms — onceden sifirla */
	c25519_smult(ecdh_pub, c25519_base_x, ecdh_priv); /* pub = priv * G */

	/* AES session key baslangic degeri: KEY_STORE varsa kalici key yukle,
	 * yoksa DEFAULT. ECDH tamamlaninca session key ile ezilir. */
	if (!KeyStore_Read(AES_KEY)) {
		memcpy(AES_KEY, DEFAULT_AES_KEY, 32);
	}

	LED_Bootloader(); // NeoPixel turuncu — bootloader aktif

	/* 60s boşta bekleme için giriş zamanı — metadata ve veri transfer
	 * aşamalarında kullanılır; 60s boyunca hiç paket gelmezse temiz çıkış. */
	uint32_t bootloader_entry_tick = HAL_GetTick();

	/* ===================================================================
	 * ADIM 1: Si4432 RF MODUL BASLATMA
	 *
	 * EXTI4_15 (Si4432 nIRQ) polling tabanli SI4432_CheckRx() ile catisiyor:
	 * EXTI GPIO bekleyen bitini temizler ama Si4432 IRQ register'larini (0x03/0x04)
	 * temizlemez. Bu durum nIRQ'nun LOW kalmasina ve her polling cagrisinda
	 * sahte veri okunmasina yol acar. Bootloader boyunca polling kullanilacagi
	 * icin EXTI4_15 burada devre disi birakilir.
	 * =================================================================== */
	HAL_NVIC_DisableIRQ(EXTI4_15_IRQn);
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

		/* ── BOOT_ACK payload (36 byte) ────────────────────────────────────
		 * [resume_start:4][pub_receiver:32]
		 * pub_receiver = alicinin public key'i (gondericiye, RF uzerinden acik gider)
		 * Gonderici bunu gorur ama shared secret'i hesaplayamaz (ECDLP). */
		uint8_t boot_ack_pld[BOOT_ACK_PLD_SIZE];
		boot_ack_pld[0] = (uint8_t) (resume_start_packet & 0xFF);
		boot_ack_pld[1] = (uint8_t) ((resume_start_packet >> 8) & 0xFF);
		boot_ack_pld[2] = (uint8_t) ((resume_start_packet >> 16) & 0xFF);
		boot_ack_pld[3] = (uint8_t) ((resume_start_packet >> 24) & 0xFF);
		memcpy(&boot_ack_pld[4], ecdh_pub, 32); /* pub_receiver */

		uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
		uint16_t rx_seq;
		uint8_t rx_pld_len;
		uint8_t got_metadata = 0;
		uint8_t ecdh_done = 0; /* Session key turetti mi? */

		/* ── Hint tabanli ECDH ────────────────────────────────────────────
		 * main.c 3s penceresi BOOT_REQUEST'i alip pub_sender'i buraya iletmis.
		 * Sender BOOT_ACK aldiktan sonra BOOT_REQUEST gondermez; beklersek
		 * ecdh_done asla 1 olmaz ve sifreleme DEFAULT/KEY_STORE key'e dusar.
		 * Hemen ECDH yap, sender BOOT_ACK aldiktan sonra metadata gonderecek. */
		if (pub_sender_hint != NULL) {
			uint8_t shared[32];
			HAL_IWDG_Refresh(&hiwdg); /* c25519_smult ~300-800ms — onceden sifirla */
			c25519_smult(shared, pub_sender_hint, ecdh_priv);
			memcpy(AES_KEY, shared, 32);
			secure_zero(shared, 32);
			secure_zero(ecdh_priv, 32); /* Kullanilmayacak — temizle */
			ecdh_done = 1;
		}

		while (!got_metadata) {
			/* 60s boyunca metadata gelmedi → çıkış yap */
			if (HAL_GetTick() - bootloader_entry_tick > BOOTLOADER_IDLE_TIMEOUT_MS) {
				/* App vector table geçerliyse (Flash silinmemişse) boot flag temizle.
				 * Kısmen silinmiş Flash varsa boot flag KORUNUR: bir sonraki reset'te
				 * sistem resume için bootloader'da beklemeye devam eder. */
				uint32_t app_msp_chk = *(volatile uint32_t*) APP_ADDRESS;
				if ((app_msp_chk & 0xFFF00000) == 0x20000000) {
					clear_boot_flag(); /* App sağlam — bayrak temizlenebilir */
				}
				/* App geçersizse boot flag bırakılır: resume bitmap korunur */
				LED_Off();
				return;
			}

			HAL_IWDG_Refresh(&hiwdg);

			/* BOOT_ACK gonder — 36 byte payload (resume_start + pub_receiver) */
			RF_SendPacket(RF_CMD_BOOT_ACK, rf_seq_counter++, boot_ack_pld,
					BOOT_ACK_PLD_SIZE);

			NeoPixel_SetAll(255, 80, 0); // Turuncu: bootloader bekleme
			NeoPixel_Show();

			/* 1 saniye paket bekle */
			if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len,
					1000)) {

				/* ── BOOT_REQUEST ile gelen pub_sender → ECDH tamamla ── */
				if (rx_type == RF_CMD_BOOT_REQUEST
						&& rx_pld_len >= BOOT_REQUEST_PLD_SIZE && !ecdh_done) {
					/* pub_sender = gondericinin public key'i */
					uint8_t pub_sender[32];
					memcpy(pub_sender, rx_pld, 32);

					/* Shared secret = X25519(own_priv, pub_sender) */
					uint8_t shared[32];
					HAL_IWDG_Refresh(&hiwdg); /* c25519_smult ~300-800ms — onceden sifirla */
					c25519_smult(shared, pub_sender, ecdh_priv);

					/* Session key = shared secret (AES-256 key olarak kullan) */
					memcpy(AES_KEY, shared, 32);

					/* Private key'i RAM'den temizle — artik kullanilmayacak */
					secure_zero(ecdh_priv, 32);
					secure_zero(shared, 32);

					ecdh_done = 1;
				}

				if (rx_type == RF_CMD_METADATA && rx_pld_len >= 12) {
					/* Metadata alindi — firmware_size, version, crc32 doldur */
					memcpy(&metadata, rx_pld, sizeof(Firmware_Metadata_t));

					/* firmware_size dogrulama: 0 veya uygulama alani disinda → reddet */
					if (metadata.firmware_size == 0
							|| metadata.firmware_size > APP_AREA_SIZE) {
						RF_SendPacket(RF_CMD_UPDATE_FAILED, rx_seq,
								(uint8_t[] ) { RF_ERR_INVALID_MSP }, 1);
						LED_Error();
						return;
					}

					total_packets =
							(metadata.firmware_size + FW_PACKET_SIZE - 1)
									/ FW_PACKET_SIZE;
					got_metadata = 1;

					RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0); // Metadata onaylandi
				}

				/* ── KEY_UPDATE: Yeni master key al ── */
				if (rx_type == RF_CMD_KEY_UPDATE
						&& rx_pld_len >= KEY_UPDATE_PLD_SIZE && ecdh_done) {
					/* payload: [AES-CBC(session_key, IV=0, new_master_key):32][crc8:1]
					 * Python tarafi IV=0 ile sifreledi; biz de IV=0 ile cozuyoruz.
					 * Session key her seferinde tazedir (ECDH) → IV=0 guvenli. */
					struct AES_ctx kctx;
					uint8_t zero_iv[16] = { 0 };
					uint8_t full_new_key[32];
					memcpy(full_new_key, rx_pld, 32);
					AES_init_ctx_iv(&kctx, AES_KEY, zero_iv);
					AES_CBC_decrypt_buffer(&kctx, full_new_key, 32);

					/* CRC-8 dogrula */
					uint8_t recv_crc = rx_pld[32];
					/* basit CRC-8 hesapla */
					uint8_t calc_crc = 0xFF;
					for (int ci = 0; ci < 32; ci++) {
						calc_crc ^= full_new_key[ci];
						for (int bi = 0; bi < 8; bi++) {
							if (calc_crc & 0x80)
								calc_crc = (calc_crc << 1) ^ 0x07;
							else
								calc_crc <<= 1;
						}
					}

					if (calc_crc == recv_crc) {
						KeyStore_Write(full_new_key);
						RF_SendPacket(RF_CMD_KEY_UPDATE_ACK, rx_seq, NULL, 0);
					} else {
						RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0);
					}
					memset(full_new_key, 0, 32);
				}
			}

			NeoPixel_Clear(); // LED off
			NeoPixel_Show();
			HAL_Delay(200); // Kisa bekleme sonra tekrar BOOT_ACK gonder
		}

		/* ECDH tamamlanmadiysa güncellemeyi reddet — session key olmadan güvensiz */
		if (!ecdh_done) {
			RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
					(uint8_t[] ) { RF_ERR_AES_FAIL }, 1);
			LED_Error();
			return;
		}

		/* Private key her durumda temizle */
		secure_zero(ecdh_priv, 32);

		/* Resume state henuz baslatilmamissa (ilk transfer), simdi baslat.
		 * RESUME_MAGIC zaten yaziliysa (kaldigi yerden devam), dokunma. */
		if (*(volatile uint32_t*) RESUME_STATE_ADDRESS != RESUME_MAGIC) {
			Resume_Init(total_packets); // Boot flag sayfasina magic + total yaz
		}
	}

	/* ===================================================================
	 * ADIM 3: SLOT A → SLOT B YEDEKLEME (anti-brick güvencesi)
	 *
	 * Slot A'da geçerli bir uygulama varsa, yeni firmware yazılmadan
	 * önce mevcut içerik Slot B'ye kopyalanır (~7-8 saniye).
	 * Bu sayede:
	 *   - Güncelleme sırasında güç kesintisi → Slot A bozulur, ama
	 *     Slot B'deki yedekten geri yükleme (rollback) yapılabilir.
	 *   - Yeni firmware CRC hatası → anında rollback.
	 *   - Yeni firmware 3 boot onaylamazsa → trial timeout → rollback.
	 *
	 * SlotMeta.state = BACKED_UP: yedekleme tamam, Slot A yazılıyor.
	 * Güç kesintisi durumunda bu state korunur; main.c ilk açılışta
	 * Slot A MSP geçersizse state'i ROLLBACK'e çevirir.
	 *
	 * Resume aktifse (kaldığı yerden devam): yedekleme zaten yapılmış,
	 * SlotMeta.state == BACKED_UP → yeniden yedeklemeye gerek yok.
	 * =================================================================== */
	{
		SlotMeta_t meta;
		uint8_t meta_valid = SlotMeta_Read(&meta);

		uint8_t need_backup = 1U;
		if (meta_valid && meta.state == SLOT_STATE_BACKED_UP) {
			/* Resume senaryosu: yedekleme zaten tamamlandı */
			need_backup = 0U;
		}

		if (need_backup) {
			uint32_t app_msp_val = *(volatile uint32_t *) SLOT_A_ADDRESS;
			uint8_t slot_a_valid = ((app_msp_val & 0xFFF00000U) == 0x20000000U);

			if (slot_a_valid) {
				/* Yedekleme öncesi state'i kaydet — güç kesilirse korunur */
				SlotMeta_t backup_meta;
				if (!meta_valid) {
					/* İlk yedekleme: sıfırdan oluştur */
					backup_meta.state          = SLOT_STATE_BACKED_UP;
					backup_meta.trial_count    = 0U;
					backup_meta.max_trials     = 3U;
					backup_meta.reserved       = 0U;
					backup_meta.slot_a_version = 0U; /* güncelleme sonrası dolar */
					backup_meta.slot_b_version = 0U;
					backup_meta.slot_a_size    = 0U;
					backup_meta.slot_b_size    = 0U;
					backup_meta.slot_a_crc32   = 0U;
					backup_meta.slot_b_crc32   = 0U;
					backup_meta.confirm_flag   = 0xFFFFFFFFU;
				} else {
					backup_meta       = meta;
					backup_meta.state = SLOT_STATE_BACKED_UP;
				}
				SlotMeta_Write(&backup_meta);

				NeoPixel_SetAll(255, 80, 0); /* Turuncu: yedekleniyor */
				NeoPixel_Show();

				/* Slot A → Slot B kopyala (~7-8 saniye, IWDG beslenir) */
				if (!SlotA_BackupToB()) {
					/* Yedekleme başarısız — güvenli devam edemeyiz */
					RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
							(uint8_t[]) { RF_ERR_FLASH_WRITE }, 1);
					LED_Error();
					return;
				}

				/* Yedek CRC'yi kaydet */
				SlotMeta_t after_backup;
				SlotMeta_Read(&after_backup);
				after_backup.slot_b_size  = SLOT_A_SIZE;
				after_backup.slot_b_crc32 = Calculate_Flash_CRC32(SLOT_B_ADDRESS,
						SLOT_A_SIZE);
				SlotMeta_Write(&after_backup);
			}
			/* Slot A geçersizse (ilk programlama): yedek alınmaz, Slot B boş kalır */
		}
	}

	/* ===================================================================
	 * ADIM 4: HAZIR BİLDİRİMİ (FLASH_ERASE_DONE) GONDER → ACK BEKLE
	 *
	 * Yedekleme tamamlandı; gönderici artık veri gönderebilir.
	 * Her sayfa, gelen chunk'lar işlenirken sırayla silinir ve yazılır.
	 * =================================================================== */
	NeoPixel_SetAll(0, 100, 255); // Açık mavi: hazır
	NeoPixel_Show();

	{
		uint8_t sent = 0U;
		for (uint8_t retry = 0U; retry < 10U && !sent; retry++) {
			RF_SendPacket(RF_CMD_FLASH_ERASE_DONE, rf_seq_counter, NULL, 0);

			uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
			uint16_t rx_seq;
			uint8_t rx_pld_len;
			if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 2000)) {
				if (rx_type == RF_CMD_ACK) {
					sent = 1U;
				}
			}
			HAL_IWDG_Refresh(&hiwdg);
		}
		rf_seq_counter++;
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

		/* Son başarılı DATA_CHUNK alım zamanı; 60s geçerse temiz çıkış */
		uint32_t last_chunk_tick = HAL_GetTick();

		while (packets_received < total_packets) {
			HAL_IWDG_Refresh(&hiwdg);
			LED_Transfer(packets_received); // Mavi/mor yanip sonsun — transfer gostergesi

			uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
			uint16_t rx_seq;
			uint8_t rx_pld_len;

			/* 5s'de bir poll et; toplam 60s aktivite yoksa temiz çıkış */
			if (!RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
				if (HAL_GetTick() - last_chunk_tick > BOOTLOADER_IDLE_TIMEOUT_MS) {
					/* 60s boyunca hiç DATA_CHUNK gelmedi — gönderici kesilmiş.
					 * Boot flag'i silmiyoruz: resume bitmap geçerli, bir sonraki
					 * bağlantıda kaldığı yerden devam edilebilir. */
					LED_Off();
					return;
				}
				continue; /* Henüz 60s dolmadı — beklemeye devam */
			}

			if (rx_type != RF_CMD_DATA_CHUNK) {
				continue; // Baska tip paket geldi — yoksay, tekrar bekle
			}

			/* Minimum payload: chunk_idx(1) + chunk_cnt(1) + data(>=1) = 3 byte */
			if (rx_pld_len < 3) {
				RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0); // Gecersiz paket
				continue;
			}

			uint8_t chunk_idx = rx_pld[0]; // Bu parcanin numarasi (0, 1, 2, 3)
			uint8_t chunk_cnt = rx_pld[1]; // Toplam parca sayisi (4 olmali)
			uint8_t data_len = rx_pld_len - 2; // Asil veri uzunlugu
			uint8_t *chunk_data = &rx_pld[2];     // Asil veri

			/* Chunk sirasini kontrol et: sirayla gelmeli (0,1,2,3) */
			if (chunk_idx != fw_chunks_received || chunk_cnt != RF_CHUNKS_PER_PACKET) {
				if (chunk_idx == 0 && fw_chunks_received > 0) {
					/* Gonderici yeniden deneme yapiyor (retry): chunk 0 tekrar geldi.
					 * Sayaci ve tamponu sifirla, chunk 0'i normal islemeye devam et. */
					fw_chunks_received = 0;
					memset(fw_assembly_buf, 0, sizeof(fw_assembly_buf));
					/* fall through — chunk 0 normal islenir */
				} else {
					RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0); /* Yanlis sira */
					continue;
				}
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
			last_chunk_tick = HAL_GetTick(); /* Aktivite zamanlayicisini sifirla */

			/* 4 chunk tamam → 148 byte'i isle */
			if (fw_chunks_received >= RF_CHUNKS_PER_PACKET) {
				fw_chunks_received = 0; // Sonraki paket icin sifirla

				/* Assembly buffer yapisi:
				 * [0..15]   = IV (AES baslangic vektoru)
				 * [16..143] = AES-256-CBC sifrelenmis veri (128 byte)
				 * [144..147]= CRC-32 (sifrelenmis verinin CRC'si, little-endian) */
				uint8_t *iv_ptr = &fw_assembly_buf[0];
				uint8_t *encrypted_ptr = &fw_assembly_buf[16];
				uint32_t received_crc;
				memcpy(&received_crc, &fw_assembly_buf[144], 4); // Little-endian CRC oku

				/* CRC-32 dogrulama: sifrelenmis verinin bozulmadigi kontrol edilir */
				uint32_t computed_crc = Calculate_CRC32(encrypted_ptr, 128);
				if (computed_crc != received_crc) {
					/* RF bozulmasi: UPDATE_FAILED gondermek yerine NACK + sifirla.
					 * Gonderici aldiginda RF_SendChunkReliable basarisiz sayar,
					 * Python'a UART NACK gonderir. Python ayni paketi tekrar
					 * gonderince chunk 0 retry mantigi devreye girer. */
					fw_chunks_received = 0;
					memset(fw_assembly_buf, 0, sizeof(fw_assembly_buf));
					RF_SendPacket(RF_CMD_NACK, rf_seq_counter++,
							(uint8_t[]) { RF_ERR_CRC_FAIL }, 1);
					continue; /* Yeniden dene — donguden cikma */
				}

				/* AES-256-CBC sifre coz: IV + AES_KEY ile encrypted_ptr'yi yerinde coz */
				AES_init_ctx_iv(&aes_ctx, AES_KEY, iv_ptr);
				AES_CBC_decrypt_buffer(&aes_ctx, encrypted_ptr, 128);
				/* NOT: sifre cozme yerinde yapilir — encrypted_ptr artik duz metin */

				/* Ilk pakette MSP gecerliligi kontrol et (yanlis firmware yazma koruması) */
				if (current_addr == APP_ADDRESS) {
					uint32_t msp_val = *(uint32_t*) encrypted_ptr; // Uygulamanin ilk word'u MSP
					if ((msp_val & 0xFFF00000) != 0x20000000) {
						/* Gecersiz MSP → yanlis firmware veya yanlis hedef cihaz */
						RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
								(uint8_t[] ) { RF_ERR_INVALID_MSP }, 1);
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

				/* Yaz-oku dogrulama: yazilan veri dogru mu? */
				if (!Flash_Verify_Data(current_addr, encrypted_ptr, 128)) {
					RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
							(uint8_t[] ) { RF_ERR_FLASH_VERIFY }, 1);
					LED_Error();
					return;
				}

				current_addr += FW_PACKET_SIZE; // Sonraki yazma adresi (128 byte ilerle)
				packets_received++;             // Alinan paket sayacini artir

				/* Her tamamlanan Flash sayfasinda (16 paket = 2KB) resume durumunu kaydet.
				 * Bu sayede cihaz resetlenince kaldigi yerden devam edebilir.
				 * Ornek: packets_received=16 → sayfa 0 tamam → Resume_SavePageDone(0) */
				if (packets_received % PACKETS_PER_PAGE == 0) {
					uint32_t page_done = (packets_received / PACKETS_PER_PAGE)
							- 1;
					Resume_SavePageDone(page_done); // Bitmap'e 0x0000 yaz
				}

				memset(fw_assembly_buf, 0, sizeof(fw_assembly_buf)); // Tampon temizle
			}
		}
	}

	/* ===================================================================
	 * ADIM 7: FINAL CRC DOGRULAMA
	 * Tum Flash'a yazilan veriyi bastan CRC-32 hesapla.
	 * metadata.firmware_crc32 ile karsilastir.
	 * Uyusmazlik → guncelleme basarisiz.
	 * =================================================================== */
	NeoPixel_SetAll(0, 200, 200); // Acik mavi: CRC hesaplaniyor
	NeoPixel_Show();

	uint32_t flash_crc = Calculate_Flash_CRC32(APP_ADDRESS,
			metadata.firmware_size);

	if (flash_crc != metadata.firmware_crc32) {
		/* ── CRC UYUŞMAZLIĞI: Slot B'den eski firmware geri yükle ── */
		for (int i = 0; i < 5; i++) {
			NeoPixel_SetAll(255, 0, 128); /* Pembe: CRC hatası */
			NeoPixel_Show();
			HAL_Delay(150);
			NeoPixel_Clear();
			NeoPixel_Show();
			HAL_Delay(150);
			HAL_IWDG_Refresh(&hiwdg);
		}

		/* Gondericiye hata bildir */
		for (uint8_t i = 0U; i < 10U; i++) {
			RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter,
					(uint8_t[]) { RF_ERR_FW_CRC_MISMATCH }, 1);
			uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
			uint16_t rx_seq;
			uint8_t rx_pld_len;
			if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
				if (rx_type == RF_CMD_ACK) { break; }
			}
			HAL_IWDG_Refresh(&hiwdg);
		}
		rf_seq_counter++;

		/* Slot B'de geçerli yedek var mı? → ROLLBACK state yaz, reset */
		SlotMeta_t rb_meta;
		if (SlotMeta_Read(&rb_meta) && rb_meta.slot_b_crc32 != 0U) {
			rb_meta.state = SLOT_STATE_ROLLBACK;
			SlotMeta_Write(&rb_meta);
			/* main.c restart'ta Slot B → Slot A kopyalar */
		}

		LED_Error();
		secure_zero(AES_KEY, sizeof(AES_KEY));
		return;
	}

	/* ===================================================================
	 * ADIM 8: BAŞARILI TAMAMLAMA
	 *
	 * - SlotMeta güncelle: state=TRIAL, Slot A bilgileri, trial_count=0
	 * - Boot flag temizle (bir sonraki reset'te uygulama çalışacak)
	 * - UPDATE_COMPLETE gönder
	 * - Uygulamaya atla (trial boot başlar; app confirm_boot çağırmalı)
	 * =================================================================== */
	{
		SlotMeta_t ok_meta;
		if (!SlotMeta_Read(&ok_meta)) {
			/* Metadata okunamazsa sıfırdan oluştur */
			ok_meta.max_trials     = 3U;
			ok_meta.reserved       = 0U;
			ok_meta.confirm_flag   = 0xFFFFFFFFU;
			ok_meta.slot_b_version = 0U;
			ok_meta.slot_b_size    = SLOT_A_SIZE;
			ok_meta.slot_b_crc32   = 0U;
		}
		ok_meta.state          = SLOT_STATE_TRIAL;
		ok_meta.trial_count    = 0U;
		ok_meta.slot_a_version = metadata.firmware_version;
		ok_meta.slot_a_size    = metadata.firmware_size;
		ok_meta.slot_a_crc32   = flash_crc;
		ok_meta.confirm_flag   = 0xFFFFFFFFU; /* app onaylayana kadar bekler */
		SlotMeta_Write(&ok_meta);
	}

	/* Boot flag temizle: bir sonraki reset'te main.c uygulamaya atlar */
	clear_boot_flag();

	/* UPDATE_COMPLETE gonder — 10 kez dene, ACK gelince dur */
	for (uint8_t i = 0U; i < 10U; i++) {
		RF_SendPacket(RF_CMD_UPDATE_COMPLETE, rf_seq_counter, NULL, 0);
		uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
		uint16_t rx_seq;
		uint8_t rx_pld_len;
		if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
			if (rx_type == RF_CMD_ACK) { break; }
		}
		HAL_IWDG_Refresh(&hiwdg);
	}
	rf_seq_counter++;

	LED_Success();
	HAL_Delay(1000);

	secure_zero(AES_KEY, sizeof(AES_KEY));
	jump_to_application(); /* Trial boot başlıyor */
}
