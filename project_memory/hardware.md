# Donanım Notları — FirmwareUpdate_RF

## Alıcı Cihaz (alici_cihaz)

### MCU Bilgileri
- **Model:** STM32F030CCT6
- **Paket:** LQFP48
- **Core:** Cortex-M0
- **Flash:** 256 KB (128 sayfa × 2 KB)
- **RAM:** 32 KB
- **CubeMX:** STM32F030CCTx

### Konfigürasyon
- **Sistem Clock:** HSI (8 MHz) × 6 PLL = **24 MHz**
- **HSE/HSI:** HSI aktif
- **LSI:** Aktif (IWDG + RTC için)
- **IWDG:** Prescaler 128
- **RTC:** LSI kaynağı (sadece IWDG desteği için)
- **Flash latency:** 1 wait state

### Aktif Peripheral'lar
| Peripheral | Kullanım |
|-----------|---------|
| SPI2 | Si4432 RF modülü |
| TIM17 | NeoPixel veri sinyali (PWM/DMA) |
| TIM6 | NeoPixel IC timer |
| TIM16 | Rezerve |
| TIM3 | Rezerve |
| IWDG | Watchdog koruması |
| RTC | LSI destekli |
| NVIC EXTI4_15 | Si4432 nIRQ interrupt (priority 2) |
| NVIC TIM17 | NeoPixel timer interrupt (priority 3) |

### Pin Atamaları (alici_cihaz)
| Pin | Fonksiyon | Not |
|-----|-----------|-----|
| PB12 | Si4432 nSEL (CS) | SPI2 CS, aktif düşük |
| PB13 | SPI2_SCK | Si4432 CLK |
| PB14 | SPI2_MISO | Si4432 SDO |
| PB15 | SPI2_MOSI | Si4432 SDI |
| PA11 | Si4432 SDN | Shutdown pin |
| PA8 | Si4432 nIRQ | EXTI4_15 interrupt |
| PB0 | NeoPixel veri | TIM17 ile sürülür |
| PC13 | GPIO | (TBD) |
| PB10 | GPIO | (TBD) |
| PB11 | GPIO | (TBD) |
| PA13 | SWDIO | Debug |
| PA14 | SWDCLK | Debug |
| PA15 | GPIO | (TBD) |
| PB3 | GPIO | (TBD) |

### Flash Bellek Düzeni
```
0x08000000 ┌─────────────────────┐
           │  BOOTLOADER (32KB)  │  Sayfa 0–15 (16 × 2KB)
           │  └ KEY_STORE        │  Sayfa 15 (0x08007800) — AES master key
0x08008000 ├─────────────────────┤
           │  APPLICATION (222KB)│  Sayfa 16–126 (111 × 2KB)
0x0803F800 ├─────────────────────┤
           │  BOOT FLAG (2KB)    │  Sayfa 127 — son sayfa
0x0803FFFF └─────────────────────┘
```

**Boot Flag Sayfası Düzeni (0x0803F800):**
| Offset | Boyut | İçerik |
|--------|-------|--------|
| +0 | 4B | BOOT_FLAG_MAGIC (0xB007B007) |
| +4 | 4B | BOOT_FLAG_REQUEST (0x00000001) veya 0xFFFFFFFF |
| +8 | 4B | Firmware version |
| +12 | 4B | RESUME_MAGIC (0x12345678) |
| +16 | 4B | Toplam paket sayısı |
| +20 | 222B | Sayfa tamamlanma bitmap (111 × 2B halfword) |

**KEY_STORE (0x08007800 — sayfa 15):**
| Offset | Boyut | İçerik |
|--------|-------|--------|
| +0 | 4B | KEY_STORE_MAGIC (0xAE5CAFE5) |
| +4 | 32B | master_key[32] |
| +36 | 1B | key_crc8 (CRC-8 SMBUS polinom 0x07) |

---

## Gönderici Cihaz (uart_rf_gonderici)

### MCU Bilgileri
- **Model:** STM32F030C8 (LQFP48)
- **Core:** Cortex-M0
- **Flash:** 64 KB
- **RAM:** 8 KB

### Konfigürasyon
- **Sistem Clock:** HSI (8 MHz) × 12 PLL = **48 MHz**
- **IWDG:** Aktif
- **Flash latency:** 1 wait state

### Aktif Peripheral'lar
| Peripheral | Kullanım |
|-----------|---------|
| USART1 | PC ile haberleşme, 115200 8N1, DMA |
| SPI2 | Si4432 RF modülü |
| DMA | UART DMA kanalları |
| TIM17 | Rezerve |
| TIM16 | Rezerve |
| IWDG | Watchdog koruması |
| LED0 | Durum göstergesi (GPIO) |
| LED1 | Transfer göstergesi (GPIO) |

### Si4432 RF Modülü (Her İki Cihaz)
- **Arayüz:** SPI2 (Mode 0, CPOL=0, CPHA=0)
- **CS (nSEL):** SI4432_CE_GPIO_Port/SI4432_CE_Pin
- **IRQ (nIRQ):** SI4432_IRQ_GPIO_Port/SI4432_IRQ_Pin (aktif düşük)
- **SDN:** SI4432_SDN pin (göndericide) — shutdown
- **Device Type Register (0x00):** 0x08 beklenir
- **Frekans:** 433.0 MHz (reg 0x75/0x76/0x77)
- **Veri hızı:** 9.6 kbps (reg 0x6E/0x6F/0x70)
- **TX gücü:** reg 0x6D = 0x07 (maksimum +20 dBm)
- **Preamble:** 10 nibble = 5 byte (reg 0x34)
- **Sync word:** 0x2D 0xD4
- **CRC:** CRC-IBM (donanım otomatik)
- **LBT:** Göndericide aktif (SI4432_LBT_WaitForClear, max 200ms) — alıcıda yok
