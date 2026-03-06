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

/* Flash'taki uygulamaya atla (geri donmez — uygulama calismaya baslar) */
void jump_to_application(void);

/* RF firmware guncelleme ana dongusu (tamamlaniinca NVIC_SystemReset cagrılir) */
void Bootloader_Main(void);

#endif /* BOOT_FLOW_H */
