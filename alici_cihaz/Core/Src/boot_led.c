/*
 * boot_led.c — Bootloader NeoPixel LED Durum Gostergeleri
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * Bootloader surecinin her asamasini farkli LED rengi ile gorsellestirir.
 * Kullanici hangi asamada oldugunu LED renginden anlayabilir.
 *
 * Renk Anlami:
 *   TURUNCU (255, 80, 0)  → LED_Bootloader: Si4432 init bitti, BOOT_ACK bekleniyor
 *   KIRMIZI (255, 0, 0)   → LED_Error     : Hata (5 kez yanip soner)
 *   YESIL   (0, 255, 0)   → LED_Success   : Guncelleme basarili (3 kez yanip soner)
 *   MAVI    (0, 0, 200)   → LED_Transfer  : Paket transferi (cift paket sayisi)
 *   MOR     (128, 0, 200) → LED_Transfer  : Paket transferi (tek paket sayisi)
 *   KAPALI              → LED_Off        : LED'ler sonduruldu
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - LED renkleri: NeoPixel_SetAll(R, G, B) satirlarindaki degerler
 * - Yanip sonme hizi: HAL_Delay() sureleri (su an 150/200 ms)
 * - Yanip sonme tekrar sayisi: for dongu limitleri (su an 5 ve 3)
 */

#include "boot_led.h"

#include "iwdg.h"
#include "main.h"
#include "neopixel.h"

/* Turuncu — bootloader aktif, gonderici bekleniyor */
void LED_Bootloader(void) {
  NeoPixel_SetAll(255, 80, 0);
  NeoPixel_Show();
}

/* Kirmizi yanip sonme (5 kez) — hata durumu */
void LED_Error(void) {
  for (int i = 0; i < 5; i++) {
    NeoPixel_SetAll(255, 0, 0); // Kirmizi
    NeoPixel_Show();
    HAL_Delay(150);
    NeoPixel_Clear();           // Kapat
    NeoPixel_Show();
    HAL_Delay(150);
    HAL_IWDG_Refresh(&hiwdg);   // Uzun yanip sonme icin watchdog sifirla
  }
}

/* Yesil yanip sonme (3 kez) — basarili guncelleme */
void LED_Success(void) {
  for (int i = 0; i < 3; i++) {
    NeoPixel_SetAll(0, 255, 0); // Yesil
    NeoPixel_Show();
    HAL_Delay(200);
    NeoPixel_Clear();
    NeoPixel_Show();
    HAL_Delay(200);
    HAL_IWDG_Refresh(&hiwdg);
  }
}

/* Mavi/mor renk degistirme — firmware transfer ilerlemesi
 * packet_num cift: mavi, tek: mor → gorunur gecisin gostergesi */
void LED_Transfer(uint32_t packet_num) {
  if (packet_num % 2 == 0) {
    NeoPixel_SetAll(0, 0, 200);    // Mavi (cift paket)
  } else {
    NeoPixel_SetAll(128, 0, 200);  // Mor (tek paket)
  }
  NeoPixel_Show();
}

/* Tum LED'leri kapat */
void LED_Off(void) {
  NeoPixel_Clear();
  NeoPixel_Show();
}
