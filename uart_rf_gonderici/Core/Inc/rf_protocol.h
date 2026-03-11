#ifndef RF_PROTOCOL_H
#define RF_PROTOCOL_H

#include <stdint.h>

// =========================================================================
// RF Firmware Update Protokolü — Ortak Tanımlar
// =========================================================================

// RF paket boyut sınırları
#define RF_MAX_PACKET 53 // Si4432 FIFO sınırı dahilinde max paket
#define RF_HEADER_LEN 3  // TYPE(1) + SEQ(2)
#define RF_MAX_DATA 50   // Max payload

// Komut tipleri
#define RF_CMD_BOOT_REQUEST 0x01
#define RF_CMD_BOOT_ACK 0x02
#define RF_CMD_METADATA 0x03
#define RF_CMD_DATA_CHUNK 0x04
#define RF_CMD_FLASH_ERASE_DONE 0x05
#define RF_CMD_UPDATE_COMPLETE 0x06
#define RF_CMD_UPDATE_FAILED 0x07
#define RF_CMD_KEY_UPDATE 0x08        // Gonderici → Alici: Yeni master key
#define RF_CMD_KEY_UPDATE_ACK 0x09    // Alici → Gonderici: Key yazildi
#define RF_CMD_ACK 0x10
#define RF_CMD_NACK 0x11
#define RF_CMD_PING 0x12
#define RF_CMD_PONG 0x13

// Zamanlama sabitleri
#define RF_ACK_TIMEOUT_MS 2000
#define RF_MAX_RETRIES 5
#define RF_CHUNK_DATA_SIZE 48
#define RF_CHUNKS_PER_PACKET 4 // 148 byte → 4 RF parça (48+48+48+4)

// ECDH sabitleri
#define ECDH_KEY_SIZE          32U  // X25519 public/private key boyutu
#define BOOT_REQUEST_PLD_SIZE  32U  // BOOT_REQUEST payload: [pub_sender:32]
#define BOOT_ACK_PLD_SIZE      36U  // BOOT_ACK payload: [resume_start:4][pub_receiver:32]
#define KEY_UPDATE_PLD_SIZE    33U  // KEY_UPDATE payload: [enc_key:32][crc8:1]

// UART firmware update sabitleri
// Yeni UART handshake: 'W'(1) + pub_sender(32) = 33 byte gonderilir
// Cevap: pub_receiver(32) + ACK(1) = 33 byte alinir
#define UART_FW_START_SIZE     33U  // 'W' + pub_sender
#define UART_FW_ACK_SIZE       33U  // pub_receiver + ACK
#define UART_ACK 0x06
#define UART_NACK 0x15
#define FW_PACKET_SIZE 128      // Python'dan gelen şifreli paket boyutu
#define FW_FULL_PACKET_SIZE 148 // IV(16) + Encrypted(128) + CRC32(4)

#endif // RF_PROTOCOL_H
