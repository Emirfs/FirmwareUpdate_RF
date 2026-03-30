/*
 * boot_flow.h — Bootloader Ana Akis Arayuzu
 *
 * jump_to_application : APP_ADDRESS'teki koda atla (MSP + SYSCFG remap)
 * Bootloader_Main     : RF uzerinden tam firmware guncelleme dongusu
 *
 * Cagrildigi yer: main.c — boot karari mantigi
 */
#ifndef BOOT_FLOW_H
#define BOOT_FLOW_H

#include <stdint.h>

/* Flash'taki uygulamaya atla (geri donmez — uygulama calismaya baslar) */
void jump_to_application(void);

/*
 * RF firmware guncelleme ana dongusu.
 *
 * pub_sender_hint : main.c 3s pencereden alinan BOOT_REQUEST'in pub_sender alani (32 byte).
 *                   NULL ise Bootloader_Main BOOT_REQUEST icin RF'i dinler.
 *                   NULL degil ise ECDH hemen gerceklestirilir (sender BOOT_ACK sonrasi
 *                   BOOT_REQUEST gondermez, bu yuzden hint gereklidir).
 */
void Bootloader_Main(const uint8_t *pub_sender_hint);

#endif /* BOOT_FLOW_H */
