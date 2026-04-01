# STM32 Gömülü Sistem Kodlama Standartları — FirmwareUpdate_RF

> Bu dosya yalnızca STM32 kodlama standartlarını içerir.
> Master ajanı başlatmak için: `/embedded-master` komutunu çalıştır.

## Zorunlu Kodlama Kuralları

- Tüm integer'lar explicit-width type kullanır: `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` (`int`, `long` yasak)
- Tüm HAL çağrıları dönüş değeri kontrol edilir: `if (HAL_XXX() != HAL_OK) { Error_Handler(); }`
- ISR ve main arasında paylaşılan tüm değişkenler `volatile` zorunlu
- ISR içinde dinamik bellek tahsisi (`malloc`/`free`) kesinlikle yasak
- Include guard zorunlu — `#pragma once` değil
- Max 300 satır per `.c` dosyası
- Flash yazımı yalnızca `HAL_FLASH_Unlock()` / `HAL_FLASH_Lock()` çifti içinde yapılır
- STM32F030 yalnızca 16-bit (halfword) Flash yazımını destekler — her zaman `FLASH_TYPEPROGRAM_HALFWORD` kullan
- Her Flash silme/yazma sonrası IWDG sıfırla: `HAL_IWDG_Refresh(&hiwdg)`
- RF protokol sabitleri tek kaynak: `uart_rf_gonderici/Core/Inc/rf_protocol.h` (gönderici) ve `alici_cihaz/Core/Inc/rf_bootloader.h` (alıcı)

## Proje Özel Kurallar

- Alıcı ve gönderici Si4432 register ayarları (frekans, veri hızı, sync) birebir aynı olmalı
- ECDH key exchange için `c25519_prepare()` sonrası `c25519_smult()` çağrısı zorunlu (bit clamp)
- ECDH private key RAM'den temizlenmeli (`memset(priv, 0, 32)`) işlem tamamlandıktan hemen sonra
- Boot flag page (sayfa 127) yalnızca `clear_boot_flag()` / `set_boot_flag()` ile değiştirilir
- KEY_STORE page (sayfa 15) yalnızca `KeyStore_Write()` / `KeyStore_Read()` ile değiştirilir
- Uygulama Flash silme işlemi yalnızca sayfa 16-126 arasını etkiler

## Dosya Organizasyonu (Bu Proje)

```
uart_rf_gonderici/Core/
  Src/sender_fw_update.c    # FW update ana akışı (gönderici)
  Src/sender_rf_link.c      # RF katmanı (gönderici)
  Src/sender_normal_mode.c  # Normal mod (test/echo + 'W' tetikleyici)
  Src/sender_state.c        # Gönderici durum makinesi
  Src/sender_uart_debug.c   # UART debug yardımcıları
  Src/si4432.c              # Si4432 sürücüsü (LBT dahil)
  Src/c25519.c              # X25519 ECDH (key exchange)
  Src/f25519.c              # Field arithmetic (c25519 bağımlılığı)
  Src/entropy.c             # Entropi kaynağı (UID + ADC + SysTick)
  Inc/rf_protocol.h         # Ortak protokol sabitleri
  Inc/sender_state.h
  Inc/sender_uart_debug.h
  Inc/c25519.h / f25519.h / entropy.h

alici_cihaz/Core/
  Src/boot_flow.c           # Bootloader ana akışı + uygulama atlama
  Src/boot_rf.c             # RF katmanı (alıcı)
  Src/boot_storage.c        # Flash yönetimi (boot flag, resume, key store)
  Src/boot_led.c            # NeoPixel LED durum göstergeleri
  Src/neopixel.c            # NeoPixel sürücüsü (boot_led bağımlılığı)
  Src/aes.c                 # AES-128 şifreleme
  Src/c25519.c              # X25519 ECDH (key exchange)
  Src/f25519.c              # Field arithmetic (c25519 bağımlılığı)
  Src/entropy.c             # Entropi kaynağı (UID + ADC + SysTick)
  Src/rtc.c                 # RTC peripheral (CubeMX generate)
  Inc/rf_bootloader.h       # Flash layout, protokol ve AES sabitleri
  Inc/neopixel.h / aes.h / c25519.h / f25519.h / entropy.h / rtc.h

Uploader/
  gui_uploader_qt.py        # Qt GUI ana pencere
  uploder.py                # RF upload + key exchange mantığı (gui tarafından import edilir)
  rf_uploader.py            # CLI uploader
  config_manager.py         # Şifreli config yönetimi
  device_monitor.py         # Cihaz bağlantı izleme
  drive_manager.py          # Sürücü yönetimi
  firmware_proxy_client.py  # Proxy istemci
  firmware_proxy_server.py  # Proxy sunucu
  gui_logger.py             # GUI log yönetimi
```

## Oturum Başlangıcı (Master Ajan Aktifken)

1. Bu `CLAUDE.md` dosyasını oku
2. `STATE.md` oku (`failure_count`, `active_prp`, son durum)
3. `project_memory/*.md` dosyalarını oku
4. Kullanıcıya bildir: `"Aktif proje: FirmwareUpdate_RF | MCU: STM32F030CC / STM32F030C8 | Durum: <özet>"`
