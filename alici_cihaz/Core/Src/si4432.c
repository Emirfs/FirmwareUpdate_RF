/*
 * Si4432 Driver — Silicon Labs EZRadioPRO Resmi Örnek Kodlardan
 *
 * Kaynak: EZRadioPRO_Sample_Codes/Si4432_revV2/TX_operation/main_sdbc_dk3.c
 *         EZRadioPRO_Sample_Codes/Si4432_revV2/RX_operation/main_SDBC_DK3.c
 *
 * Register değerleri birebir Silicon Labs'ın resmi kodlarından alınmıştır.
 * Tek fark: frekans 915 MHz yerine 433 MHz olarak ayarlanmıştır.
 */

#include "si4432.h"
#include "spi.h"
#include <string.h>

extern SPI_HandleTypeDef hspi2;

// =========================================================================
// Chip Select (nSEL)
// =========================================================================
#define CS_LOW()                                                               \
  HAL_GPIO_WritePin(SI4432_CE_GPIO_Port, SI4432_CE_Pin, GPIO_PIN_RESET)
#define CS_HIGH()                                                              \
  HAL_GPIO_WritePin(SI4432_CE_GPIO_Port, SI4432_CE_Pin, GPIO_PIN_SET)

// =========================================================================
// nIRQ Pin Okuma
// =========================================================================
#define NIRQ_IS_LOW()                                                          \
  (HAL_GPIO_ReadPin(SI4432_IRQ_GPIO_Port, SI4432_IRQ_Pin) == GPIO_PIN_RESET)

// =========================================================================
// SPI Register Okuma/Yazma
// =========================================================================
void SI4432_WriteReg(uint8_t reg, uint8_t value) {
	CS_LOW();
	uint8_t addr = reg | 0x80; // MSB=1 → write
	HAL_SPI_Transmit(&hspi2, &addr, 1, 10);
	HAL_SPI_Transmit(&hspi2, &value, 1, 10);
	CS_HIGH();
}

uint8_t SI4432_ReadReg(uint8_t reg) {
	uint8_t val = 0;
	CS_LOW();
	uint8_t addr = reg & 0x7F; // MSB=0 → read
	HAL_SPI_Transmit(&hspi2, &addr, 1, 10);
	HAL_SPI_Receive(&hspi2, &val, 1, 10);
	CS_HIGH();
	return val;
}

// =========================================================================
// Interrupt flag'lerini temizle (nIRQ pin'i serbest bırakır)
// =========================================================================
static void SI4432_ClearIRQ(void) {
	(void) SI4432_ReadReg(0x03); // Interrupt Status 1
	(void) SI4432_ReadReg(0x04); // Interrupt Status 2
}

// =========================================================================
// FIFO Reset
// =========================================================================
static void SI4432_ResetRxFIFO(void) {
	SI4432_WriteReg(0x08, 0x02);
	SI4432_WriteReg(0x08, 0x00);
}

static void SI4432_ResetTxFIFO(void) {
	SI4432_WriteReg(0x08, 0x01);
	SI4432_WriteReg(0x08, 0x00);
}

