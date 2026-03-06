/*
 * boot_rf.h — RF Haberlesme Katmani Arayuzu (Alici / Bootloader)
 *
 * RF_SendPacket    : Paketi gonder, ACK beklemez (tek seferlik)
 * RF_WaitForPacket : Belirli sure paket bekle (polling)
 * RF_SendReliable  : Gonder + ACK bekle; basarisizsa RF_MAX_RETRIES kez tekrar
 *
 * Cagrildigi yer: boot_flow.c — Bootloader_Main() ve main.c (3sn dinleme)
 */
#ifndef BOOT_RF_H
#define BOOT_RF_H

#include <stdint.h>

/* Paketi gonder, ACK bekleme. payload=NULL, len=0 ise sadece baslik gider. */
void RF_SendPacket(uint8_t type, uint16_t seq, const uint8_t *payload,
                   uint8_t payload_len);

/* timeout_ms sure icinde paket bekle.
 * Donus: 1=alindi (type, seq, payload dolu), 0=zaman asimi */
uint8_t RF_WaitForPacket(uint8_t *type, uint16_t *seq, uint8_t *payload,
                         uint8_t *payload_len, uint32_t timeout_ms);

/* Gonder + ACK bekle; RF_MAX_RETRIES kez dener.
 * Donus: 1=basarili, 0=basarisiz */
uint8_t RF_SendReliable(uint8_t type, uint16_t seq, const uint8_t *payload,
                        uint8_t payload_len);

#endif /* BOOT_RF_H */
