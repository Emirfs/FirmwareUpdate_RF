# Changelog — FirmwareUpdate_RF

> Her PRP veya önemli aşama tamamlandığında `github-agent` tarafından güncellenir.
> Format: En yeni aşama en üstte.

---

## [init] — embedded-master (WID) Sistemi Eklendi

**Tarih:** 2026-03-27
**Branch:** main
**PRP:** null

### Tamamlananlar
- ✅ WID embedded-master ajan sistemi mevcut projeye entegre edildi (/init-project)
- ✅ Kapsamlı kaynak kodu analizi yapıldı (tüm .c/.h dosyaları okundu)
- ✅ CLAUDE.md — STM32 kodlama standartları
- ✅ PROJECT.md — MCU, clock, donanım detayları
- ✅ STATE.md — Proje durumu
- ✅ REQUIREMENTS.md — Fonksiyonel gereksinimler
- ✅ ROADMAP.md — Faz bazlı yol haritası
- ✅ project_memory/hardware.md — Pin atamaları, flash düzeni, Si4432 ayarları
- ✅ project_memory/decisions.md — 10 mimari karar kaydı
- ✅ project_memory/bugs.md — 4 çözülmüş hata kaydı
- ✅ project_memory/progress.md — Tüm tamamlanan modüller
- ✅ project_memory/changelog.md — Bu dosya

### Mevcut Proje Durumu
Sistem çalışır durumda. RF firmware güncelleme akışı, ECDH güvenlik katmanı, resume mekanizması
ve Qt GUI tamamlanmış. Bilinen hatalar (kanal kayması, BOOT_ACK zamanlama, Flash silme gecikmesi) çözülmüş.

### Sonraki Adım
- `/embedded-master` ile oturum başlat
- Yeni özellik için `/generate-prp "<görev>"` ile görev planla

---

## [v4] — Proxy Server Admin Paneline Eklendi

**Tarih:** ~2026-03-17
**Branch:** main
**Commit:** 762f872

### Tamamlananlar
- ✅ Proxy server admin paneli entegrasyonu

---

## [v3] — Güvenlik İyileştirmeleri

**Tarih:** ~2026-03-11
**Branch:** main
**Commit:** c47a5ec

### Tamamlananlar
- ✅ ECDH X25519 key exchange
- ✅ KEY_STORE flash alanı
- ✅ KEY_UPDATE protokol komutu
- ✅ config.enc + credentials.enc

---

## [v2] — Sistem İyileştirmeleri

**Tarih:** ~2026-03-11
**Branch:** main
**Commit:** d3c6856

### Tamamlananlar
- ✅ Arayüz güncelleme
- ✅ Kanal kayması ve BOOT_ACK zamanlama düzeltmeleri

---

## [v1] — İlk Çalışan Versiyon

**Tarih:** ~2026-02 (tahmin)
**Branch:** main

### Tamamlananlar
- ✅ Si4432 RF sürücüsü
- ✅ Temel firmware güncelleme protokolü
- ✅ AES-256-CBC şifreleme
- ✅ Resume mekanizması
- ✅ Qt GUI temel versiyonu
