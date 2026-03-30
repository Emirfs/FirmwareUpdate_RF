# Proje: FirmwareUpdate_RF

## Genel Açıklama

Si4432 433 MHz RF üzerinden uzaktan STM32 firmware güncelleme sistemi.
PC → UART → STM32 Gönderici (köprü) → Si4432 RF → STM32 Alıcı Bootloader.

## Donanım

### Alıcı Cihaz (`alici_cihaz/`)
- **MCU:** STM32F030CCT6 (LQFP48)
- **Core:** Cortex-M0
- **Flash:** 256 KB (64 sayfa × 2 KB değil; 128 sayfa × 2 KB = 256 KB)
- **RAM:** 32 KB
- **Sistem Clock:** HSI × 6 PLL = **24 MHz**
- **RF Modül:** Si4432 — SPI2
- **LED:** NeoPixel (TIM17 + TIM6)
- **Watchdog:** IWDG (prescaler 128)
- **RTC:** LSI kaynaklı

### Gönderici Cihaz (`uart_rf_gonderici/`)
- **MCU:** STM32F030C8 (LQFP48)
- **Core:** Cortex-M0
- **Flash:** 64 KB
- **RAM:** 8 KB
- **Sistem Clock:** HSI × 12 PLL = **48 MHz**
- **RF Modül:** Si4432 — SPI2
- **UART:** USART1, 115200 8N1, DMA
- **LED:** LED0, LED1 (GPIO)
- **Watchdog:** IWDG

## Proje Hedefi

Fiziksel erişimin zor olduğu cihazlara kablosuz ve güvenli firmware güncellemesi.
- Paket seviyesinde CRC-32 bütünlük kontrolü
- AES-256-CBC ile firmware şifreleme
- X25519 ECDH ile ephemeral oturum anahtarı türetme
- Sayfa bazlı resume (kesilen transferi devam ettirme)
- Qt tabanlı GUI + Python CLI uploader

## RTOS

**FreeRTOS kullanılmıyor** — Her iki cihaz da bare-metal (polling loop + IWDG).

## RF Ayarları (Si4432)

- Frekans: 433.0 MHz
- Veri hızı: 9.6 kbps GFSK
- Sapma: ±45 kHz
- Preamble: 5 byte (10 nibble)
- Sync: `0x2D 0xD4`
- CRC: CRC-IBM (donanım otomatik)
- Max paket: 64 byte FIFO → kullanılan: 53 byte (3 byte başlık + 50 byte payload)
- Kanal: Sabit kanal 0 (register 0x79 = 0x00)

## Init-project ile Eklendi

2026-03-27
