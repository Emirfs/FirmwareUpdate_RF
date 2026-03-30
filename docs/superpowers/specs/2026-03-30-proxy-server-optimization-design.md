# Proxy Server Optimizasyon Tasarımı

**Tarih:** 2026-03-30
**Kapsam:** `Uploader/firmware_proxy_server.py`, `Uploader/drive_manager.py`
**Durum:** Onaylandı

---

## Genel Bakış

Firmware proxy server'ında altı optimizasyon yapılacak. Değişiklikler iki dosyayla sınırlıdır; dışa açık API değişmez, mevcut istemci (`firmware_proxy_client.py`) ve GUI (`gui_uploader_qt.py`) güncelleme gerektirmez.

---

## 1. Drive Katalog Cache (60s TTL)

**Sorun:** Her `/api/v1/catalog` isteği Google Drive API'ye gidiyor. Eşzamanlı veya sık sorgular API kotasını tüketir.

**Çözüm:**
- `ProxyState`'e `_catalog_cache: Dict[str, Tuple[float, List]]` eklenir.
  - Key: channel adı
  - Value: `(cache_time_float, files_list)`
- `_catalog_lock: threading.Lock()` ile thread-safe erişim sağlanır.
- Yeni `get_catalog(channel)` metodu:
  1. Lock altında cache kontrol et
  2. `(time.time() - cache_time) < 60` ise cache'den döndür
  3. Aksi halde Drive API'yi çağır, sonucu cache'e yaz
- Cache invalidation: channel map reload edildiğinde (`reload()`) tüm cache temizlenir.

---

## 2. Pagination — nextPageToken Takibi

**Sorun:** `drive_manager.py:96`'da `pageSize=100` var ama `nextPageToken` takip edilmiyor. 100'den fazla dosya içeren klasörlerde eksik listeleme oluşur.

**Çözüm:**
- `list_all_files_in_folder` içine `while True` döngüsü eklenir.
- Her iterasyonda `pageToken=page_token` parametresiyle istek atılır.
- Yanıtta `nextPageToken` varsa döngü devam eder, yoksa kırılır.
- Tüm sayfalardaki dosyalar biriktirilir, tek liste döndürülür.

---

## 3. Channel Map Hot-Reload (File Polling, 5s)

**Sorun:** `proxy_channels.json` değişince server restart gerekiyor. Windows'ta SIGHUP çalışmadığından polling seçildi.

**Çözüm:**
- `ProxyState.__init__` içinde dosyanın ilk `mtime` ve `size` değerleri kaydedilir.
- `_start_channel_map_watcher()` ile daemon thread başlatılır (5s sleep döngüsü).
- Thread her döngüde `os.stat(channel_map_file)` çağırır:
  - `st_mtime` veya `st_size` değiştiyse `reload()` çağırır ve snapshot güncellenir.
  - `FileNotFoundError` sessizce geçilir (dosya geçici olarak silinmiş olabilir).
- Thread daemon=True; server kapanınca otomatik ölür.

---

## 4. Bare `except` Düzeltme

**Sorun:** `drive_manager.py:152`'de `except:` ile `KeyboardInterrupt`, `SystemExit` gibi sinyaller de yakalanıyor.

**Çözüm:** `except:` → `except Exception:`

---

## 5. Download Chunk Yazma

**Sorun:** `_send_binary` içinde `wfile.write(data)` tüm içeriği tek seferde ağa göndermeye çalışır. Büyük dosyalarda socket buffer'ı dolabilir.

**Çözüm:**
- `_send_binary` içinde 8192 byte'lık `CHUNK_SIZE` sabiti kullanılır.
- `while True` döngüsü ile `data.read(CHUNK_SIZE)` okuma yapılır, `wfile.write(chunk)` ile gönderilir.
- `data` parametresi `bytes` yerine `io.BytesIO` olarak alınır (zaten öyle kullanılıyor).

---

## 6. Rate Limiting — Hatalı Key Koruması

**Sorun:** API key brute-force'a karşı koruma yok.

**Çözüm:**
- `ProxyState`'e `_auth_fails: Dict[str, Tuple[int, float]]` eklenir.
  - Key: istemci IP string'i
  - Value: `(fail_count, window_start_time)`
- `_auth_lock: threading.Lock()` ile thread-safe erişim.
- Sabitler: `_AUTH_FAIL_MAX = 10`, `_AUTH_FAIL_WINDOW = 60.0`, `_AUTH_BLOCK_TTL = 60.0`
- `_authorized()` metodu `(authorized: bool, blocked: bool)` tuple döndürür.
- `FirmwareProxyHandler.do_GET` içinde blocked=True ise `429` döndürür.
- Başarılı auth'ta o IP'nin sayacı sıfırlanır.
- Periyodik temizleme: her successful request'te eski kayıtlar (>120s) silinir.

---

## Etkilenen Dosyalar

| Dosya | Değişiklik |
|-------|-----------|
| `Uploader/firmware_proxy_server.py` | Cache, hot-reload, rate limiting, chunk yazma |
| `Uploader/drive_manager.py` | Pagination, bare except fix |

## Etkilenmeyen Dosyalar

`firmware_proxy_client.py`, `gui_uploader_qt.py`, `drive_manager.py` (diğer metodlar), `config_manager.py`

---

## Kısıtlar

- Windows ortamı — SIGHUP yok, polling seçildi
- STM32 firmware dosyaları küçük (<256KB) — streaming zorunlu değil, önlem amaçlı
- Mevcut HTTP API değişmez — istemci güncellemesi gerekmez
