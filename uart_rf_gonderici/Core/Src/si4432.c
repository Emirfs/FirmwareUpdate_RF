/*
 * si4432.c — Silicon Labs Si4432 RF Modul Surucusu (Gonderici)
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * Si4432 EZRadioPRO RF modülünü SPI üzerinden kontrol eder.
 * Register degerlerinin tamamı Silicon Labs'ın resmi ornek kodlarından
 * alinmistir; tek degisiklik frekansın 915 MHz → 433 MHz yapilmasidir.
 *
 * Kaynak:
 *   EZRadioPRO_Sample_Codes/Si4432_revV2/TX_operation/main_sdbc_dk3.c
 *   EZRadioPRO_Sample_Codes/Si4432_revV2/RX_operation/main_SDBC_DK3.c
 *
 * RF Ayarlari (degistirme — alici ile ayni olmali):
 *   Frekans : 433.0 MHz
 *   Veri hizi: 9.6 kbps GFSK
 *   Sapma   : +/- 45 kHz
 *   Preamble: 5 byte (10 nibble)
 *   Sync    : 0x2D 0xD4
 *   CRC     : CRC-IBM (donanim tarafindan otomatik eklenir/kontrol edilir)
 *   Max paket: 64 byte (FIFO siniri)
 *
 * SPI Protokolu (Si4432):
 *   Yazma: [addr | 0x80][value]    — MSB=1 yaz
 *   Okuma: [addr & 0x7F] → [value] — MSB=0 oku
 *
 * nIRQ Pini:
 *   LOW → interrupt var (paket gönderildi, paket alindi, hata)
 *   HIGH → interrupt yok
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - Frekans: reg 0x75/0x76/0x77 (su an 433.0 MHz)
 * - Veri hizi: reg 0x6E/0x6F/0x70 (su an 9.6 kbps)
 * - TX gucu: reg 0x6D (0x07 = maks +20 dBm, 0x00 = min)
 * - Preamble uzunlugu: reg 0x34 (su an 10 nibble = 5 byte)
 */

#include "si4432.h"
#include "spi.h"
#include <string.h>

extern SPI_HandleTypeDef hspi2;

/* Chip Select (nSEL) kontrolu */
#define CS_LOW()  HAL_GPIO_WritePin(SI4432_CE_GPIO_Port, SI4432_CE_Pin, GPIO_PIN_RESET)
#define CS_HIGH() HAL_GPIO_WritePin(SI4432_CE_GPIO_Port, SI4432_CE_Pin, GPIO_PIN_SET)

/* nIRQ pin okuma — LOW ise interrupt var */
#define NIRQ_IS_LOW() \
  (HAL_GPIO_ReadPin(SI4432_IRQ_GPIO_Port, SI4432_IRQ_Pin) == GPIO_PIN_RESET)

/* SPI register yazma: [addr|0x80][value] */
void SI4432_WriteReg(uint8_t reg, uint8_t value) {
  CS_LOW();
  uint8_t addr = reg | 0x80; // MSB=1 → yazma islemi
  HAL_SPI_Transmit(&hspi2, &addr, 1, 10);
  HAL_SPI_Transmit(&hspi2, &value, 1, 10);
  CS_HIGH();
}

/* SPI register okuma: [addr&0x7F] → [value] */
uint8_t SI4432_ReadReg(uint8_t reg) {
  uint8_t val = 0;
  CS_LOW();
  uint8_t addr = reg & 0x7F; // MSB=0 → okuma islemi
  HAL_SPI_Transmit(&hspi2, &addr, 1, 10);
  HAL_SPI_Receive(&hspi2, &val, 1, 10);
  CS_HIGH();
  return val;
}

/* Interrupt flag'lerini temizle — nIRQ pinini serbest birakir */
static void SI4432_ClearIRQ(void) {
  (void)SI4432_ReadReg(0x03); // Interrupt Status 1 — okumak temizler
  (void)SI4432_ReadReg(0x04); // Interrupt Status 2
}

/* RX FIFO'yu sifirla (0x08 reg bit1) */
static void SI4432_ResetRxFIFO(void) {
  SI4432_WriteReg(0x08, 0x02); // FFCLRRX=1
  SI4432_WriteReg(0x08, 0x00); // FFCLRRX=0 (tek pulse yeterli)
}

/* TX FIFO'yu sifirla (0x08 reg bit0) */
static void SI4432_ResetTxFIFO(void) {
  SI4432_WriteReg(0x08, 0x01); // FFCLRTX=1
  SI4432_WriteReg(0x08, 0x00); // FFCLRTX=0
}

/*
 * SI4432_Init — Modulü baslat ve tum register'lari ayarla
 *
 * Siralama:
 *   1. SDN pin ile hardware reset (10ms HIGH, 30ms LOW)
 *   2. Software reset (0x07 = 0x80)
 *   3. POR interrupt bekle (nIRQ LOW)
 *   4. Frekans, veri hizi, paket yapilandirmasi, RX modem ayarla
 *   5. FIFO ve interrupt'lari temizle
 */
