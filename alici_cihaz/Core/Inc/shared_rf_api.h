/*
 * shared_rf_api.h — Shared RF Library ROM API Table
 *
 * Sabit adres: 0x08006000 (SHARED_LIB_FLASH başı, page 12)
 *
 * ─── KULLANIM ────────────────────────────────────────────────────────────────
 * Bootloader:
 *   Doğrudan si4432.c / boot_rf.c fonksiyonlarını çağırır — table overhead yok.
 *   Bu header'ı include etmek zorunda değil, ama etse de çalışır.
 *
 * Uygulama:
 *   #include "shared_rf_api.h"
 *   SHARED_RF_LIB->RF_SendPacket(type, seq, payload, len);
 *   SHARED_RF_LIB->SI4432_Init();
 *
 * ─── BELLEK DÜZENİ ───────────────────────────────────────────────────────────
 *   0x08006000  SharedRfTable_t  (ROM API Table — bu struct)
 *   0x08006040+ RF Library kodu  (si4432.o + boot_rf.o)
 *   ...
 *   0x08007800  KEY_STORE (page 15) — bu bölge dışında, boot_storage.c yönetir
 *
 * ─── GENİŞLEME ───────────────────────────────────────────────────────────────
 * İleride eklecek kütüphaneler (c25519, AES, entropy) için yeni bir
 * SharedCryptoTable_t veya benzeri struct 0x08006040+ adresinden sonraki
 * boş alana eklenecek. Bu table'ın struct boyutu değişmez — yeni table
 * ayrı bir .shared_lib_table_xxx section ile sabitlenir.
 */

#ifndef SHARED_RF_API_H
#define SHARED_RF_API_H

#include <stdint.h>

/* ─── si4432 API ─────────────────────────────────────────────────────────── */

typedef void     (*pfn_SI4432_WriteReg)  (uint8_t reg, uint8_t value);
typedef uint8_t  (*pfn_SI4432_ReadReg)   (uint8_t reg);
typedef void     (*pfn_SI4432_Init)      (void);
typedef void     (*pfn_SI4432_SendPacket)(const uint8_t *data, uint8_t len);
typedef void     (*pfn_SI4432_StartRx)   (void);
typedef uint8_t  (*pfn_SI4432_CheckRx)   (uint8_t *data);

/* ─── boot_rf API ────────────────────────────────────────────────────────── */

typedef void     (*pfn_RF_SendPacket)    (uint8_t type, uint16_t seq,
                                          const uint8_t *payload,
                                          uint8_t payload_len);
typedef uint8_t  (*pfn_RF_WaitForPacket) (uint8_t *type, uint16_t *seq,
                                          uint8_t *payload, uint8_t *payload_len,
                                          uint32_t timeout_ms);
typedef uint8_t  (*pfn_RF_SendReliable)  (uint8_t type, uint16_t seq,
                                          const uint8_t *payload,
                                          uint8_t payload_len);

/* ─── ROM API Table ──────────────────────────────────────────────────────── */

/*
 * TABLO KURALI: Bu struct'a yalnızca SONA ekleme yapılır.
 *
 * Var olan pointer'ların sırası hiçbir zaman değiştirilmez.
 * Silinmez — sadece NULL ile işaretlenir (geriye dönük uyumluluk için).
 * İlerideki kütüphaneler bu struct'a DEĞİL, kendi table'larına eklenir.
 */
typedef struct
{
    /* Sihirli sayı — table'ın geçerli olduğunu doğrular */
    uint32_t magic;                         /* 0x00: RF_TABLE_MAGIC */
    uint32_t version;                       /* 0x04: tablo versiyonu (artırılır) */

    /* si4432 fonksiyonları */
    pfn_SI4432_WriteReg   SI4432_WriteReg;  /* 0x08 */
    pfn_SI4432_ReadReg    SI4432_ReadReg;   /* 0x0C */
    pfn_SI4432_Init       SI4432_Init;      /* 0x10 */
    pfn_SI4432_SendPacket SI4432_SendPacket;/* 0x14 */
    pfn_SI4432_StartRx    SI4432_StartRx;   /* 0x18 */
    pfn_SI4432_CheckRx    SI4432_CheckRx;   /* 0x1C */

    /* boot_rf fonksiyonları */
    pfn_RF_SendPacket     RF_SendPacket;    /* 0x20 */
    pfn_RF_WaitForPacket  RF_WaitForPacket; /* 0x24 */
    pfn_RF_SendReliable   RF_SendReliable;  /* 0x28 */

    /* 0x2C-0x3F: rezerve — gelecek RF fonksiyonları için */
    uint32_t _reserved[5];

} SharedRfTable_t;  /* toplam: 64 byte — tam olarak 0x08006000-0x0800603F */

/* ─── Makro: Uygulama tarafından kullanılır ──────────────────────────────── */

#define RF_TABLE_MAGIC       0x52465442UL   /* "RFTB" */
#define RF_TABLE_VERSION     1U

#define SHARED_RF_TABLE_ADDR 0x08006000UL

#define SHARED_RF_LIB  ((const SharedRfTable_t *)(SHARED_RF_TABLE_ADDR))

/*
 * Örnek kullanım (uygulama tarafı):
 *
 *   if (SHARED_RF_LIB->magic != RF_TABLE_MAGIC) { Error_Handler(); }
 *   SHARED_RF_LIB->SI4432_Init();
 *   SHARED_RF_LIB->RF_SendPacket(0x01, 0, NULL, 0);
 */

#endif /* SHARED_RF_API_H */