// =========================================================================
// Si4432 Başlatma — Silicon Labs Resmi Örnek Koddan
// =========================================================================
void SI4432_Init(void) {
	CS_HIGH();

	// SDN pin ile hardware reset
	HAL_GPIO_WritePin(SI4432_SDN_GPIO_Port, SI4432_SDN_Pin, GPIO_PIN_SET);
	HAL_Delay(10);
	HAL_GPIO_WritePin(SI4432_SDN_GPIO_Port, SI4432_SDN_Pin, GPIO_PIN_RESET);
	HAL_Delay(30);

	// Interrupt flag'lerini temizle
	SI4432_ClearIRQ();

	// Software Reset (resmi örnekteki gibi)
	SI4432_WriteReg(0x07, 0x80);

	// nIRQ LOW olana kadar bekle — POR interrupt
	uint32_t t = HAL_GetTick();
	while (!NIRQ_IS_LOW() && (HAL_GetTick() - t) < 500) {
	}
	SI4432_ClearIRQ();

	// Chip ready interrupt bekle
	t = HAL_GetTick();
	while (!NIRQ_IS_LOW() && (HAL_GetTick() - t) < 500) {
	}
	SI4432_ClearIRQ();

	// ==========================================
	// FREKANS: 433.0 MHz
	// ==========================================
	// Hesaplama: fb=0x53 (band 19 + sbsel=1), fc_high=0x64, fc_low=0x00
	SI4432_WriteReg(0x75, 0x53); // Frequency Band Select
	SI4432_WriteReg(0x76, 0x64); // Nominal Carrier Frequency 1
	SI4432_WriteReg(0x77, 0x00); // Nominal Carrier Frequency 0

	// ==========================================
	// TX DATA RATE: 9.6 kbps (resmi örnekten)
	// ==========================================
	SI4432_WriteReg(0x6E, 0x4E); // TX Data Rate 1
	SI4432_WriteReg(0x6F, 0xA5); // TX Data Rate 0
	SI4432_WriteReg(0x70, 0x2C); // Modulation Mode Control 1

	// ==========================================
	// TX DEVIATION: ±45 kHz (resmi örnekten)
	// ==========================================
	SI4432_WriteReg(0x72, 0x48); // Frequency Deviation (TX'de 0x48)

	// ==========================================
	// PAKET YAPILANDIRMASI (resmi örnekten)
	// ==========================================
	SI4432_WriteReg(0x34, 0x10); // Preamble Length = 8 bytes (16 nibble) — gurultu direnci icin uzatildi
	SI4432_WriteReg(0x33, 0x06); // Header Control 2: variable len, sync word 3,2&1 (3 byte)
	SI4432_WriteReg(0x36, 0x2D); // Sync Word 3
	SI4432_WriteReg(0x37, 0xD4); // Sync Word 2
	SI4432_WriteReg(0x38, 0xAA); // Sync Word 1 — 3. sync byte, false sync olasiligini 256x dusurur
	SI4432_WriteReg(0x32, 0x00); // Header Control 1: disable header filters
	SI4432_WriteReg(0x35, 0x30); // Preamble Detection: 24 bits threshold (preamble uzamasiyla uyumlu)

	// TX: packet handler + CRC-IBM
	SI4432_WriteReg(0x30, 0x0D); // Data Access Control (TX)
	// FIFO mode + GFSK modulation (resmi örnekten — 0x63, 0x23 DEĞİL!)
	SI4432_WriteReg(0x71, 0x63); // Modulation Mode Control 2

	// ==========================================
	// RX MODEM AYARLARI (resmi RX örneğinden)
	// 9.6 kbps, 45 kHz deviation, 112.1 kHz kanal filtre BW
	// ==========================================
	SI4432_WriteReg(0x1C, 0x05); // IF Filter Bandwidth
	SI4432_WriteReg(0x20, 0xA1); // Clock Recovery Oversampling Ratio
	SI4432_WriteReg(0x21, 0x20); // Clock Recovery Offset 2
	SI4432_WriteReg(0x22, 0x4E); // Clock Recovery Offset 1
	SI4432_WriteReg(0x23, 0xA5); // Clock Recovery Offset 0
	SI4432_WriteReg(0x24, 0x00); // Clock Recovery Timing Loop Gain 1
	SI4432_WriteReg(0x25, 0x13); // Clock Recovery Timing Loop Gain 0
	SI4432_WriteReg(0x1D, 0x40); // AFC Loop Gearshift Override
	SI4432_WriteReg(0x1F, 0x03); // Clock Recovery Gearshift Override

	// ==========================================
	// Si4432 NON-DEFAULT REGISTER'LAR (resmi örnekten)
	// ==========================================
	SI4432_WriteReg(0x5A, 0x7F); // VCO Current Trimming
	SI4432_WriteReg(0x58, 0x80); // Chargepump Current Trimming Override
	SI4432_WriteReg(0x59, 0x40); // Divider Current Trimming
	SI4432_WriteReg(0x6A, 0x0B); // AGC Override 2
	SI4432_WriteReg(0x68, 0x04); // Deltasigma ADC Tuning 2
	SI4432_WriteReg(0x09, 0xD7); // Crystal Oscillator Load Capacitance

	// ==========================================
	// TX GÜCÜ: maksimum (+20 dBm)
	// ==========================================
	SI4432_WriteReg(0x6D, 0x07); // TX Power

	// FIFO temizle
	SI4432_ResetTxFIFO();
	SI4432_ResetRxFIFO();
	SI4432_ClearIRQ();
}

