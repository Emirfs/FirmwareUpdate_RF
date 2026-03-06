/*
 * sender_uart_debug.h — UART Debug Ciktisi Arayuzu
 *
 * Firmware update sirasinda debug ciktisi devre disi birakilir
 * (UART ikili protokol icin kullanilir), normal modda tekrar acilir.
 *
 * SenderDebug_SetEnabled(0) → tum Print/PrintHex/PrintBuf cagrılari sessiz
 * SenderDebug_SetEnabled(1) → ciktilar UART'a gonderilir
 */
#ifndef SENDER_UART_DEBUG_H
#define SENDER_UART_DEBUG_H

#include <stdint.h>

/* Debug ciktisini etkinlestir (1) veya kapat (0) */
void SenderDebug_SetEnabled(uint8_t enabled);

/* Null-terminated string gonder: Print("[TX] Gonderiliyor...\r\n") */
void Print(const char *s);

/* Bir byte'i "HH " formatinda gonder: PrintHex(0x4F) → "4F " */
void PrintHex(uint8_t v);

/* Ham byte dizisini oldugu gibi gonder: metin veya ikili veri */
void PrintBuf(const uint8_t *b, uint16_t n);

#endif /* SENDER_UART_DEBUG_H */
