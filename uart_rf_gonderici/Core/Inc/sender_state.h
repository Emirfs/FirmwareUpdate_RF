/*
 * sender_state.h — Gonderici Global Degisken Bildirimleri
 *
 * sender_state.c'de tanimlanan global degiskenleri duyurur.
 * Bu header'i include eden her modul bu degiskenlere erisebilir.
 *
 * uart_buf       : Normal modda klavye girisi tamponu (MAX_MSG byte)
 * uart_idx       : uart_buf'taki gecerli karakter sayisi
 * fw_packet_buf  : PC'den gelen 148 byte firmware paketi (IV+Encrypted+CRC)
 * rf_seq_counter : RF paket sequence numarasi (her TX'te arttirilir)
 */
#ifndef SENDER_STATE_H
#define SENDER_STATE_H

#include <stdint.h>
#include "rf_protocol.h"

/* Normal mod UART tampon boyutu (klavye girisi icin) */
#define MAX_MSG 32

extern uint8_t uart_buf[MAX_MSG];        // Klavye girisi tamponu
extern uint8_t uart_idx;                 // Tampondaki karakter sayisi
extern uint8_t fw_packet_buf[FW_FULL_PACKET_SIZE]; // 148 byte FW paket tamponu
extern uint16_t rf_seq_counter;          // RF sequence numarasi (0'dan baslar)

#endif /* SENDER_STATE_H */