// =========================================================================
// Paket Gönder — Resmi TX örneğinden birebir
// =========================================================================
void SI4432_SendPacket(const uint8_t *data, uint8_t len) {
	if (len == 0 || len > 64)
		return;

	// Idle moduna geç
	SI4432_WriteReg(0x07, 0x01);

	// TX packet handler + CRC-IBM etkinleştir
	SI4432_WriteReg(0x30, 0x0D);

	// TX FIFO temizle
	SI4432_ResetTxFIFO();

	// Paket uzunluğunu yaz
	SI4432_WriteReg(0x3E, len);

	// Veriyi FIFO'ya yaz (tek tek — resmi örnekteki gibi)
	for (uint8_t i = 0; i < len; i++) {
		SI4432_WriteReg(0x7F, data[i]);
	}

	// Sadece "packet sent" interrupt'ını etkinleştir
	SI4432_WriteReg(0x05, 0x04); // ipksent etkin
	SI4432_WriteReg(0x06, 0x00); // diğerleri kapalı

	// Interrupt flag'lerini temizle
	SI4432_ClearIRQ();

	// TX moduna geç: 0x09 = TX + Ready (resmi örnekteki gibi)
	SI4432_WriteReg(0x07, 0x09);

	// nIRQ LOW olana kadar bekle — paket gönderildi
	uint32_t start = HAL_GetTick();
	while (!NIRQ_IS_LOW() && (HAL_GetTick() - start) < 1000) {
		// nIRQ hala HIGH → paket henüz gönderilmedi
	}

	// Interrupt flag'lerini oku ve temizle
	SI4432_ClearIRQ();

	// Idle'a dön
	SI4432_WriteReg(0x07, 0x01);
}

// =========================================================================
// RX Moduna Geç — Resmi RX örneğinden birebir
// =========================================================================
void SI4432_StartRx(void) {
	// Idle moduna geç
	SI4432_WriteReg(0x07, 0x01);

	// RX packet handler + CRC-IBM etkinleştir
	SI4432_WriteReg(0x30, 0x85);

	// RX FIFO temizle
	SI4432_ResetRxFIFO();

	// RX interrupt'ları etkinleştir: ipkvalid + icrcerror
	SI4432_WriteReg(0x05, 0x03);
	SI4432_WriteReg(0x06, 0x00);

	// Interrupt flag'lerini temizle
	SI4432_ClearIRQ();

	// RX moduna geç: 0x05 = RX + Ready (resmi örnekteki gibi)
	SI4432_WriteReg(0x07, 0x05);
}

// =========================================================================
// RX Kontrol — nIRQ pinini kontrol et, paket varsa oku
// Dönüş: 0 = paket yok, >0 = alınan paket uzunluğu
// =========================================================================
uint8_t SI4432_CheckRx(uint8_t *data) {
	// nIRQ HIGH ise → interrupt yok, paket yok
	if (!NIRQ_IS_LOW())
		return 0;

	// Alıcıyı kapat (resmi örnekteki gibi)
	SI4432_WriteReg(0x07, 0x01);

	// Interrupt durumunu oku
	uint8_t st1 = SI4432_ReadReg(0x03);
	(void) SI4432_ReadReg(0x04);

	// CRC hatası?
	if (st1 & 0x01) {
		SI4432_ResetRxFIFO();
		// RX'i tekrar başlat
		SI4432_StartRx();
		return 0;
	}

	// Geçerli paket alındı?
	if (st1 & 0x02) {
		uint8_t len = SI4432_ReadReg(0x4B); // Received Packet Length

		if (len > 0 && len <= 64) {
			// FIFO'dan oku (tek tek — resmi örnekteki gibi)
			for (uint8_t i = 0; i < len; i++) {
				data[i] = SI4432_ReadReg(0x7F);
			}
		} else {
			len = 0;
		}

		// RX FIFO temizle
		SI4432_ResetRxFIFO();
		// RX'i tekrar başlat
		SI4432_StartRx();
		return len;
	}

	// Bilinmeyen interrupt — temizle ve devam et
	SI4432_ResetRxFIFO();
	SI4432_StartRx();
	return 0;
}
