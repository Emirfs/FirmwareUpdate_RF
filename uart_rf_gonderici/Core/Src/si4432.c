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

#define CS_LOW()                                                               \
  HAL_GPIO_WritePin(SI4432_CE_GPIO_Port, SI4432_CE_Pin, GPIO_PIN_RESET)
#define CS_HIGH()                                                              \
  HAL_GPIO_WritePin(SI4432_CE_GPIO_Port, SI4432_CE_Pin, GPIO_PIN_SET)
#define NIRQ_IS_LOW()                                                          \
  (HAL_GPIO_ReadPin(SI4432_IRQ_GPIO_Port, SI4432_IRQ_Pin) == GPIO_PIN_RESET)

void SI4432_WriteReg(uint8_t reg, uint8_t value) {
  CS_LOW();
  uint8_t addr = reg | 0x80;
  HAL_SPI_Transmit(&hspi2, &addr, 1, 10);
  HAL_SPI_Transmit(&hspi2, &value, 1, 10);
  CS_HIGH();
}

uint8_t SI4432_ReadReg(uint8_t reg) {
  uint8_t val = 0;
  CS_LOW();
  uint8_t addr = reg & 0x7F;
  HAL_SPI_Transmit(&hspi2, &addr, 1, 10);
  HAL_SPI_Receive(&hspi2, &val, 1, 10);
  CS_HIGH();
  return val;
}

static void SI4432_ClearIRQ(void) {
  (void)SI4432_ReadReg(0x03);
  (void)SI4432_ReadReg(0x04);
}

static void SI4432_ResetRxFIFO(void) {
  SI4432_WriteReg(0x08, 0x02);
  SI4432_WriteReg(0x08, 0x00);
}

static void SI4432_ResetTxFIFO(void) {
  SI4432_WriteReg(0x08, 0x01);
  SI4432_WriteReg(0x08, 0x00);
}

void SI4432_Init(void) {
  CS_HIGH();
  HAL_GPIO_WritePin(SI4432_SDN_GPIO_Port, SI4432_SDN_Pin, GPIO_PIN_SET);
  HAL_Delay(10);
  HAL_GPIO_WritePin(SI4432_SDN_GPIO_Port, SI4432_SDN_Pin, GPIO_PIN_RESET);
  HAL_Delay(30);

  SI4432_ClearIRQ();
  SI4432_WriteReg(0x07, 0x80); // SW Reset

  uint32_t t = HAL_GetTick();
  while (!NIRQ_IS_LOW() && (HAL_GetTick() - t) < 500) {
  }
  SI4432_ClearIRQ();
  t = HAL_GetTick();
  while (!NIRQ_IS_LOW() && (HAL_GetTick() - t) < 500) {
  }
  SI4432_ClearIRQ();

  // Frekans: 433.0 MHz
  SI4432_WriteReg(0x75, 0x53);
  SI4432_WriteReg(0x76, 0x64);
  SI4432_WriteReg(0x77, 0x00);

  // TX Data Rate: 9.6 kbps
  SI4432_WriteReg(0x6E, 0x4E);
  SI4432_WriteReg(0x6F, 0xA5);
  SI4432_WriteReg(0x70, 0x2C);
  SI4432_WriteReg(0x72, 0x48);

  // Paket yapılandırması
  SI4432_WriteReg(0x34, 0x0A);
  SI4432_WriteReg(0x33, 0x02);
  SI4432_WriteReg(0x36, 0x2D);
  SI4432_WriteReg(0x37, 0xD4);
  SI4432_WriteReg(0x32, 0x00);
  SI4432_WriteReg(0x35, 0x28);
  SI4432_WriteReg(0x30, 0x0D);
  SI4432_WriteReg(0x71, 0x63);

  // RX Modem
  SI4432_WriteReg(0x1C, 0x05);
  SI4432_WriteReg(0x20, 0xA1);
  SI4432_WriteReg(0x21, 0x20);
  SI4432_WriteReg(0x22, 0x4E);
  SI4432_WriteReg(0x23, 0xA5);
  SI4432_WriteReg(0x24, 0x00);
  SI4432_WriteReg(0x25, 0x13);
  SI4432_WriteReg(0x1D, 0x40);
  SI4432_WriteReg(0x1F, 0x03);

  // Non-default registers
  SI4432_WriteReg(0x5A, 0x7F);
  SI4432_WriteReg(0x58, 0x80);
  SI4432_WriteReg(0x59, 0x40);
  SI4432_WriteReg(0x6A, 0x0B);
  SI4432_WriteReg(0x68, 0x04);
  SI4432_WriteReg(0x09, 0xD7);
  SI4432_WriteReg(0x6D, 0x07);

  SI4432_ResetTxFIFO();
  SI4432_ResetRxFIFO();
  SI4432_ClearIRQ();
}

void SI4432_SendPacket(const uint8_t *data, uint8_t len) {
  if (len == 0 || len > 64)
    return;
  SI4432_WriteReg(0x07, 0x01);
  SI4432_WriteReg(0x30, 0x0D);
  SI4432_ResetTxFIFO();
  SI4432_WriteReg(0x3E, len);
  for (uint8_t i = 0; i < len; i++)
    SI4432_WriteReg(0x7F, data[i]);
  SI4432_WriteReg(0x05, 0x04);
  SI4432_WriteReg(0x06, 0x00);
  SI4432_ClearIRQ();
  SI4432_WriteReg(0x07, 0x09);
  uint32_t start = HAL_GetTick();
  while (!NIRQ_IS_LOW() && (HAL_GetTick() - start) < 1000) {
  }
  SI4432_ClearIRQ();
  SI4432_WriteReg(0x07, 0x01);
}

void SI4432_StartRx(void) {
  SI4432_WriteReg(0x07, 0x01);
  SI4432_WriteReg(0x30, 0x85);
  SI4432_ResetRxFIFO();
  SI4432_WriteReg(0x05, 0x03);
  SI4432_WriteReg(0x06, 0x00);
  SI4432_ClearIRQ();
  SI4432_WriteReg(0x07, 0x05);
}

uint8_t SI4432_CheckRx(uint8_t *data) {
  if (!NIRQ_IS_LOW())
    return 0;
  SI4432_WriteReg(0x07, 0x01);
  uint8_t st1 = SI4432_ReadReg(0x03);
  (void)SI4432_ReadReg(0x04);

  if (st1 & 0x01) {
    SI4432_ResetRxFIFO();
    SI4432_StartRx();
    return 0;
  }
  if (st1 & 0x02) {
    uint8_t len = SI4432_ReadReg(0x4B);
    if (len > 0 && len <= 64) {
      for (uint8_t i = 0; i < len; i++)
        data[i] = SI4432_ReadReg(0x7F);
    } else {
      len = 0;
    }
    SI4432_ResetRxFIFO();
    SI4432_StartRx();
    return len;
  }
  SI4432_ResetRxFIFO();
  SI4432_StartRx();
  return 0;
}
