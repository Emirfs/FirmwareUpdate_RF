#ifndef __SI4432_H__
#define __SI4432_H__

#include "main.h"
#include <stdint.h>

void SI4432_Init(void);
void SI4432_WriteReg(uint8_t reg, uint8_t value);
uint8_t SI4432_ReadReg(uint8_t reg);

void SI4432_SendPacket(const uint8_t *data, uint8_t len);
void SI4432_StartRx(void);
uint8_t SI4432_CheckRx(uint8_t *data);

#endif