void SI4432_Init(void) {
  CS_HIGH(); // SPI bos

  /* Hardware reset: SDN=HIGH bekle SDN=LOW */
  HAL_GPIO_WritePin(SI4432_SDN_GPIO_Port, SI4432_SDN_Pin, GPIO_PIN_SET);
  HAL_Delay(10);
  HAL_GPIO_WritePin(SI4432_SDN_GPIO_Port, SI4432_SDN_Pin, GPIO_PIN_RESET);
  HAL_Delay(30);

  SI4432_ClearIRQ();
  SI4432_WriteReg(0x07, 0x80); // Software reset

  /* POR interrupt bekle */
  uint32_t t = HAL_GetTick();
  while (!NIRQ_IS_LOW() && (HAL_GetTick() - t) < 500) {}
  SI4432_ClearIRQ();

  /* Chip ready interrupt bekle */
  t = HAL_GetTick();
  while (!NIRQ_IS_LOW() && (HAL_GetTick() - t) < 500) {}
  SI4432_ClearIRQ();

  /* Frekans: 433.0 MHz
   * fb=0x53 (band 19, sbsel=1), fc=0x6400 */
  SI4432_WriteReg(0x75, 0x53); // Frequency Band Select
  SI4432_WriteReg(0x76, 0x64); // Nominal Carrier Frequency 1
  SI4432_WriteReg(0x77, 0x00); // Nominal Carrier Frequency 0

  /* TX Data Rate: 9.6 kbps (resmi ornekten) */
  SI4432_WriteReg(0x6E, 0x4E); // TX Data Rate 1
  SI4432_WriteReg(0x6F, 0xA5); // TX Data Rate 0
  SI4432_WriteReg(0x70, 0x2C); // Modulation Mode Control 1
  SI4432_WriteReg(0x72, 0x48); // Frequency Deviation: +/-45 kHz

  /* Paket yapilandirmasi (resmi ornekten) */
  SI4432_WriteReg(0x34, 0x10); // Preamble Length: 8 byte (16 nibble) — gurultu direnci icin uzatildi
  SI4432_WriteReg(0x33, 0x06); // Header Control 2: variable len, sync word 3,2&1 (3 byte)
  SI4432_WriteReg(0x36, 0x2D); // Sync Word 3
  SI4432_WriteReg(0x37, 0xD4); // Sync Word 2
  SI4432_WriteReg(0x38, 0xAA); // Sync Word 1 — 3. sync byte, false sync olasiligini 256x dusurur
  SI4432_WriteReg(0x32, 0x00); // Header Control 1: header filtresiz
  SI4432_WriteReg(0x35, 0x30); // Preamble Detection: 24 bit esik (preamble uzamasiyla uyumlu)
  SI4432_WriteReg(0x30, 0x0D); // Data Access Control (TX): CRC-IBM + paket isleme
  SI4432_WriteReg(0x71, 0x63); // Modulation Mode Control 2: FIFO + GFSK

  /* RX Modem: 9.6 kbps, 45 kHz sapma, 112.1 kHz kanal filtresi */
  SI4432_WriteReg(0x1C, 0x05); // IF Filter Bandwidth
  SI4432_WriteReg(0x20, 0xA1); // Clock Recovery Oversampling Ratio
  SI4432_WriteReg(0x21, 0x20); // Clock Recovery Offset 2
  SI4432_WriteReg(0x22, 0x4E); // Clock Recovery Offset 1
  SI4432_WriteReg(0x23, 0xA5); // Clock Recovery Offset 0
  SI4432_WriteReg(0x24, 0x00); // Clock Recovery Timing Loop Gain 1
  SI4432_WriteReg(0x25, 0x13); // Clock Recovery Timing Loop Gain 0
  SI4432_WriteReg(0x1D, 0x40); // AFC Loop Gearshift Override
  SI4432_WriteReg(0x1F, 0x03); // Clock Recovery Gearshift Override

  /* Si4432 non-default register'lari (resmi ornekten — degistirme!) */
  SI4432_WriteReg(0x5A, 0x7F); // VCO Current Trimming
  SI4432_WriteReg(0x58, 0x80); // Chargepump Current Trimming Override
  SI4432_WriteReg(0x59, 0x40); // Divider Current Trimming
  SI4432_WriteReg(0x6A, 0x0B); // AGC Override 2
  SI4432_WriteReg(0x68, 0x04); // Deltasigma ADC Tuning 2
  SI4432_WriteReg(0x09, 0xD7); // Crystal Oscillator Load Capacitance
  SI4432_WriteReg(0x6D, 0x07); // TX Power: maksimum (+20 dBm)

  /* Baslangicta FIFO ve interrupt'lari temizle */
  SI4432_ResetTxFIFO();
  SI4432_ResetRxFIFO();
  SI4432_ClearIRQ();
}

/*
 * SI4432_SendPacket — Paketi gonder, tamamlanana kadar bekle
 *
 * Siralama:
 *   1. Idle moda gec
 *   2. TX FIFO'ya uzunluk + veri yaz
 *   3. "Packet sent" interrupt'ini etkinlestir
 *   4. TX moduna gec (0x07 = 0x09)
 *   5. nIRQ LOW olana kadar bekle (paket gönderildi)
 *   6. Interrupt temizle, idle'a don
 *
 * Maks gonderim suresi: len×8 / 9600bps + baslik/kuyruk ≈ ~50 ms icin 53 byte
 */
