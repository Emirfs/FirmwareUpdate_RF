# Gereksinimler — FirmwareUpdate_RF

> /init-project + kapsamlı kaynak analizi ile oluşturuldu (2026-03-27).

## Fonksiyonel Gereksinimler

### RF İletişim
- [x] Si4432 üzerinden 433 MHz, 9.6 kbps GFSK ile veri iletimi
- [x] Sabit kanal 0 (frekans atlama yok)
- [x] Donanım CRC-IBM ile paket bütünlüğü
- [x] Yazılım CRC-32 (ISO-HDLC) ile firmware paket bütünlüğü
- [x] ACK/NACK tabanlı güvenilir DATA_CHUNK iletimi (RF_MAX_RETRIES = 5)
- [x] 2000 ms ACK timeout

### Bootloader (Alıcı)
- [x] Boot flag kontrolü ile bootloader/uygulama kararı
- [x] Geçerli uygulama varsa 3 sn RF dinleme penceresi
- [x] BOOT_REQUEST → BOOT_ACK el sıkışması
- [x] Firmware metadata alımı (boyut, versiyon, CRC-32)
- [x] 148 byte paket → 4 DATA_CHUNK RF parçalama
- [x] Her 128 byte AES-256-CBC çözme
- [x] Sayfa bazlı Flash yazımı (halfword programlama)
- [x] Flash verify (byte bazlı karşılaştırma)
- [x] Final Flash CRC-32 doğrulaması
- [x] UPDATE_COMPLETE / UPDATE_FAILED bildirimi
- [x] Başarılı güncelleme sonrası uygulamaya atlama

### Resume (Kesinti Kurtarma)
- [x] Sayfa bazlı resume bitmap (boot flag page'de, offset +20)
- [x] Her tamamlanan sayfa (16 paket = 2KB) bitmap'e kaydedilir (0xFFFF → 0x0000)
- [x] Reset sonrası kaldığı yerden devam etme
- [x] BOOT_ACK payload'ında resume_start_packet bildirimi (4 byte)

### Güvenlik
- [x] X25519 ECDH ile ephemeral oturum anahtarı türetme
- [x] AES-256-CBC ile firmware şifreleme (her blok için rastgele IV)
- [x] KEY_STORE: kalıcı AES master key (Flash page 15, 0x08007800)
- [x] KEY_UPDATE: session key ile şifrelenmiş yeni master key RF üzerinden güncelleme
- [x] CRC-8 ile key store bütünlüğü

### Uygulama Atlama (alici_cihaz)
- [x] MSP doğrulaması (0x2000xxxx kontrolü)
- [x] STM32F030 için SYSCFG MEM_MODE=11 ile vektör tablosu SRAM'e remap
- [x] Tüm NVIC interrupt'ları devre dışı bırakma
- [x] HAL deinit + SysTick durdurma

### Gönderici (uart_rf_gonderici)
- [x] UART 115200 8N1, DMA destekli
- [x] 'W' karakteri ile FW update modu tetikleme
- [x] pub_sender 32 byte UART üzerinden alımı
- [x] pub_receiver 32 byte UART üzerinden iletimi
- [x] Resume destekli paket atlama (ilk N paket RF'e gönderilmez)
- [x] Normal mod: UART echo/test modu
- [x] LBT (Listen Before Talk) — SI4432_LBT_WaitForClear, max 200ms

### PC Tarafı (Uploader)
- [x] Qt tabanlı PySide6 GUI (3 adımlı wizard)
- [x] Admin paneli (cihaz profili, AES key, backend, port yönetimi)
- [x] .bin ve .hex format desteği
- [x] Proxy/backend üzerinden kanal bazlı firmware kataloğu
- [x] Google Drive entegrasyonu (proxy aracılığıyla)
- [x] config.enc + credentials.enc şifreli konfigürasyon
- [x] rf_uploader.py: ECDH + opsiyonel --new-master-key CLI aracı

## Kısıtlar

- Flash (Alıcı): 256 KB — Bootloader maks 32 KB, Uygulama maks 222 KB
- Flash (Gönderici): 64 KB
- RAM (Alıcı): 32 KB
- RAM (Gönderici): 8 KB
- RF max paket: 53 byte (3 başlık + 50 payload)
- Clock (Alıcı): 24 MHz
- Clock (Gönderici): 48 MHz
- FreeRTOS: Kullanılmıyor
