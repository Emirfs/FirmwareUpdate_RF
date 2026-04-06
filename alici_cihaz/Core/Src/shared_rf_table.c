/*
 * shared_rf_table.c — RF Library ROM API Table (sabit adres: 0x08006000)
 *
 * Bu dosya yalnızca pointer tablosunu tanımlar.
 * Gerçek fonksiyonlar si4432.c ve boot_rf.c'de bulunur;
 * linker onları .shared_lib section'ına (0x08006040+) yerleştirir.
 *
 * Bootloader bu dosyayı derler ve table'ı Flash'a yazar.
 * Uygulama shared_rf_api.h'daki SHARED_RF_LIB makrosu ile table'ı okur.
 */

#include "shared_rf_api.h"
#include "si4432.h"
#include "boot_rf.h"

/*
 * __attribute__((section(".shared_lib_table")))
 *   → Linker script'teki .shared_lib_table section'ına gider (0x08006000).
 *
 * const → Flash'a yazılır (RAM'e kopyalanmaz).
 * KEEP  → Linker script'te KEEP(*(.shared_lib_table)) ile garbage collect edilmez.
 */
const SharedRfTable_t shared_rf_table
    __attribute__((section(".shared_lib_table"))) =
{
    .magic          = RF_TABLE_MAGIC,
    .version        = RF_TABLE_VERSION,

    /* si4432 */
    .SI4432_WriteReg   = SI4432_WriteReg,
    .SI4432_ReadReg    = SI4432_ReadReg,
    .SI4432_Init       = SI4432_Init,
    .SI4432_SendPacket = SI4432_SendPacket,
    .SI4432_StartRx    = SI4432_StartRx,
    .SI4432_CheckRx    = SI4432_CheckRx,

    /* boot_rf */
    .RF_SendPacket    = RF_SendPacket,
    .RF_WaitForPacket = RF_WaitForPacket,
    .RF_SendReliable  = RF_SendReliable,

    ._reserved        = {0, 0, 0, 0, 0},
};
