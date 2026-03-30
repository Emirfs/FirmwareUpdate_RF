# Proxy Server Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Proxy server'a katalog cache, pagination, hot-reload, rate limiting, chunk download ve bare-except düzeltmesi ekle.

**Architecture:** İki dosya değişir (`firmware_proxy_server.py`, `drive_manager.py`). Dışa açık HTTP API değişmez. Yeni özellikler `ProxyState` sınıfına eklenir, handler katmanı minimal değişir. Thread-safety tüm paylaşımlı state için `threading.Lock()` ile sağlanır.

**Tech Stack:** Python 3.x, `threading`, `unittest.mock`, `pytest`

---

## Dosya Haritası

| Dosya | Değişiklik |
|-------|-----------|
| `Uploader/firmware_proxy_server.py` | `threading` import, `List` import, catalog cache, hot-reload watcher, rate limiter, chunk writer, `_check_auth` |
| `Uploader/drive_manager.py` | Pagination loop, bare `except` fix |
| `Uploader/tests/__init__.py` | Yeni — test paketi |
| `Uploader/tests/test_drive_manager.py` | Yeni — pagination ve except testleri |
| `Uploader/tests/test_proxy_server.py` | Yeni — cache, rate limit, hot-reload testleri |

---

## Task 1: Test altyapısını kur

**Files:**
- Create: `Uploader/tests/__init__.py`

- [ ] **Step 1: Dizini ve boş `__init__.py` dosyasını oluştur**

```bash
mkdir -p Uploader/tests
touch Uploader/tests/__init__.py
```

- [ ] **Step 2: pytest'in çalıştığını doğrula**

```bash
cd Uploader && python -m pytest tests/ -v
```

Expected: `no tests ran` veya `0 passed`

- [ ] **Step 3: Commit**

```bash
git add Uploader/tests/__init__.py
git commit -m "test: add tests package scaffold"
```

---

## Task 2: Drive pagination — failing test yaz

**Files:**
- Create: `Uploader/tests/test_drive_manager.py`
- Modify: `Uploader/drive_manager.py`

- [ ] **Step 1: Failing test yaz**

`Uploader/tests/test_drive_manager.py` dosyasını oluştur:

```python
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from unittest.mock import MagicMock, patch, call
from drive_manager import DriveManager


def _make_service(pages):
    """pages: list of lists — her eleman bir sayfa dosya listesi."""
    service = MagicMock()
    files_resource = service.files.return_value
    list_resource = files_resource.list.return_value

    responses = []
    for i, page_files in enumerate(pages):
        resp = {"files": page_files}
        if i < len(pages) - 1:
            resp["nextPageToken"] = f"token_{i+1}"
        responses.append(resp)

    list_resource.execute.side_effect = responses
    return service


def test_list_all_files_single_page():
    """Tek sayfa — nextPageToken yok."""
    page1 = [{"id": "a", "name": "update_1.bin", "size": "1024"}]
    dm = DriveManager.__new__(DriveManager)
    dm.service = _make_service([page1])

    files, err = dm.list_all_files_in_folder("folder_xyz")

    assert err is None
    assert len(files) == 1
    assert files[0]["id"] == "a"


def test_list_all_files_multiple_pages():
    """İki sayfa — ikinci sayfadaki dosyalar da gelir."""
    page1 = [{"id": "a", "name": "update_1.bin", "size": "100"}]
    page2 = [{"id": "b", "name": "update_2.bin", "size": "200"}]
    dm = DriveManager.__new__(DriveManager)
    dm.service = _make_service([page1, page2])

    files, err = dm.list_all_files_in_folder("folder_xyz")

    assert err is None
    assert len(files) == 2
    ids = {f["id"] for f in files}
    assert ids == {"a", "b"}


def test_list_all_files_skips_non_firmware():
    """Sadece .bin ve .hex dosyaları döner."""
    page1 = [
        {"id": "a", "name": "update_1.bin", "size": "100"},
        {"id": "b", "name": "readme.txt", "size": "50"},
        {"id": "c", "name": "update_2.hex", "size": "200"},
    ]
    dm = DriveManager.__new__(DriveManager)
    dm.service = _make_service([page1])

    files, err = dm.list_all_files_in_folder("folder_xyz")

    assert len(files) == 2
    names = {f["name"] for f in files}
    assert names == {"update_1.bin", "update_2.hex"}
```

