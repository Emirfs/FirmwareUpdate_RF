/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Alıcı Cihaz — RF Bootloader + Uygulama Yükleyici
 ******************************************************************************
 * STM32F030CC (256KB Flash, 32KB RAM)
 *
 * Flash Düzeni:
 *   0x08000000 – 0x08003FFF : Bootloader (16KB, 8 sayfa)
 *   0x08004000 – 0x0803F7FF : Uygulama   (238KB, 119 sayfa)
 *   0x0803F800 – 0x0803FFFF : Boot Flag  (2KB, 1 sayfa)
 *
 * Boot Kararı:
 *   - Boot flag sayfasında BOOT_FLAG_MAGIC + BOOT_FLAG_REQUEST varsa
 *     → Bootloader moduna gir (RF üzerinden firmware güncelleme)
 *   - Yoksa → Uygulamaya atla
 *   - Geçerli uygulama yoksa (MSP kontrolü) → Bootloader'da kal
 *
 * Güncelleme Akışı:
 *   1. CMD_BOOT_ACK gönder (gönderici hazır olana kadar)
 *   2. CMD_METADATA al → firmware boyutu, versiyon, CRC
 *   3. Flash sil → CMD_FLASH_ERASE_DONE gönder
 *   4. DATA_CHUNK paketlerini al (3 parça × ~50 byte = 148 byte)
 *   5. Her 148 byte → CRC-32 doğrula → AES-256 decrypt → Flash'a yaz
 *   6. Final CRC doğrulama → CMD_UPDATE_COMPLETE veya CMD_UPDATE_FAILED
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"
#include "iwdg.h"
#include "rtc.h"
#include "spi.h"
#include "tim.h"

/* USER CODE BEGIN Includes */
#include "boot_flow.h"
#include "boot_led.h"
#include "boot_rf.h"
#include "boot_storage.h"
#include "neopixel.h"
#include "rf_bootloader.h"
#include "si4432.h"
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */
/* USER CODE END 0 */

