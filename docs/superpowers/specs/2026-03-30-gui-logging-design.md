# GUI Operation Logger + Clear Button Tasarımı

**Tarih:** 2026-03-30
**Kapsam:** `Uploader/gui_logger.py` (yeni), `Uploader/gui_uploader_qt.py` (değişen)
**Durum:** Onaylandı

---

## Genel Bakış

GUI işlemlerini yapılandırılmış bloklar halinde `gui.log` dosyasına kaydeder. Her işlem başlık, detaylar ve BASARILI/HATA sonucunu içerir. Dosya birikimli (append), içinde tarih separator'ları ile bölünür. Ek olarak ana log widget'ına "Logu Temizle" butonu eklenir.

---

## Dosya Yapısı

| Dosya | Değişiklik |
|-------|-----------|
| `Uploader/gui_logger.py` | Yeni — `GUILogger` sınıfı |
| `Uploader/gui_uploader_qt.py` | `GUILogger` entegrasyonu + clear button |

---

## 1. `GUILogger` Sınıfı (`gui_logger.py`)

### Sorumluluk
- `gui.log` dosyasına append modunda yazar
- Tarih değişince `=== YYYY-MM-DD ===` separator ekler (günde bir kez)
- Her işlemi hizalanmış blok formatında yazar
- Thread-safe (`threading.Lock`)

### API

```python
class GUILogger:
    def __init__(self, log_path: str) -> None:
        """log_path: gui.log'un tam yolu. Yoksa oluşturur."""

    def log_operation(
        self,
        title: str,
        details: Dict[str, str],
        success: bool,
        error: Optional[str] = None,
    ) -> None:
        """
        Dosyaya tek bir işlem bloğu yazar.
        title   : İşlem başlığı (ör. "Firmware Yükleme")
        details : Sıralı anahtar-değer dict (ör. {"Cihaz": "DeviceA", ...})
        success : True = BASARILI, False = HATA
        error   : Hata mesajı (success=False ise eklenir)
        """
```

### Dosya Formatı

```
=== 2026-03-30 ===

[10:15:22] Proxy Başlatma
  Adres         : http://127.0.0.1:8787
  Servis JSON   : C:/path/service_account.json
  Kanal haritası: C:/path/channels.json
  Token TTL     : 120 sn
  Sonuç         : BASARILI

[10:22:33] Firmware Yükleme
  Cihaz         : DeviceA
  Kanal         : stm32-test
  Firmware      : update_v5.bin
  Tür           : BIN
  Versiyon      : v5
  Boyut         : 48320 byte
  Mod           : RF
  COM Port      : COM7
  Baud          : 115200
  Paket boyutu  : 64 byte
  Süre          : 34.2 sn
  Sonuç         : HATA
  Hata          : Zaman aşımı — 30s içinde BOOT_ACK gelmedi

[10:45:11] Katalog Sorgusu
  Kanal         : stm32-test
  Proxy         : http://127.0.0.1:8787
  Dosyalar      : update_v5.bin (BIN, 48320 byte, v5)
                  update_v4.bin (BIN, 46080 byte, v4)
                  bootloader_v2.hex (HEX, 12288 byte)
  Bulunan       : 3 dosya
  Sonuç         : BASARILI
```

### Hizalama Kuralı

- `details` dict'indeki key'lerin maksimum uzunluğuna göre sağa hizalı iki nokta sütunu
- Çok satırlı değer (dosya listesi): ikinci satırdan itibaren ilk satırın değer başlangıç sütununa hizalanır
- `Sonuç` her zaman son satırdan önce, `Hata` en sonda

### Thread Safety

`threading.Lock` ile `log_operation` çağrıları serialize edilir. GUI upload thread'inden de güvenli çağrılabilir.

---

## 2. Loglanan İşlemler (8 adet)

### 2.1 Config Yükleme
**Tetikleyici:** `_try_load_config()` sonrası
**Başarı durumu:** Config yüklendi
**Hata durumu:** Config yok veya şifreli

```
[HH:MM:SS] Config Yükleme
  Durum     : Yuklendi / Dosya yok / Sifreli
  Cihaz sayısı: 3
  Proxy     : Yapilandirilmis / Tanimli degil
  Sonuç     : BASARILI / HATA
```

### 2.2 Proxy Başlatma
**Tetikleyici:** `_start_local_proxy_server()` çağrısı öncesi/sonrası
**Mevcut `_append_log` çağrısı:** satır 1785 ("Yerel proxy baslatildi")

```
[HH:MM:SS] Proxy Başlatma
  Adres         : http://127.0.0.1:8787
  Servis JSON   : /path/service_account.json
  Kanal haritası: /path/channels.json
  Token TTL     : 120 sn
  Sonuç         : BASARILI / HATA
  Hata          : <exc mesajı>   (sadece HATA durumunda)
```

### 2.3 Proxy Durdurma
**Tetikleyici:** `_stop_local_proxy_server()` çağrısı
**Mevcut `_append_log` çağrısı:** satır 1810 ("Yerel proxy durduruldu")

