/*
 * sender_rf_link.h — RF Haberlesme Katmani Arayuzu (Gonderici)
 *
 * RF_SendPacket        : Paketi gonder, ACK bekleme (tek seferlik)
 * RF_WaitForPacket     : Belirli süre paket bekle
 * RF_SendChunkReliable : Gonder + ACK bekle, basarisizsa RF_MAX_RETRIES kez tekrar
 */
#ifndef SENDER_RF_LINK_H
#define SENDER_RF_LINK_H

#include <stdint.h>

/* Paketi gonder — ACK beklemez, aninda doner */
void RF_SendPacket(uint8_t type, uint16_t seq, const uint8_t *payload,
                   uint8_t payload_len);

/* timeout_ms sure icinde paket bekle.
 * Donus: 1=alindi, 0=zaman asimi */
uint8_t RF_WaitForPacket(uint8_t *type, uint16_t *seq, uint8_t *payload,
                         uint8_t *payload_len, uint32_t timeout_ms);

/* Gonder + ACK bekle; ACK gelmezse RF_MAX_RETRIES kez tekrar dene.
 * Donus: 1=basarili, 0=basarisiz */
uint8_t RF_SendChunkReliable(uint8_t type, uint16_t seq, const uint8_t *payload,
                             uint8_t payload_len);

#endif /* SENDER_RF_LINK_H */
