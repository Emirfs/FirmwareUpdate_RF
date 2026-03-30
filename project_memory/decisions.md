# Mimari Kararlar — FirmwareUpdate_RF

> Kaynak kodu ve git geçmişinden çıkarılan kararlar. 2026-03-27 tarihinde /init-project ile oluşturuldu.

---

## [KARAR-001] Frekans Atlama Kaldırıldı — Sabit Kanal 0

**Karar:** Her iki cihaz da sabit kanal 0 kullanır (Si4432 reg 0x79 = 0x00). Frekans atlama devre dışı.

**Neden:** RF link'de kanal kayması yaşandı. Gönderici ve alıcı farklı kanallara geçince BOOT_ACK kaçırılıyordu. Transfer sırasında güvenilirlik sorunları oluştu.

**Nasıl:** `rf_link_stm32.c` ve `rf_link_sender.c`'den tüm `rf_link_hop_channel()` çağrıları kaldırıldı.

---

## [KARAR-002] Reaktif BOOT_ACK — Periyodik Değil, Anlık Yanıt

**Karar:** Alıcı, gönderici BOOT_REQUEST gönderdiğinde anında BOOT_ACK ile yanıt verir. Periyodik broadcast yerine olay tetiklemeli yanıt.

**Neden:** Önceki tasarımda alıcı ~1200ms döngü başında BOOT_ACK gönderiyordu. Gönderici 2000ms pencere açıyordu. Pencere BOOT_ACK ile örtüşmeyince el sıkışma kaçırılıyordu.

**Nasıl:** `boot_flow.c` içindeki `while(!got_metadata)` döngüsüne `else if (rx_type == RF_CMD_BOOT_REQUEST)` bloğu eklendi.

---

## [KARAR-003] Sayfa Bazlı Flash Silme — Önceden Tam Silme Değil

**Karar:** Flash silme işlemi, her 128-byte yazımdan önce sayfa sınırında (current_addr % FLASH_PAGE_SIZE == 0) gerçekleştirilir.

**Neden:** Önceki tasarımda tüm uygulama alanı (111 sayfa × ~5ms = ~555ms) FLASH_ERASE_DONE öncesinde siliniyordu. Bu uzun bekleme süresine ve resume ile uyumsuzluğa yol açıyordu.

**Nasıl:** `Flash_Erase_Application()` yerine `Flash_Erase_Page(current_addr)` kullanılıyor; her sayfa başında çağrılır.

---

## [KARAR-004] ECDH X25519 — c25519 Public Domain Kütüphane

**Karar:** X25519 ECDH için `c25519` + `f25519` public domain kütüphanesi kullanıldı. Her oturumda ephemeral key pair üretilir.

**Neden:** STM32F030 için hafif, STM32 HAL'e bağımlılığı olmayan, küçük footprint'li kütüphane gerekiyordu. Hardware crypto accelerator yok (STM32F030).

**Nasıl:**
- `entropy_generate()` → UID 96-bit + ADC iç sensör + SysTick + FNV1a ile seed
- `c25519_prepare()` → bit clamp (Curve25519 zorunluluğu)
- `c25519_smult(pub, base_x, priv)` → public key türetme
- İşlem sonrası `memset(priv, 0, 32)` ile private key temizleme

---

## [KARAR-005] KEY_STORE — Flash Page 15 (Bootloader Alanı)

**Karar:** Kalıcı AES master key, bootloader alanının son sayfası olan page 15'te (0x08007800) saklanır.

**Neden:** Firmware güncelleme sırasında yalnızca sayfa 16–126 silinir. Page 15, bootloader ile birlikte asla silinmez. Bu sayede master key güncellemeler boyunca korunur.

---

## [KARAR-006] Resume Bitmap — Halfword Flash Yazımı

**Karar:** Resume bitmap'i, sayfa tamamlandığında 0xFFFF'ten 0x0000'a halfword yazımı ile güncellenir.

**Neden:** STM32F030 Flash'ta 1→0 yazımı silmeden yapılabilir. 0→1 için silme gerekir. Bu özellik kullanılarak boot flag sayfasında ayrı silme işlemi yapmadan bitmap güncelleniyor.

---

## [KARAR-007] SYSCFG MEM_MODE=11 — SRAM Vektör Tablosu Remapping

**Karar:** STM32F030'da VTOR register'ı yok. Uygulama atlama sırasında vektör tablosu Flash'tan RAM'e kopyalanıp SYSCFG ile remapping yapılır.

**Neden:** STM32F030 (Cortex-M0) VTOR desteklemez. Uygulama kendi vektör tablosuna sahip olabilmesi için bu yöntem zorunlu.

**Nasıl:** `jump_to_application()` içinde:
1. APP_ADDRESS'ten ilk 192 byte RAM başına (0x20000000) kopyalanır
2. `SYSCFG->CFGR1 |= MEM_MODE=0x03` (SRAM remap)

---

## [KARAR-008] Gönderici rf_link_sender.c — Kullanılmayan Ölü Kod

**Karar:** `uart_rf_gonderici/Core/Src/rf_link_sender.c` aktif kodda kullanılmıyor. `FirmwareUpdate_Mode()` kendi yerel RF fonksiyonlarını (`sender_rf_link.c`) kullanıyor.

**Neden:** Refactoring sırasında kodun bir kısmı `sender_rf_link.c`'ye taşındı. `rf_link_sender.c` bağımlılıkları kaldırıldı ama dosya silindi değil.

**Sonuç:** Bu dosyaya dokunmamak gerekir; içeriği yanıltıcı olabilir.

---

## [KARAR-009] KEY_UPDATE Protokolü — Ayrı Komut Byte ile Tetikleme

**Karar:** PC her zaman BOOT_ACK'ten sonra 1 byte komut byte gönderir: `0x00` = normal, `0x08` = KEY_UPDATE var.

**Neden:** Metadata'nın ilk byte'ı 0x08 olabilir (firmware_size < 256 ise). Komut byte olmadan KEY_UPDATE ile normal metadata birbirine karışabilirdi.

---

## [KARAR-010] LBT Sadece Göndericide

**Karar:** LBT (Listen Before Talk) sadece gönderici Si4432 sürücüsünde uygulandı; alıcıda yok.

**Neden:** Alıcı bootloader başlatılma hızı kritik. LBT bekleme süresi (max 200ms) alıcı tarafında kabul edilemez gecikme yaratır.
