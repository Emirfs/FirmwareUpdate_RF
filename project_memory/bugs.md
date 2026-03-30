# Bug Kayıtları — FirmwareUpdate_RF

> Çözülen hatalar ve kök neden analizleri. 2026-03-27 tarihinde oluşturuldu.

---

## [BUG-001] ✅ Kanal Kayması — BOOT_ACK Kaçırılıyor

**Semptom:** Alıcı sarı LED sonsuz yanıp sönüyor (stuck in `while(!got_metadata)`). Gönderici 30 saniye sonra UART_NACK gönderiyor ("ACK gelmedi! Gelen: 15").

**Kök Neden:** Frekans atlama (frequency hopping) aktifken gönderici ve alıcı farklı kanallara geçiyordu. Gönderici BOOT_REQUEST gönderirken alıcı başka kanalı dinliyordu.

**Çözüm:**
1. `rf_link_stm32.c` ve `rf_link_sender.c`'den tüm `rf_link_hop_channel()` kaldırıldı.
2. Her iki cihaz da sabit kanal 0 kullanıyor (Si4432 reg 0x79 = 0x00).
3. `boot_rf_update.c`'de `while(!got_metadata)` döngüsüne kanal sıfırlama eklendi: `rf_link_set_channel_index(0U)`.

**Dosyalar:** `rf_link_stm32.c`, `rf_link_sender.c`, `boot_rf_update.c`

---

## [BUG-002] ✅ BOOT_ACK Zamanlama — Periyodik Broadcast Yetersiz

**Semptom:** Bazen BOOT_ACK alınıyor, bazen alınmıyor. Güvenilmez el sıkışma.

**Kök Neden:** Alıcı ~1200ms döngü başında bir kez BOOT_ACK gönderiyordu. Gönderici 2000ms bekleme penceresi açıyordu. İki periyot örtüşmeyince BOOT_ACK kaçırılıyordu.

**Çözüm:** `boot_flow.c` içindeki `while(!got_metadata)` döngüsüne reaktif yanıt eklendi:
```c
else if (rx_type == RF_CMD_BOOT_REQUEST) {
    RF_SendPacket(RF_CMD_BOOT_ACK, rf_seq_counter++, boot_ack_pld, BOOT_ACK_PLD_SIZE);
}
```
Artık alıcı BOOT_REQUEST aldığında anında BOOT_ACK ile yanıt veriyor.

**Dosya:** `alici_cihaz/Core/Src/boot_flow.c`

---

## [BUG-003] ✅ Flash Silme Uzun Bekleme — FLASH_ERASE_DONE Gecikmesi

**Semptom:** Metadata sonrası gönderici "Flash silme zaman aşımı" hatası alıyordu.

**Kök Neden:** Tüm uygulama alanı (111 sayfa × ~5ms = ~555ms) FLASH_ERASE_DONE gönderilmeden önce siliniyordu. Gönderici tarafındaki bekleme penceresi bu süreye yetmiyordu.

**Çözüm:** Flash silme işlemi sayfa bazlı yapıldı. `Flash_Erase_Application()` yerine her 2KB sayfa başında `Flash_Erase_Page(current_addr)` çağrılıyor. FLASH_ERASE_DONE artık metadata ACK'inden hemen sonra gönderiliyor.

**Dosya:** `alici_cihaz/Core/Src/boot_flow.c`, `boot_storage.c`

---

## [BUG-004] ✅ rf_link_sender.c Ölü Kod

**Semptom:** `uart_rf_gonderici`'de `rf_link_sender.c` var ama `FirmwareUpdate_Mode()` içinden çağrılmıyor.

**Kök Neden:** Refactoring sırasında RF fonksiyonları `sender_rf_link.c`'ye taşındı. `rf_link_sender.c`'nin bağımlılıkları kaldırıldı ama dosya silindi değil.

**Durum:** Ölü kod olarak bırakıldı (silerek sorun çıkarma riski var). Yanıltıcı olduğu için `decisions.md`'ye not eklendi.

---

## Aktif Bilinen Sorunlar

(Şu an bilinen açık hata yok.)
