/*
 * sender_uart_debug.c — UART Debug Ciktisi Yardimci Fonksiyonlari
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * UART1 uzerinden debug mesajlari gondermeyi saglar.
 * fw_debug_en bayragi ile tum ciktilar tek satirda devre disi birakilabilir.
 *
 * Neden devre disi birakma ozelligi var?
 *   Firmware update sirasinda UART, PC ile ikili protokol icin kullanilir
 *   (ACK/NACK byte'lari, firmware paketleri). Bu surede serbest metin
 *   gondermek protokolu bozar. Bu yuzden FirmwareUpdate_Mode() baslarken
 *   SenderDebug_SetEnabled(0) ile debug ciktisi kapatilir; bitince
 *   SenderDebug_SetEnabled(1) ile tekrar acilir.
 *
 * Fonksiyonlar:
 *   SenderDebug_SetEnabled — debug ciktisini ac/kapat
 *   Print    — null-terminated string gonder
 *   PrintHex — 1 byte'i "XX " formatinda gonder (ornk: "4F ")
 *   PrintBuf — ham byte dizisini oldugu gibi gonder
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - UART timeout'lari: HAL_UART_Transmit'deki 500 / 100 ms degerleri
 */

#include "sender_uart_debug.h"

#include "usart.h"
#include <string.h>

/* 1 = debug ciktisi aktif, 0 = tum Print/PrintHex/PrintBuf cagrılari sessiz */
static uint8_t fw_debug_en = 1;

/* Debug ciktisini etkinlestir (1) veya devre disi birak (0) */
void SenderDebug_SetEnabled(uint8_t enabled) { fw_debug_en = enabled; }

/* Null-terminated string'i UART'a gonder */
void Print(const char *s) {
  if (!fw_debug_en) {
    return; // FW update modunda ses yok
  }
  HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 500);
}

/* Bir byte'i "HH " formatinda (iki hex basamak + bosluk) gonder
 * Ornek: 0x4F → "4F " */
void PrintHex(uint8_t v) {
  if (!fw_debug_en) {
    return;
  }

  const char h[] = "0123456789ABCDEF";
  char o[3] = {h[(v >> 4) & 0xF], h[v & 0xF], ' '}; // Ust nibble, alt nibble, bosluk
  HAL_UART_Transmit(&huart1, (uint8_t *)o, 3, 100);
}

/* Ham byte dizisini oldugu gibi UART'a gonder (metin veya ikili veri icin) */
void PrintBuf(const uint8_t *b, uint16_t n) {
  if (!fw_debug_en) {
    return;
  }
  HAL_UART_Transmit(&huart1, (uint8_t *)b, n, 500);
}