- [ ] **Step 2: Testi çalıştır, başarısız olduğunu doğrula**

```bash
cd Uploader && python -m pytest tests/test_drive_manager.py -v
```

Expected: `test_list_all_files_multiple_pages` FAILS — ikinci sayfadaki dosya gelmiyor çünkü pagination yok.

---

## Task 3: Drive pagination — implementasyon

**Files:**
- Modify: `Uploader/drive_manager.py:91-125`

- [ ] **Step 1: `list_all_files_in_folder` içinde pagination döngüsü ekle**

`drive_manager.py` dosyasında `list_all_files_in_folder` metodunu bul (satır 80). `try` bloğu içindeki ilk `results = ...` ve `files = ...` satırlarını aşağıdaki kodla değiştir:

```python
        try:
            query = f"'{folder_id}' in parents and trashed = false and mimeType != 'application/vnd.google-apps.folder'"
            all_raw_files = []
            page_token = None
            while True:
                kwargs: Dict[str, Any] = {
                    "q": query,
                    "fields": "nextPageToken, files(id, name, size)",
                    "pageSize": 100,
                }
                if page_token:
                    kwargs["pageToken"] = page_token
                results = self.service.files().list(**kwargs).execute()
                all_raw_files.extend(results.get("files", []))
                page_token = results.get("nextPageToken")
                if not page_token:
                    break

            firmware_files = []
            for file in all_raw_files:
```

> Not: `for file in all_raw_files:` satırından itibaren mevcut kod devam eder — sadece `for file in files:` → `for file in all_raw_files:` değişir, geri kalan kod aynı kalır.

Ayrıca dosyanın üst kısmındaki `from typing import` satırına `Dict, Any` ekle (eğer yoksa):

```python
from typing import Any, Dict, Optional, Tuple
```

- [ ] **Step 2: Testleri çalıştır, geçtiğini doğrula**

```bash
cd Uploader && python -m pytest tests/test_drive_manager.py -v
```

Expected: `3 passed`

- [ ] **Step 3: Commit**

```bash
git add Uploader/drive_manager.py Uploader/tests/test_drive_manager.py
git commit -m "fix: add nextPageToken pagination in list_all_files_in_folder"
```

---

## Task 4: Bare `except` düzeltme

**Files:**
- Modify: `Uploader/drive_manager.py:152`

- [ ] **Step 1: Failing test ekle**

`test_drive_manager.py` dosyasına aşağıdaki testi ekle:

```python
def test_check_updates_bare_except_is_exception():
    """KeyboardInterrupt gibi sinyaller check_updates_in_folder'dan kaçmamalı;
    bare except yerine Exception kullanılmalı."""
    import inspect, ast, textwrap
    import drive_manager as dm_module
    src = inspect.getsource(dm_module.DriveManager.check_updates_in_folder)
    # AST ile bare except kontrolü
    tree = ast.parse(textwrap.dedent(src))
    for node in ast.walk(tree):
        if isinstance(node, ast.ExceptHandler) and node.type is None:
            raise AssertionError(
                "check_updates_in_folder içinde 'except:' (bare) bulundu. "
                "'except Exception:' olmalı."
            )
```

- [ ] **Step 2: Testi çalıştır, başarısız olduğunu doğrula**

```bash
cd Uploader && python -m pytest tests/test_drive_manager.py::test_check_updates_bare_except_is_exception -v
```

Expected: FAIL — `bare except` bulundu

- [ ] **Step 3: `drive_manager.py:152` düzelt**

`check_updates_in_folder` metodunda `except:` satırını bul ve değiştir:

```python
                except Exception:
                    continue
```

- [ ] **Step 4: Testi çalıştır, geçtiğini doğrula**

```bash
cd Uploader && python -m pytest tests/test_drive_manager.py -v
```

Expected: `4 passed`

- [ ] **Step 5: Commit**

```bash
git add Uploader/drive_manager.py Uploader/tests/test_drive_manager.py
git commit -m "fix: replace bare except with except Exception in check_updates_in_folder"
```