void SI4432_SendPacket(const uint8_t *data, uint8_t len) {
  if (len == 0 || len > 64)
    return; // Gecersiz uzunluk — Si4432 FIFO 64 byte

  SI4432_WriteReg(0x07, 0x01); // Idle moda gec
  SI4432_WriteReg(0x30, 0x0D); // TX packet handler + CRC-IBM etkinlestir
  SI4432_ResetTxFIFO();        // Onceki veriyi temizle

  SI4432_WriteReg(0x3E, len);  // Paket uzunlugunu yaz (Transmit Packet Length reg)

  /* Veriyi TX FIFO'ya yaz (0x7F = FIFO Access reg) */
  for (uint8_t i = 0; i < len; i++)
    SI4432_WriteReg(0x7F, data[i]);

  SI4432_WriteReg(0x05, 0x04); // Interrupt Enable 1: sadece "packet sent" (ipksent)
  SI4432_WriteReg(0x06, 0x00); // Interrupt Enable 2: hepsi kapali
  SI4432_ClearIRQ();           // Onceki interrupt'lari temizle

  SI4432_WriteReg(0x07, 0x09); // TX + Ready moduna gec (gonderim baslıyor)

  /* nIRQ LOW olana kadar bekle — paket gönderildi sinyali */
  uint32_t start = HAL_GetTick();
  while (!NIRQ_IS_LOW() && (HAL_GetTick() - start) < 1000) {
    /* 1 sn timeout — donanimsal sorun olursa takilmaz */
  }

  SI4432_ClearIRQ();           // "Packet sent" interrupt'ini temizle
  SI4432_WriteReg(0x07, 0x01); // Idle'a don
}

/*
 * SI4432_StartRx — Alici moduna gec
 *
 * RX FIFO'yu temizler, "packet valid" ve "CRC error" interrupt'larini
 * etkinlestirir, RX moduna gecer.
 * Bu fonksiyon cagrilinca Si4432 gelen paketi bekler.
 */
void SI4432_StartRx(void) {
  SI4432_WriteReg(0x07, 0x01); // Once idle'a gec
  SI4432_WriteReg(0x30, 0x85); // RX packet handler + CRC etkinlestir
  SI4432_ResetRxFIFO();        // Eski veriyi temizle

  SI4432_WriteReg(0x05, 0x03); // Interrupt Enable 1: ipkvalid (bit1) + icrcerror (bit0)
  SI4432_WriteReg(0x06, 0x00); // Interrupt Enable 2: kapali
  SI4432_ClearIRQ();           // Onceki interrupt'lari temizle

  SI4432_WriteReg(0x07, 0x05); // RX + Ready moduna gec (alim basliyor)
}

/*
 * SI4432_CheckRx — Paket alindiysa oku, dondur
 *
 * nIRQ pinini polling ile kontrol eder. Interrupt yoksa 0 doner.
 * Interrupt varsa:
 *   - CRC hatasi (bit0)       → FIFO temizle, RX'e don, 0 dondur
 *   - Gecerli paket (bit1)    → FIFO'dan oku, RX'e don, uzunlugu dondur
 *   - Diger interrupt         → Temizle, RX'e don, 0 dondur
 *
 * Donus: 0 = paket yok, >0 = okunan paket uzunlugu
 */
uint8_t SI4432_CheckRx(uint8_t *data) {
  if (!NIRQ_IS_LOW())
    return 0; // nIRQ HIGH → interrupt yok, paket yok

  SI4432_WriteReg(0x07, 0x01); // Aliciyi durdur (idle)

  uint8_t st1 = SI4432_ReadReg(0x03); // Interrupt Status 1 oku
  (void)SI4432_ReadReg(0x04);         // Interrupt Status 2 oku (temizlemek icin)

  /* CRC hatasi? (bit0 = icrcerror) */
  if (st1 & 0x01) {
    SI4432_ResetRxFIFO(); // Bozuk veriyi at
    SI4432_StartRx();     // Tekrar dinlemeye gec
    return 0;
  }

  /* Gecerli paket? (bit1 = ipkvalid) */
  if (st1 & 0x02) {
    uint8_t len = SI4432_ReadReg(0x4B); // Received Packet Length reg

    if (len > 0 && len <= 64) {
      /* FIFO'dan byte byte oku (0x7F = FIFO Access reg) */
      for (uint8_t i = 0; i < len; i++)
        data[i] = SI4432_ReadReg(0x7F);
    } else {
      len = 0; // Gecersiz uzunluk — yoksay
    }

    SI4432_ResetRxFIFO();
    SI4432_StartRx(); // Sonraki paket icin hemen RX'e don
    return len;
  }

  /* Bilinmeyen interrupt — temizle ve devam et */
  SI4432_ResetRxFIFO();
  SI4432_StartRx();
  return 0;
}
