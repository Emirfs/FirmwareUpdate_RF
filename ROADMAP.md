# Yol Haritası — FirmwareUpdate_RF

> /init-project + kapsamlı kaynak analizi ile oluşturuldu (2026-03-27).
> Tamamlanan özellikler REQUIREMENTS.md ile senkronize.

## Mevcut Durum (2026-03-27)

Sistem çalışır durumda. Temel firmware güncelleme akışı, güvenlik katmanı ve
Python uploader tamamlanmış ve test edilmiş.

## Tamamlananlar

### Faz 1 — Temel RF Güncelleme
- [x] Si4432 sürücüsü (gönderici + alıcı)
- [x] RF protokol tasarımı (BOOT_REQUEST → BOOT_ACK → METADATA → DATA_CHUNK × N → COMPLETE)
- [x] AES-256-CBC firmware şifreleme
- [x] Flash yazma/silme/doğrulama (halfword tabanlı)
- [x] Boot flag mekanizması
- [x] Uygulama atlama (SYSCFG SRAM remap ile)

### Faz 2 — Güvenilirlik İyileştirmeleri
- [x] Sayfa bazlı resume mekanizması (boot flag page bitmap)
- [x] ACK/NACK güvenilir iletim (RF_MAX_RETRIES = 5)
- [x] Kanal sabitleme (frekans atlama kaldırıldı — sabit kanal 0)
- [x] Reaktif BOOT_ACK (BOOT_REQUEST gelince anında yanıt)
- [x] Sayfa bazlı Flash silme (önceki: tüm alan silme ~555ms)
- [x] Watchdog koruması (her uzun döngüde IWDG_Refresh)

### Faz 3 — Güvenlik Katmanı
- [x] X25519 ECDH key exchange (c25519 + f25519 public domain kütüphane)
- [x] KEY_STORE (Flash page 15, kalıcı AES master key)
- [x] KEY_UPDATE protokol komutu (RF üzerinden remote key güncelleme)
- [x] GUI şifreli konfigürasyon (config.enc + credentials.enc)
- [x] Proxy server (kanal bazlı firmware kataloğu + kısa ömürlü token)

### Faz 4 — GUI ve Kullanıcı Deneyimi
- [x] Qt PySide6 3 adımlı wizard arayüzü
- [x] Admin paneli (cihaz, key, backend, port yönetimi)
- [x] Upload ilerleme takibi
- [x] Proxy admin paneline eklenmesi

## Devam Eden / Planlanan

- [ ] WID embedded-master sistemi ile proje hafızasının tamamlanması
- [ ] footprint-scout.md ve price-scout.md ajanlarının kurulumu
- [ ] Test mühendisi ile unit test coverage artırımı
- [ ] Donanım doğrulaması (Gate 5) — kart bağlıyken `/verify-hardware`
- [ ] NeoPixel LED animasyon iyileştirmeleri (isteğe bağlı)
- [ ] rf_uploader.py ile gui_uploader_qt.py ECDH akışının birleştirilmesi (isteğe bağlı)
