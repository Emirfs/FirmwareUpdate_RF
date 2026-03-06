/*
 * sender_state.c — Gonderici Global Degiskenleri
 *
 * ─── BU DOSYA NE YAPAR? ───────────────────────────────────────────────────
 * Gonderici sistemin tum modullerinden erisilen global degiskenleri tanimlar.
 * Her degisken extern olarak sender_state.h'da duyurulur; digер moduller
 * bu dosyayi dogrudan degistirmez, sadece header'i include eder.
 *
 * Degiskenler:
 *   uart_buf        — Normal modda klavyeden gelen karakterlerin tamponu
 *   uart_idx        — uart_buf'a yazilan karakter sayisi (imleç)
 *   fw_packet_buf   — PC'den gelen 148 byte'lik firmware paketinin tamponu
 *   rf_seq_counter  — RF paketlerinin sequence numarasi (her gonderimde artar)
 *
 * ─── DEGISTIRILEBİLECEK SEYLER ───────────────────────────────────────────
 * - MAX_MSG: Normal mod icin UART tampon boyutu (sender_state.h, su an 32)
 * - FW_FULL_PACKET_SIZE: Firmware paket boyutu (rf_protocol.h, su an 148)
 * - rf_seq_counter baslangic degeri (su an 0)
 */

#include "sender_state.h"

/* Normal modda klavyeden gelen karakterlerin tamponu (maks MAX_MSG karakter) */
uint8_t uart_buf[MAX_MSG];

/* uart_buf icindeki gecerli karakter sayisi (Enter basilinca sifirlanir) */
uint8_t uart_idx = 0;

/* PC'den UART uzerinden gelen tam firmware paketi:
 * [IV:16][AES_Encrypted:128][CRC32:4] = 148 byte */
uint8_t fw_packet_buf[FW_FULL_PACKET_SIZE];

/* RF paket sequence numarasi: her gondericiden sonra arttirilir.
 * Alici bu numara ile tekrar eden / kayip paketleri tespit edebilir. */
uint16_t rf_seq_counter = 0;
