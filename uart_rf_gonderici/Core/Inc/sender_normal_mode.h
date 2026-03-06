/*
 * sender_normal_mode.h — Normal Mod UART Isleyici Arayuzu
 *
 * Ana dongude her alinan UART karakteri bu fonksiyona iletilir.
 * 'W'/'w' → firmware update moduna gec
 * Enter    → mevcut tamponu RF ile gonder, echo bekle
 * Diger    → tampona ekle, terminale echo
 */
#ifndef SENDER_NORMAL_MODE_H
#define SENDER_NORMAL_MODE_H

#include <stdint.h>

/* Ana donguden cagrilir; gelen UART karakterini isle.
 * Cagrildigi yer: main.c → while(1) dongusu */
void HandleNormalModeByte(uint8_t ch);

#endif /* SENDER_NORMAL_MODE_H */
