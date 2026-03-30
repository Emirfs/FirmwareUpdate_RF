# İlerleme Kaydı — FirmwareUpdate_RF

> 2026-03-27 tarihinde /init-project ile kaynak analizi sonrası oluşturuldu.

## Tamamlananlar

### Alıcı Firmware (`alici_cihaz/`)
- [x] `boot_flow.c` — Bootloader ana akışı, ECDH, chunk toplama, AES çözme, Flash yazma
- [x] `boot_storage.c` — Flash yönetimi (boot flag, resume bitmap, key store)
- [x] `boot_rf.c` — RF katmanı (RF_SendPacket, RF_WaitForPacket, RF_SendReliable)
- [x] `boot_led.c` — NeoPixel LED durum göstergeleri
- [x] `si4432.c` — Si4432 sürücüsü (433 MHz, 9.6kbps)
- [x] `aes.c` — AES-256-CBC yazılım implementasyonu
- [x] `c25519.c` + `f25519.c` — X25519 ECDH kütüphanesi
- [x] `entropy.c` — UID + ADC + SysTick bazlı rastgelelik
- [x] `neopixel.c` — NeoPixel WS2812 sürücüsü
- [x] `main.c` — Boot karar mantığı (flag → bootloader, MSP kontrol → 3sn RF listen → uygulama)

### Gönderici Firmware (`uart_rf_gonderici/`)
- [x] `sender_fw_update.c` — FW update state machine (ECDH + resume + paket transfer)
- [x] `sender_rf_link.c` — RF katmanı (RF_SendPacket, RF_WaitForPacket, RF_SendChunkReliable)
- [x] `sender_normal_mode.c` — Normal mod (UART echo/test + 'W' tetikleyici)
- [x] `si4432.c` — Si4432 sürücüsü (LBT dahil)
- [x] `main.c` — Ana döngü (UART polling + HandleNormalModeByte)

### PC Uploader (`Uploader/`)
- [x] `rf_uploader.py` — ECDH + AES CLI uploader (--new-master-key desteği)
- [x] `gui_uploader_qt.py` — Qt PySide6 3 adımlı wizard GUI
- [x] `uploder.py` — GUI backend yükleme motoru
- [x] `config_manager.py` — config.enc + credentials.enc şifreli yönetim
- [x] `firmware_proxy_client.py` — Proxy API istemcisi
- [x] `firmware_proxy_server.py` — Kanal bazlı proxy (Google Drive + kısa ömürlü token)
- [x] `drive_manager.py` — Google Drive erişim katmanı

### Protokol ve Güvenlik
- [x] RF protokol (komutlar, zamanlayıcılar, paket yapısı) — `rf_bootloader.h` / `rf_protocol.h`
- [x] Firmware şifreleme (IV:16 + Encrypted:128 + CRC32:4 = 148 byte)
- [x] 4 RF DATA_CHUNK parçalama (48+48+48+4 byte)
- [x] Resume mekanizması (sayfa bitmap, BOOT_ACK payload)
- [x] ECDH key exchange (her oturumda ephemeral)
- [x] KEY_STORE (kalıcı) + KEY_UPDATE (RF üzerinden remote güncelleme)

## Yapılacaklar

- [ ] footprint-scout.md ve price-scout.md ajanlarının `~/.claude/agents/`'e kopyalanması
- [ ] Donanım doğrulaması (Gate 5) — kart bağlı olduğunda `/verify-hardware`
- [ ] Unit test coverage artırımı (CRC, AES, resume fonksiyonları)
- [ ] REQUIREMENTS.md'deki işaretsiz maddelerin tamamlanması
