/*
 * boot_led.h — NeoPixel LED Durum Gostergesi Arayuzu
 *
 * Bootloader surecinin her asamasini farkli renk ile gorsellestirir:
 *   LED_Bootloader : Turuncu — Si4432 hazir, gonderici bekleniyor
 *   LED_Error      : Kirmizi yanip sonme (5x) — hata
 *   LED_Success    : Yesil yanip sonme (3x) — basarili guncelleme
 *   LED_Transfer   : Mavi/mor degisim — paket transfer ilerlemesi
 *   LED_Off        : Tum LED'leri kapat
 */
#ifndef BOOT_LED_H
#define BOOT_LED_H

#include <stdint.h>

void LED_Bootloader(void);           // Turuncu — bootloader bekleme
void LED_Error(void);                // Kirmizi 5x — hata
void LED_Success(void);              // Yesil 3x — basarili
void LED_Transfer(uint32_t packet_num); // Mavi/mor — transfer
void LED_Off(void);                  // LED kapat

#endif /* BOOT_LED_H */