```
[HH:MM:SS] Proxy Durdurma
  Adres     : http://127.0.0.1:8787
  Sonuç     : BASARILI / HATA
  Hata      : <exc mesajı>   (sadece HATA durumunda)
```

### 2.4 Proxy Bağlantı Testi
**Tetikleyici:** `_test_proxy_connection()` çağrısı
**Mevcut `_append_log` çağrısı:** satır 1903 ("Proxy testi basarili")

```
[HH:MM:SS] Proxy Bağlantı Testi
  Adres     : http://127.0.0.1:8787
  Sunucu TS : 1774855176
  Sonuç     : BASARILI / HATA
  Hata      : <error>   (sadece HATA durumunda)
```

### 2.5 Katalog Sorgusu
**Tetikleyici:** `_on_scan_finished()` — satır 2056 ("Klasor tarandi")
**Hata durumu:** satır 2035 (error string)

```
[HH:MM:SS] Katalog Sorgusu
  Kanal     : stm32-test
  Proxy     : http://127.0.0.1:8787
  Dosyalar  : update_v5.bin (BIN, 48320 byte, v5)
              update_v4.bin (BIN, 46080 byte, v4)
  Bulunan   : 2 dosya
  Sonuç     : BASARILI / HATA
  Hata      : <error>   (sadece HATA durumunda)
```

Dosya listesi boşsa: `Bulunan: 0 dosya`, dosyalar satırı atlanır.

### 2.6 Firmware Yükleme
**Tetikleyici:** Upload başlarken (`_start_upload`, satır 2166 civarı) ve biterken (`_on_upload_finished`)
**Strateji:** Upload başında `_pending_upload_log` dict'i oluştur, `_upload_start_time` kaydet; `_on_upload_finished`'da tamamla ve yaz.

```
[HH:MM:SS] Firmware Yükleme
  Cihaz         : DeviceA
  Kanal         : stm32-test
  Firmware      : update_v5.bin
  Tür           : BIN
  Versiyon      : v5
  Boyut         : 48320 byte
  Mod           : RF / Seri
  COM Port      : COM7
  Baud          : 115200
  Paket boyutu  : 64 byte
  Süre          : 34.2 sn
  Sonuç         : BASARILI / DURDURULDU / HATA
  Hata          : <error>   (sadece HATA durumunda)
```

`success=True` → BASARILI
`stop_requested=True, success=False` → DURDURULDU
`success=False` → HATA
Hata string'i `_on_upload_finished` parametresinde yok — bu yüzden `_upload_last_error: Optional[str]` instance değişkeni eklenir; `signals.log` ile gelen son hata satırı yakalanır (upload sırasındaki `log=lambda msg: self.signals.log.emit(...)` callback'inden).

### 2.7 AES Key Güncelleme
**Tetikleyici:** `_on_key_update_finished()` — satır 2762 ("Cihaz AES key guncellendi")

```
[HH:MM:SS] AES Key Güncelleme
  Cihaz     : DeviceA
  Sonuç     : BASARILI / HATA
  Hata      : <error>   (sadece HATA durumunda)
```

### 2.8 COM Port Tarama
**Tetikleyici:** `_on_scan_ports_finished()` veya port scan tamamlama — satır 1955

```
[HH:MM:SS] COM Port Tarama
  Bulunan   : COM3, COM7
  Sonuç     : BASARILI / HATA
  Hata      : pyserial bulunamadi   (sadece HATA durumunda)
```

Bulunan port yoksa: `Bulunan: yok`

---

## 3. Clear Button

### Yerleşim
`gui_uploader_qt.py` içinde `_w(QPlainTextEdit, "logTextEdit")` widget'ının bulunduğu parent layout'a dinamik olarak `QPushButton("Logu Temizle")` eklenir.

Buton `self.log_text`'in parent widget'ı (`log_text.parent()`) üzerinden layout'a eklenir; `.ui` dosyasına dokunulmaz.

### Davranış
- Tıklanınca: `self.log_text.clear()` — sadece ekrandaki görünümü temizler
- `gui.log` dosyasına dokunmaz, dosya birikmeye devam eder
- Buton her zaman aktif (upload sırasında da temizlenebilir)

### Stil
Mevcut "subtle" buton stilini kullanır (diğer secondary butonlarla tutarlı).

---

## 4. `GUILogger` Başlatma

`gui_uploader_qt.py` `__init__` içinde:

```python
_log_path = os.path.join(os.path.dirname(__file__), "gui.log")
self.gui_logger = GUILogger(_log_path)
```

---

## Kısıtlar

- `.ui` dosyasına dokunulmaz — tüm widget değişiklikleri Python'da yapılır
- `gui.log` dosyası büyüyebilir; bu tasarımda rotation yok (kapsam dışı)
- Upload hata mesajı `signals.log` callback'inden capture edilir — son satır yeterli
- Mevcut `_append_log` çağrıları kaldırılmaz; `log_operation` onlara ek olarak çağrılır