---

## Task 5: Katalog cache (60s TTL) — failing test yaz

**Files:**
- Create: `Uploader/tests/test_proxy_server.py`

- [ ] **Step 1: Test dosyasını oluştur**

`Uploader/tests/test_proxy_server.py`:

```python
import sys
import os
import time
import threading
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from unittest.mock import MagicMock, patch


def _make_proxy_state(api_key="testkey", channel_map=None, token_ttl=120):
    """ProxyState oluşturur; Drive ve dosya sistemi mock'lanır."""
    from firmware_proxy_server import ProxyState

    if channel_map is None:
        channel_map = {"test-channel": "folder_abc"}

    with patch("firmware_proxy_server.DriveManager"), \
         patch("builtins.open", create=True), \
         patch("os.path.exists", return_value=True), \
         patch("json.load", return_value=channel_map):
        state = ProxyState.__new__(ProxyState)
        state.api_key = api_key
        state.token_ttl = token_ttl
        state.channel_map = channel_map
        state.channel_map_file = "fake_channels.json"
        state._catalog_cache = {}
        state._catalog_lock = threading.Lock()
        state._auth_fails = {}
        state._auth_lock = threading.Lock()
        # Mock drive
        state.drive = MagicMock()
        return state


def test_catalog_cache_hit_skips_drive():
    """İkinci çağrı Drive API'ye gitmez."""
    state = _make_proxy_state()
    fake_files = [{"id": "f1", "name": "update_1.bin", "version": 1, "type": "BIN", "size": "100"}]
    state.drive.list_all_files_in_folder.return_value = (fake_files, None)

    # İlk çağrı — Drive çağrılır
    files1, err1 = state.get_catalog("test-channel")
    assert err1 is None
    assert files1 == fake_files
    assert state.drive.list_all_files_in_folder.call_count == 1

    # İkinci çağrı — cache'den gelmeli, Drive tekrar çağrılmamalı
    files2, err2 = state.get_catalog("test-channel")
    assert err2 is None
    assert files2 == fake_files
    assert state.drive.list_all_files_in_folder.call_count == 1  # hâlâ 1


def test_catalog_cache_miss_after_ttl():
    """TTL geçince Drive tekrar çağrılır."""
    state = _make_proxy_state()
    fake_files = [{"id": "f1", "name": "update_1.bin", "version": 1, "type": "BIN", "size": "100"}]
    state.drive.list_all_files_in_folder.return_value = (fake_files, None)

    files1, _ = state.get_catalog("test-channel")
    assert state.drive.list_all_files_in_folder.call_count == 1

    # Cache zamanını manuel olarak geçmişe al
    state._catalog_cache["test-channel"] = (time.time() - 61, fake_files)

    files2, _ = state.get_catalog("test-channel")
    assert state.drive.list_all_files_in_folder.call_count == 2


def test_catalog_cache_cleared_on_reload():
    """reload() sonrası cache boşalır."""
    state = _make_proxy_state()
    fake_files = [{"id": "f1", "name": "update_1.bin", "version": 1, "type": "BIN", "size": "100"}]
    state.drive.list_all_files_in_folder.return_value = (fake_files, None)

    state.get_catalog("test-channel")
    assert "test-channel" in state._catalog_cache

    with patch.object(state, "_load_channel_map", return_value={"test-channel": "folder_abc"}):
        state.reload()

    assert state._catalog_cache == {}
```

- [ ] **Step 2: Testi çalıştır, başarısız olduğunu doğrula**

```bash
cd Uploader && python -m pytest tests/test_proxy_server.py::test_catalog_cache_hit_skips_drive -v
```

Expected: FAIL — `ProxyState` nesnesi `get_catalog` metoduna sahip değil

---

## Task 6: Katalog cache — implementasyon

**Files:**
- Modify: `Uploader/firmware_proxy_server.py`

- [ ] **Step 1: `import threading` ve `List` tipini ekle**

`firmware_proxy_server.py` dosyasının en üstündeki import bloğunu bul ve şu iki satırı ekle:

```python
import threading
```

ve `from typing import ...` satırını güncelle:

```python
from typing import Any, Dict, List, Optional, Tuple
```

