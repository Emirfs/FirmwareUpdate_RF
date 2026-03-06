/*
 * sender_fw_update.h — Firmware Update Modu Arayuzu
 *
 * PC'den 'W' komutu alindiktan sonra cagrilir.
 * Tum RF protokol handshake + paket transferi bu fonksiyon icinde yonetilir.
 * Fonksiyon bitince sistem normal moda doner.
 */
#ifndef SENDER_FW_UPDATE_H
#define SENDER_FW_UPDATE_H

/* Firmware update modunu baslat.
 * Donus: fonksiyon tamamlandiktan sonra normal moda doner.
 * Cagrildigi yer: sender_normal_mode.c → HandleNormalModeByte() */
void FirmwareUpdate_Mode(void);

#endif /* SENDER_FW_UPDATE_H */
