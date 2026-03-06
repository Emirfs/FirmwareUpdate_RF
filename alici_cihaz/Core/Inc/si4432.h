/*
 * si4432.h — Si4432 RF Modul Surucusu Arayuzu (Alici)
 *
 * Silicon Labs EZRadioPRO Si4432 — SPI uzerinden register erisimi.
 * 433 MHz, 9.6 kbps GFSK, CRC-IBM, maks 64 byte FIFO.
 *
 * Kullanim sirasi:
 *   1. SI4432_Init()            — sadece bir kez, baslangicta
 *   2. SI4432_SendPacket(d, n)  — TX
 *   3. SI4432_StartRx()         — RX moduna gec
 *   4. SI4432_CheckRx(buf) != 0 — RX polling ile paket kontrol
 *
 * NOT: Bu header hem alici hem gondericide kullanilir.
 *      Register degerlerinin gonderici tarafiyla ayni olmasi zorunludur.
 */
#ifndef __SI4432_H__
#define __SI4432_H__

#include "main.h"
#include <stdint.h>

/* Chip baslatma — hardware reset + software reset + tum register ayarlari */
void SI4432_Init(void);

/* SPI register erisimi */
void SI4432_WriteReg(uint8_t reg, uint8_t value);
uint8_t SI4432_ReadReg(uint8_t reg);

/* TX: paketi gonder, tamamlanana kadar bekle (bloke) */
void SI4432_SendPacket(const uint8_t *data, uint8_t len);

/* RX: alici moduna gec — SI4432_CheckRx ile polling yapilmali */
void SI4432_StartRx(void);

/* Paket alindiysa data'ya kopyala ve uzunlugu don.
 * Donus: 0 = paket yok, >0 = okunan uzunluk */
uint8_t SI4432_CheckRx(uint8_t *data);

#endif