- [ ] **Step 2: `ProxyState.__init__` içine cache state ekle**

`ProxyState.__init__` metodunda `self.channel_map = self._load_channel_map()` satırından sonra şunu ekle:

```python
        self._catalog_cache: Dict[str, Tuple[float, List]] = {}
        self._catalog_lock = threading.Lock()
        self._CATALOG_TTL = 60.0
```

- [ ] **Step 3: `reload()` metodunu güncelle — cache temizleme**

Mevcut `reload()`:
```python
    def reload(self) -> None:
        self.channel_map = self._load_channel_map()
```

Şu hale getir:
```python
    def reload(self) -> None:
        self.channel_map = self._load_channel_map()
        with self._catalog_lock:
            self._catalog_cache.clear()
```

- [ ] **Step 4: `get_catalog` metodunu ekle**

`resolve_folder_id` metodundan sonra şu metodu ekle:

```python
    def get_catalog(self, channel: str) -> Tuple[Optional[List], Optional[str]]:
        folder_id = self.resolve_folder_id(channel)
        if not folder_id:
            return None, "kanal bulunamadi"

        now = time.time()
        with self._catalog_lock:
            cached = self._catalog_cache.get(channel)
            if cached is not None and (now - cached[0]) < self._CATALOG_TTL:
                return cached[1], None

        files, error = self.drive.list_all_files_in_folder(folder_id)
        if files is None:
            return None, error

        with self._catalog_lock:
            self._catalog_cache[channel] = (time.time(), files)

        return files, error
```

- [ ] **Step 5: `/catalog` endpoint'ini `get_catalog` kullanacak şekilde güncelle**

`do_GET` içindeki `/api/v1/catalog` bloğunu bul. Şu satırları:

```python
            folder_id = self.state.resolve_folder_id(channel)
            if not folder_id:
                _send_json(self, 404, {"error": "kanal bulunamadi"})
                return

            files, error = self.state.drive.list_all_files_in_folder(folder_id)
            if files is None:
                _send_json(self, 502, {"error": error or "drive catalog hatasi"})
                return
```

Şu hale getir:

```python
            files, error = self.state.get_catalog(channel)
            if files is None:
                status_code = 404 if error == "kanal bulunamadi" else 502
                _send_json(self, status_code, {"error": error or "drive catalog hatasi"})
                return
```

- [ ] **Step 6: Testleri çalıştır**

```bash
cd Uploader && python -m pytest tests/test_proxy_server.py -v
```

Expected: `3 passed`

- [ ] **Step 7: Commit**

```bash
git add Uploader/firmware_proxy_server.py Uploader/tests/test_proxy_server.py
git commit -m "feat: add 60s TTL catalog cache to ProxyState"
```

---

## Task 7: Channel map hot-reload (5s polling) — test ve implementasyon

**Files:**
- Modify: `Uploader/firmware_proxy_server.py`
- Modify: `Uploader/tests/test_proxy_server.py`

- [ ] **Step 1: Test ekle**

`test_proxy_server.py` dosyasına şunu ekle:

```python
def test_channel_map_reload_on_file_change():
    """_channel_map_poll_loop mtime değişince reload çağırır."""
    state = _make_proxy_state()
    state._channel_map_stat = (1000.0, 100)

    reload_called = []
    original_reload = state.reload

    def mock_reload():
        reload_called.append(True)
        state._catalog_cache.clear()
        state.channel_map = {"test-channel": "folder_abc"}

    state.reload = mock_reload

    # Farklı mtime/size simüle et
    new_stat = MagicMock()
    new_stat.st_mtime = 2000.0
    new_stat.st_size = 200

    with patch("os.stat", return_value=new_stat):
        state._poll_channel_map_once()

    assert len(reload_called) == 1
    assert state._channel_map_stat == (2000.0, 200)


def test_channel_map_no_reload_if_unchanged():
    """mtime ve size değişmediyse reload çağrılmaz."""
    state = _make_proxy_state()
    state._channel_map_stat = (1000.0, 100)

    reload_called = []
    state.reload = lambda: reload_called.append(True)

    unchanged_stat = MagicMock()
    unchanged_stat.st_mtime = 1000.0
    unchanged_stat.st_size = 100

    with patch("os.stat", return_value=unchanged_stat):
        state._poll_channel_map_once()

    assert len(reload_called) == 0
```