int main(void) {
	/* HAL ve sistem saatini baslat */
	HAL_Init();
	SystemClock_Config(); // HSI x6 PLL = 24 MHz (alici daha dusuk hizda)

	/* Cevresel birim baslatma */
	MX_GPIO_Init();        // GPIO — Si4432 CS/SDN/IRQ, NeoPixel vb.
	MX_SPI2_Init();        // Si4432 SPI baglantisi
	MX_IWDG_Init();        // Watchdog — sonsuz dongu koruması
	MX_TIM17_Init();       // NeoPixel zamanlayicisi (tim.c)
	MX_TIM6_Init();        // NeoPixel IC zamanlayicisi
	/* TIM16, TIM3 — rezerve, bootloader'da kullanilmiyor, init atlanıyor */
	/* RTC — bootloader'da kullanilmiyor; LSI zaten SystemClock_Config'de aktif */

	/* NeoPixel LED baslatma */
	NeoPixel_Init();

	/* Beyaz kisa flas — MCU ayaga kalktı, diger kontroller baslamadan */
	NeoPixel_SetAll(100, 100, 100); // Beyaz
	NeoPixel_Show();
	HAL_Delay(300);
	NeoPixel_Clear();
	NeoPixel_Show();

	/* ===================================================================
	 * BOOT KARARI — Dual-Slot OTA
	 *
	 * Öncelik sırası:
	 *   0. SlotMeta oku → ROLLBACK veya trial timeout tespit et
	 *   1. ROLLBACK state → Slot B → Slot A geri yükle → uygulamaya atla
	 *   2. Boot flag varsa → doğrudan Bootloader_Main (RF güncelleme)
	 *   3. Geçerli uygulama (Slot A MSP OK):
	 *      a. TRIAL veya BACKED_UP state → trial_count++
	 *         trial_count >= max_trials → ROLLBACK state → reset
	 *      b. 3 sn RF dinle → BOOT_REQUEST gelirse → Bootloader_Main
	 *      c. Gelmezse → uygulamaya atla
	 *   4. Geçerli uygulama yok:
	 *      - BACKED_UP state + Slot A geçersiz → ROLLBACK state → reset
	 *      - Yoksa → Bootloader_Main (ilk programlama)
	 * =================================================================== */

	/* ── ADIM 0: SlotMeta oku ────────────────────────────────────────── */
	SlotMeta_t s_meta;
	uint8_t    s_meta_valid = SlotMeta_Read(&s_meta);

	/* ── ADIM 1: ROLLBACK state → Slot B'den geri yükle ─────────────── */
	if (s_meta_valid && s_meta.state == SLOT_STATE_ROLLBACK) {
		NeoPixel_SetAll(255, 40, 0); /* Kırmızı-turuncu: rollback */
		NeoPixel_Show();

		if (SlotA_RestoreFromB()) {
			/* Geri yükleme başarılı — state güncelle */
			s_meta.state       = SLOT_STATE_NORMAL;
			s_meta.trial_count = 0U;
			s_meta.confirm_flag = 0xFFFFFFFFU;
			SlotMeta_Write(&s_meta);
		}
		/* Başarısız olsa bile devam: Slot A ne ise oradan boot dene */
	}

	/* ── ADIM 2: Boot flag → doğrudan bootloader ────────────────────── */
	if (check_boot_flag()) {
		Bootloader_Main(NULL);
		NVIC_SystemReset();
	}

	/* ── ADIM 3: Slot A geçerlilik kontrolü ─────────────────────────── */
	uint32_t app_msp = *(volatile uint32_t*) APP_ADDRESS;
	if ((app_msp & 0xFFF00000U) == 0x20000000U) {

		/* ── Trial boot sayacı ──────────────────────────────────────── */
		if (s_meta_valid
				&& (s_meta.state == SLOT_STATE_TRIAL
						|| s_meta.state == SLOT_STATE_BACKED_UP)) {

			/* confirm_flag = 0 ise app zaten onaylamış → state'i ilerlet */
			if (s_meta.confirm_flag == 0x00000000U) {
				s_meta.state       = SLOT_STATE_CONFIRMED;
				s_meta.trial_count = 0U;
				SlotMeta_Write(&s_meta);
			} else {
				s_meta.trial_count++;
				if (s_meta.trial_count >= s_meta.max_trials) {
					/* Trial timeout: Slot B'de yedek var mı? */
					if (s_meta.slot_b_crc32 != 0U) {
						s_meta.state = SLOT_STATE_ROLLBACK;
						SlotMeta_Write(&s_meta);
						NVIC_SystemReset(); /* Rollback için yeniden başla */
					} else {
						/* Yedek yok — kurtarma yapılamaz, bootloader moduna gir */
						s_meta.trial_count = 0U;
						SlotMeta_Write(&s_meta);
						Bootloader_Main(NULL);
						NVIC_SystemReset();
					}
				}
				SlotMeta_Write(&s_meta); /* Artan sayacı kaydet */
			}
		}

		/* ── 3s RF penceresi: BOOT_REQUEST gelirse bootloader'a geç ── */
		HAL_NVIC_DisableIRQ(EXTI4_15_IRQn);
		SI4432_Init();
		HAL_Delay(10);

		if (SI4432_ReadReg(0x00U) == 0x08U) {
			NeoPixel_SetAll(0, 0, 100); /* Mavi: RF dinliyor */
			NeoPixel_Show();

			uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
			uint16_t rx_seq;
			uint8_t rx_pld_len;

			if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
				if (rx_type == RF_CMD_BOOT_REQUEST) {
					NeoPixel_SetAll(255, 128, 0); /* Turuncu: bootloader'a geçiş */
					NeoPixel_Show();
					set_boot_flag();
					const uint8_t *pub_hint =
							(rx_pld_len >= ECDH_KEY_SIZE) ? rx_pld : NULL;
					Bootloader_Main(pub_hint);
					NVIC_SystemReset();
				}
			}
		}

		/* BOOT_REQUEST gelmedi → uygulamaya atla */
		LED_Off();
		jump_to_application();
	}

	/* ── ADIM 4: Slot A geçersiz ─────────────────────────────────────── */
	if (s_meta_valid && s_meta.state == SLOT_STATE_BACKED_UP
			&& s_meta.slot_b_crc32 != 0U) {
		/* Yedekleme sırasında güç kesilmiş, Slot A bozulmuş → rollback */
		s_meta.state = SLOT_STATE_ROLLBACK;
		SlotMeta_Write(&s_meta);
		NVIC_SystemReset();
	}

	/* Slot A geçersiz + yedek yok → ilk programlama modu */
	Bootloader_Main(NULL);
	NVIC_SystemReset();

	while (1) { HAL_IWDG_Refresh(&hiwdg); }
}

void SystemClock_Config(void) {
	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };
	RCC_PeriphCLKInitTypeDef PeriphClkInit = { 0 };

	RCC_OscInitStruct.OscillatorType =
	RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
	RCC_OscInitStruct.HSIState = RCC_HSI_ON;
	RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	RCC_OscInitStruct.LSIState = RCC_LSI_ON;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
	RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL6;
	RCC_OscInitStruct.PLL.PREDIV = RCC_PREDIV_DIV1;
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
		Error_Handler();

	RCC_ClkInitStruct.ClockType =
	RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
		Error_Handler();

	PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC;
	PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
	if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
		Error_Handler();
}

void Error_Handler(void) {
	__disable_irq();
	while (1) {
	}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
