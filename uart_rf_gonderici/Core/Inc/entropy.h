/*
 * entropy.h — Donanim entropi kaynagi (UID + ADC gurultusu + SysTick)
 *
 * STM32F030 icin RNG yok. Bu modül:
 *   1. 96-bit UID   → cihaza ozel benzersizlik (uretimde yakili)
 *   2. ADC gurultu  → her sessionda farkli (ic sicaklik sensoru, analog gurultu)
 *   3. SysTick      → zamanlama belirsizligi
 *   4. FNV-1a hash  → kucuk farklar tum havuzu etkiler (avalanche)
 *
 * Kullanim:
 *   uint8_t key[32];
 *   entropy_generate(key, 32);
 *   c25519_prepare(key);  // ECDH private key hazir
 */

#ifndef ENTROPY_H_
#define ENTROPY_H_

#include <stdint.h>

/*
 * entropy_generate — len byte entropi uret, buf'a yaz
 *
 * buf : cikis tamponu
 * len : istenilen byte sayisi (tipik: 32)
 *
 * NOT: Fonksiyon ADC'yi geçici olarak baslatiр, okuma sonrasi durdurur.
 *      Diger ADC kullanicilariyla cakisma olmamasi icin bootloader basinda cagir.
 */
void entropy_generate(uint8_t *buf, uint16_t len);

#endif /* ENTROPY_H_ */