- [ ] **Step 2: Testi çalıştır, başarısız olduğunu doğrula**

```bash
cd Uploader && python -m pytest tests/test_proxy_server.py::test_channel_map_reload_on_file_change -v
```

Expected: FAIL — `_poll_channel_map_once` metodu yok

- [ ] **Step 3: `ProxyState.__init__`'e watcher state ekle**

`self._CATALOG_TTL = 60.0` satırından sonra şunu ekle:

```python
        self._channel_map_stat: Tuple[float, int] = self._stat_channel_map()
        self._start_channel_map_watcher()
```

- [ ] **Step 4: Üç yeni metod ekle**

`reload()` metodundan sonra şu üç metodu ekle:

```python
    def _stat_channel_map(self) -> Tuple[float, int]:
        try:
            s = os.stat(self.channel_map_file)
            return s.st_mtime, s.st_size
        except OSError:
            return 0.0, 0

    def _poll_channel_map_once(self) -> None:
        current = self._stat_channel_map()
        if current != self._channel_map_stat:
            self.reload()
            self._channel_map_stat = current

    def _channel_map_poll_loop(self) -> None:
        while True:
            time.sleep(5)
            try:
                self._poll_channel_map_once()
            except Exception:
                pass

    def _start_channel_map_watcher(self) -> None:
        t = threading.Thread(target=self._channel_map_poll_loop, daemon=True)
        t.start()
```

- [ ] **Step 5: Testleri çalıştır**

```bash
cd Uploader && python -m pytest tests/test_proxy_server.py -v
```

Expected: `5 passed`

- [ ] **Step 6: Commit**

```bash
git add Uploader/firmware_proxy_server.py Uploader/tests/test_proxy_server.py
git commit -m "feat: add channel map hot-reload via 5s file polling"
```

---

## Task 8: Download chunk writing — test ve implementasyon

**Files:**
- Modify: `Uploader/firmware_proxy_server.py`
- Modify: `Uploader/tests/test_proxy_server.py`

- [ ] **Step 1: Test ekle**

`test_proxy_server.py` dosyasına şunu ekle:

```python
def test_send_binary_chunks():
    """_send_binary BytesIO'yu 8KB chunk'larla gönderir."""
    import io
    from firmware_proxy_server import _send_binary

    # 20KB veri — en az 3 chunk gerekir
    data = io.BytesIO(b"x" * (8192 * 2 + 512))

    written_chunks = []
    handler = MagicMock()
    handler.wfile.write.side_effect = lambda chunk: written_chunks.append(len(chunk))

    _send_binary(handler, "test.bin", data)

    assert len(written_chunks) == 3
    assert written_chunks[0] == 8192
    assert written_chunks[1] == 8192
    assert written_chunks[2] == 512
    # Content-Length doğru gönderildi
    content_length_calls = [
        c for c in handler.send_header.call_args_list
        if c[0][0] == "Content-Length"
    ]
    assert content_length_calls[0][0][1] == str(8192 * 2 + 512)
```

- [ ] **Step 2: Testi çalıştır, başarısız olduğunu doğrula**

```bash
cd Uploader && python -m pytest tests/test_proxy_server.py::test_send_binary_chunks -v
```

Expected: FAIL — `_send_binary` `bytes` alıyor, `BytesIO` değil

- [ ] **Step 3: `_CHUNK_SIZE` sabiti ekle**

`firmware_proxy_server.py` dosyasında `_send_json` fonksiyonundan önce şunu ekle:

```python
_CHUNK_SIZE = 8192
```

- [ ] **Step 4: `_send_binary` fonksiyonunu güncelle**

Mevcut `_send_binary`:

```python
def _send_binary(handler: BaseHTTPRequestHandler, name: str, data: bytes) -> None:
    handler.send_response(200)
    handler.send_header("Content-Type", "application/octet-stream")
    quoted_name = urllib.parse.quote(name)
    handler.send_header("Content-Disposition", f"attachment; filename*=UTF-8''{quoted_name}")
    handler.send_header("Content-Length", str(len(data)))
    handler.end_headers()
    handler.wfile.write(data)
```

