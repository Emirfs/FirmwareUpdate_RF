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

/* Tani seviyeleri (SendDiag level parametresi icin) */
#define DIAG_LEVEL_ERROR  'E'
#define DIAG_LEVEL_WARN   'W'
#define DIAG_LEVEL_INFO   'I'

/* Tani/hata kodlari */
#define DIAG_ERR_SI4432_INIT   0x01U  /* Si4432 baslatilamadi */
#define DIAG_ERR_RF_TIMEOUT    0x02U  /* RF zaman asimi */
#define DIAG_ERR_UART_RX       0x03U  /* UART alma hatasi */
#define DIAG_WARN_BOOT_RETRY   0x11U  /* BOOT_ACK icin yeniden deneme */
#define DIAG_INFO_READY        0x21U  /* Gonderici hazir */
#define DIAG_INFO_FW_COMPLETE  0x22U  /* FW guncelleme tamamlandi */

/* Yapilandirilmis tani mesaji: [E:01] mesaj\r\n
 * Debug devre disi ise (FW update modunda) sessiz kalir. */
void SendDiag(char level, uint8_t code, const char *msg);

#endif /* SENDER_UART_DEBUG_H */
