/*
 * sender_normal_mode.c — Normal Mod UART Isleyici
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * Ana dongude her gelen UART karakterini isler.
 *
 * Iki farkli davranis:
 *   'W' / 'w'       : Firmware update moduna gecis → FirmwareUpdate_Mode()
 *   Diger karakterler: Tamponа yaz, Enter gelince RF ile gonder ve echo bekle
 *
 * Normal mod akisi (Enter'a basilinca):
 *   1. Tamponu RF ile gonder (ham veri, protokol yok)
 *   2. 10 saniye echo bekle (alici geri gondermeli)
 *   3. Echo eslesmesi kontrol et → "BASARILI" veya "FARKLI" yaz
 *   4. Echo gelmezse "TIMEOUT" yaz
 *
 * Bu mod RF baglantisini test etmek icin kullanilir.
 * Gercek firmware guncellemesi icin 'W' gonderilmeli.
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - Echo bekleme suresi: while döngüsündeki 10000 ms
 * - Firmware update tetik karakteri: 'W' / 'w'
 */

#include "sender_normal_mode.h"

#include "iwdg.h"
#include "sender_fw_update.h"
#include "sender_state.h"
#include "sender_uart_debug.h"
#include "si4432.h"
#include "usart.h"
#include <string.h>

/*
 * HandleNormalModeByte — Ana donguden cagrilir, her UART karakterini isler
 *
 * 'W'/'w'   → Firmware update moduna gec (geri donunce normal moda devam)
 * '\r'/'\n' → Tamponu gonder, echo bekle
 * Diger     → Tampona ekle, echo karakteri UART'a yansit (terminal goruntuleme)
 */
void HandleNormalModeByte(uint8_t ch) {
  if (ch == 'W' || ch == 'w') {
    /* Firmware update moduna gec — bitince normal moda doner */
    FirmwareUpdate_Mode();
    uart_idx = 0;                   // Tamponu sifirla
    memset(uart_buf, 0, MAX_MSG);
    return;
  }

  if (ch == '\r' || ch == '\n') {
    /* Enter'a basildi — tampon doluysa gonder */
    if (uart_idx > 0) {
      HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET); // TX LED yak

      /* Gondermeden once UART debug ciktisi */
      Print("\r\n[TX] Veri: ");
      PrintBuf(uart_buf, uart_idx);
      Print("\r\n[TX] Hex : ");
      for (uint8_t i = 0; i < uart_idx; i++) {
        PrintHex(uart_buf[i]);
      }
      Print("\r\n[TX] Gonderiliyor...\r\n");

      /* Ham veriyi RF ile gonder (normal modda protokol yok) */
      SI4432_SendPacket(uart_buf, uart_idx);

      Print("[TX] Gonderildi! Echo bekleniyor...\r\n");
      HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);

      /* Gonderilen veriyi kaydet (echo karsilastirmasi icin) */
      uint8_t sent_len = uart_idx;
      uint8_t sent[MAX_MSG];
      memcpy(sent, uart_buf, uart_idx);

      /* Tamponu temizle — yeni mesaj icin hazir */
      uart_idx = 0;
      memset(uart_buf, 0, MAX_MSG);

      /* Alici moda gec ve echo bekle */
      SI4432_StartRx();
      HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET); // RX LED yak

      uint8_t got_echo = 0;
      uint32_t t0 = HAL_GetTick();

      while ((HAL_GetTick() - t0) < 10000) { // 10 saniye echo bekle
        HAL_IWDG_Refresh(&hiwdg);

        uint8_t rx[64];
        uint8_t rx_len = SI4432_CheckRx(rx); // Paket geldi mi kontrol et

        if (rx_len > 0) {
          got_echo = 1;

          /* Echo alindi — her iki LED 3 kez yanip sonsun */
          for (int b = 0; b < 3; b++) {
            HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
            HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
            HAL_Delay(100);
            HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
            HAL_Delay(100);
          }

          /* Alinan veriyi UART'a yaz */
          Print("\r\n[RX] Veri: ");
          PrintBuf(rx, rx_len);
          Print("\r\n[RX] Hex : ");
          for (uint8_t i = 0; i < rx_len; i++) {
            PrintHex(rx[i]);
          }
          Print("\r\n");

          /* Gonderilen ile alinan ayni mi? */
          if (rx_len == sent_len && memcmp(sent, rx, sent_len) == 0) {
            Print("[SONUC] BASARILI!\r\n"); // Birebir eslesme
          } else {
            Print("[SONUC] FARKLI!\r\n"); // Uzunluk veya icerik farki var
          }
          Print("---\r\n\r\n");
          break;
        }
      }

      HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET); // RX LED sondur
      if (!got_echo) {
        /* 10 saniye icerisinde echo gelmedi — LED0 5 kez yanip sonsun */
        for (int b = 0; b < 5; b++) {
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
          HAL_Delay(50);
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
          HAL_Delay(50);
        }
        Print("[RX] TIMEOUT!\r\n---\r\n\r\n");
      }
    }
  } else {
    /* Normal karakter — tampona ekle, terminale yanit */
    if (uart_idx < MAX_MSG) {
      uart_buf[uart_idx++] = ch;
      HAL_UART_Transmit(&huart1, &ch, 1, 10); // Karakteri terminale geri gonder (echo)
    }
    /* uart_idx >= MAX_MSG ise tampon dolu, karakter siliniyor */
  }
}