Yeni hali:

```python
def _send_binary(handler: BaseHTTPRequestHandler, name: str, data: "io.BytesIO") -> None:
    start = data.tell()
    data.seek(0, 2)
    total = data.tell() - start
    data.seek(start)

    handler.send_response(200)
    handler.send_header("Content-Type", "application/octet-stream")
    quoted_name = urllib.parse.quote(name)
    handler.send_header("Content-Disposition", f"attachment; filename*=UTF-8''{quoted_name}")
    handler.send_header("Content-Length", str(total))
    handler.end_headers()
    while True:
        chunk = data.read(_CHUNK_SIZE)
        if not chunk:
            break
        handler.wfile.write(chunk)
```

- [ ] **Step 5: `do_GET` içindeki çağrıyı güncelle**

`do_GET` içinde şu satırı bul:

```python
            _send_binary(self, file_name, file_data.read())
```

Şu hale getir:

```python
            _send_binary(self, file_name, file_data)
```

- [ ] **Step 6: Testleri çalıştır**

```bash
cd Uploader && python -m pytest tests/test_proxy_server.py -v
```

Expected: `6 passed`

- [ ] **Step 7: Commit**

```bash
git add Uploader/firmware_proxy_server.py Uploader/tests/test_proxy_server.py
git commit -m "feat: stream binary downloads in 8KB chunks"
```

---

## Task 9: Rate limiting — test ve implementasyon

**Files:**
- Modify: `Uploader/firmware_proxy_server.py`
- Modify: `Uploader/tests/test_proxy_server.py`

- [ ] **Step 1: Test ekle**

`test_proxy_server.py` dosyasına şunu ekle:

```python
def test_rate_limit_blocks_after_10_fails():
    """10 hatalı denemeden sonra IP bloklanır."""
    state = _make_proxy_state()

    for _ in range(10):
        blocked = state.record_auth_fail("1.2.3.4")

    assert blocked is True
    assert state.is_auth_blocked("1.2.3.4") is True


def test_rate_limit_resets_after_window():
    """60 saniyelik pencere geçince blok kalkar."""
    state = _make_proxy_state()

    for _ in range(10):
        state.record_auth_fail("1.2.3.4")

    # Zamanı geçmişe al
    state._auth_fails["1.2.3.4"] = (10, time.time() - 61)

    assert state.is_auth_blocked("1.2.3.4") is False


def test_rate_limit_clears_on_success():
    """Başarılı auth sonrası hata sayacı sıfırlanır."""
    state = _make_proxy_state()

    for _ in range(5):
        state.record_auth_fail("1.2.3.4")

    state.clear_auth_fail("1.2.3.4")

    assert state.is_auth_blocked("1.2.3.4") is False
    assert "1.2.3.4" not in state._auth_fails
```

- [ ] **Step 2: Testi çalıştır, başarısız olduğunu doğrula**

```bash
cd Uploader && python -m pytest tests/test_proxy_server.py::test_rate_limit_blocks_after_10_fails -v
```

Expected: FAIL — `record_auth_fail` metodu yok

- [ ] **Step 3: `ProxyState.__init__`'e rate limiter state ekle**

`self._CATALOG_TTL = 60.0` satırından sonra şunu ekle:

```python
        self._auth_fails: Dict[str, Tuple[int, float]] = {}
        self._auth_lock = threading.Lock()
        self._AUTH_FAIL_MAX = 10
        self._AUTH_FAIL_WINDOW = 60.0
        self._AUTH_BLOCK_TTL = 60.0
```

- [ ] **Step 4: Üç rate limiter metodu ekle**

`_start_channel_map_watcher` metodundan sonra şu üç metodu ekle:

```python
    def record_auth_fail(self, ip: str) -> bool:
        """Başarısız auth kaydeder. True döndürürse IP bloklanmıştır."""
        now = time.time()
        with self._auth_lock:
            count, window_start = self._auth_fails.get(ip, (0, now))
            if now - window_start > self._AUTH_FAIL_WINDOW:
                count = 0
                window_start = now
            count += 1
            self._auth_fails[ip] = (count, window_start)
            return count > self._AUTH_FAIL_MAX

    def is_auth_blocked(self, ip: str) -> bool:
        now = time.time()
        with self._auth_lock:
            count, window_start = self._auth_fails.get(ip, (0, 0.0))
            if now - window_start > self._AUTH_BLOCK_TTL:
                return False
            return count > self._AUTH_FAIL_MAX

    def clear_auth_fail(self, ip: str) -> None:
        now = time.time()
        with self._auth_lock:
            self._auth_fails.pop(ip, None)
            expired = [k for k, (c, t) in self._auth_fails.items() if now - t > 120.0]
            for k in expired:
                del self._auth_fails[k]
```

- [ ] **Step 5: `_authorized` metodunu `_check_auth` ile değiştir**

`FirmwareProxyHandler` içindeki mevcut `_authorized`:

```python
    def _authorized(self) -> bool:
        expected = self.state.api_key
        provided = self.headers.get("X-Proxy-Key", "")
        return bool(expected and hmac.compare_digest(expected, provided))
```

Şu hale getir:

```python
    def _check_auth(self) -> Tuple[bool, bool]:
        """(authorized, blocked) döndürür."""
        ip = self.client_address[0]
        if self.state.is_auth_blocked(ip):
            return False, True
        expected = self.state.api_key
        provided = self.headers.get("X-Proxy-Key", "")
        if bool(expected and hmac.compare_digest(expected, provided)):
            self.state.clear_auth_fail(ip)
            return True, False
        self.state.record_auth_fail(ip)
        return False, False
```

- [ ] **Step 6: `do_GET` içindeki `_authorized()` çağrılarını güncelle**

`do_GET` içinde `/api/v1/catalog` bloğunu bul:

```python
            if not self._authorized():
                _send_json(self, 401, {"error": "yetkisiz"})
                return
```

Her iki `_authorized()` bloğunu şu hale getir:

```python
            authorized, blocked = self._check_auth()
            if blocked:
                _send_json(self, 429, {"error": "cok fazla basarisiz deneme, bekleyin"})
                return
            if not authorized:
                _send_json(self, 401, {"error": "yetkisiz"})
                return
```

> Not: `do_GET` içinde `/catalog` ve `/download/` olmak üzere iki ayrı `_authorized()` çağrısı var. Her ikisi de yukarıdaki şekilde değiştirilmeli.

- [ ] **Step 7: Testleri çalıştır**

```bash
cd Uploader && python -m pytest tests/ -v
```

Expected: `9 passed` (drive_manager: 4, proxy_server: 9)

- [ ] **Step 8: Commit**

```bash
git add Uploader/firmware_proxy_server.py Uploader/tests/test_proxy_server.py
git commit -m "feat: add per-IP rate limiting for failed auth attempts"
```

---

## Task 10: Bütünleşik doğrulama

- [ ] **Step 1: Tüm testleri çalıştır**

```bash
cd Uploader && python -m pytest tests/ -v --tb=short
```

Expected: `13 passed`, 0 failed, 0 error

- [ ] **Step 2: Proxy server'ı başlat ve health endpoint'i doğrula**

```bash
cd Uploader && FIRMWARE_PROXY_API_KEY=testkey FIRMWARE_PROXY_SA_JSON=/dev/null FIRMWARE_PROXY_CHANNEL_MAP_FILE=proxy_channels.json python firmware_proxy_server.py &
sleep 1
curl -s http://127.0.0.1:8787/api/v1/health
```

Expected: `{"ok": true, "ts": <timestamp>}`

- [ ] **Step 3: Rate limiting manuel doğrula**

```bash
for i in $(seq 1 12); do
  curl -s -o /dev/null -w "%{http_code}\n" \
    -H "X-Proxy-Key: wrongkey" \
    http://127.0.0.1:8787/api/v1/catalog?channel=test
done
```

Expected: İlk 10 istek `401`, 11. ve 12. istek `429`

- [ ] **Step 4: Proxy server'ı durdur**

```bash
kill %1
```

- [ ] **Step 5: Final commit**

```bash
git add -A
git commit -m "test: add integration validation for proxy optimizations"
```
