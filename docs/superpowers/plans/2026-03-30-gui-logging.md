# GUI Operation Logger + Clear Button Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** GUI işlemlerini yapılandırılmış bloklar halinde `gui.log` dosyasına kaydeder ve ekranı temizleyen bir buton ekler.

**Architecture:** `GUILogger` sınıfı `gui_logger.py`'da izole tutulur; `gui_uploader_qt.py` bunu kullanır. 8 işlem tipi `log_operation()` çağrısıyla kaydedilir. Upload için iki aşamalı strateji: başlangıçta detaylar toplanır, bitişte süre hesaplanıp yazar.

**Tech Stack:** Python 3, threading.Lock, PySide6/PyQt (QPushButton), tempfile (test)

---

## Dosya Yapısı

| Dosya | Rol |
|-------|-----|
| `Uploader/gui_logger.py` | Yeni — `GUILogger` sınıfı (sadece dosya yazma, UI bağımlılığı yok) |
| `Uploader/gui_uploader_qt.py` | Değişen — clear button, GUILogger init, 8 op entegrasyonu |
| `Uploader/tests/test_gui_logger.py` | Yeni — `GUILogger` unit testleri |

---

### Task 1: `GUILogger` Sınıfı + Testler

**Files:**
- Create: `Uploader/gui_logger.py`
- Create: `Uploader/tests/test_gui_logger.py`

- [ ] **Step 1: Failing testleri yaz**

`Uploader/tests/test_gui_logger.py` içeriği:

```python
import os
import re
import tempfile
import threading
import time

import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def test_success_block_format():
    """Başarılı işlem bloğu doğru alanları içerir."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation(
            "Proxy Başlatma",
            {"Adres": "http://127.0.0.1:8787", "TTL": "120 sn"},
            success=True,
        )
        content = _read(path)
        assert "Proxy Başlatma" in content
        assert "http://127.0.0.1:8787" in content
        assert "120 sn" in content
        assert "BASARILI" in content
        assert "HATA" not in content
    finally:
        os.unlink(path)


def test_failure_block_includes_error():
    """Hatalı işlem bloğu Hata satırını içerir, BASARILI içermez."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation(
            "Firmware Yükleme",
            {"Cihaz": "DeviceA"},
            success=False,
            error="BOOT_ACK gelmedi",
        )
        content = _read(path)
        assert "HATA" in content
        assert "BOOT_ACK gelmedi" in content
        assert "BASARILI" not in content
    finally:
        os.unlink(path)


def test_failure_without_error_no_hata_line():
    """error=None ise Hata satırı yazılmaz."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation("Op", {"k": "v"}, success=False, error=None)
        content = _read(path)
        assert "HATA" in content
        lines = [l for l in content.splitlines() if "Hata" in l and " : " in l]
        assert len(lines) == 0
    finally:
        os.unlink(path)


def test_date_separator_written_once():
    """Aynı gün iki çağrıda tarih separator bir kez yazılır."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation("Op1", {"k": "v"}, success=True)
        logger.log_operation("Op2", {"k": "v"}, success=True)
        content = _read(path)
        today = time.strftime("%Y-%m-%d")
        assert content.count(f"=== {today} ===") == 1
    finally:
        os.unlink(path)


def test_date_separator_written_on_date_change():
    """_current_date değişince yeni separator eklenir."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation("Op1", {"k": "v"}, success=True)
        logger._current_date = "2000-01-01"  # farklı gün simüle et
        logger.log_operation("Op2", {"k": "v"}, success=True)
        content = _read(path)
        sep_lines = [l for l in content.splitlines() if l.startswith("===")]
        assert len(sep_lines) == 2
    finally:
        os.unlink(path)


def test_column_alignment():
    """Tüm ' : ' ayraçları aynı sütunda hizalanır."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation(
            "Test",
            {"Kisa": "v1", "Uzun anahtar burada": "v2"},
            success=True,
        )
        content = _read(path)
        detail_lines = [l for l in content.splitlines() if l.startswith("  ") and " : " in l]
        positions = [l.index(" : ") for l in detail_lines]
        assert len(set(positions)) == 1, f"Farklı sütun pozisyonları: {positions}"
    finally:
        os.unlink(path)


def test_multiline_value_second_line_indented():
    """Çok satırlı değerin ikinci satırı değer sütununa hizalanır."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation(
            "Katalog",
            {"Dosyalar": "file1.bin\nfile2.bin"},
            success=True,
        )
        content = _read(path)
        lines = content.splitlines()
        file1_line = next(l for l in lines if "file1.bin" in l)
        file2_line = next(l for l in lines if "file2.bin" in l)
        # file2 satırı, file1'in değer başlangıcıyla aynı sütunda başlamalı
        value_start = file1_line.index(" : ") + 3
        assert file2_line[:value_start].strip() == "", (
            f"file2 satırı beklenen indent'te değil: '{file2_line}'"
        )
    finally:
        os.unlink(path)


def test_result_label_override():
    """result_label verilince BASARILI/HATA yerine o değer yazılır."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation(
            "Upload",
            {"Cihaz": "A"},
            success=False,
            result_label="DURDURULDU",
        )
        content = _read(path)
        assert "DURDURULDU" in content
        assert "HATA" not in content
    finally:
        os.unlink(path)


def test_append_mode_preserves_previous_content():
    """Mevcut dosyaya yeni blok eklenir, önceki içerik silinmez."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log", mode="w", encoding="utf-8") as f:
        f.write("ONCEKI ICERIK\n")
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        logger.log_operation("YeniOp", {"k": "v"}, success=True)
        content = _read(path)
        assert "ONCEKI ICERIK" in content
        assert "YeniOp" in content
    finally:
        os.unlink(path)


def test_oserror_does_not_raise():
    """Yazılamayan dosya yolu exception fırlatmaz."""
    from gui_logger import GUILogger
    logger = GUILogger("/nonexistent_dir_xyz/gui.log")
    logger.log_operation("Test", {"k": "v"}, success=True)  # raises olmamalı


def test_thread_safety():
    """Eşzamanlı çağrılar blokları karıştırmaz: toplam 10 blok yazılır."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".log") as f:
        path = f.name
    try:
        from gui_logger import GUILogger
        logger = GUILogger(path)
        barrier = threading.Barrier(2)

        def write_ops(name: str) -> None:
            barrier.wait()
            for _ in range(5):
                logger.log_operation(name, {"k": "v"}, success=True)

        t1 = threading.Thread(target=write_ops, args=("Op-A",))
        t2 = threading.Thread(target=write_ops, args=("Op-B",))
        t1.start()
        t2.start()
        t1.join()
        t2.join()

        content = _read(path)
        # Her işlem [HH:MM:SS] Title satırıyla başlar
        op_lines = re.findall(r"^\[\d{2}:\d{2}:\d{2}\]", content, re.MULTILINE)
        assert len(op_lines) == 10, f"Beklenen 10 blok, bulunan: {len(op_lines)}"
    finally:
        os.unlink(path)
```

- [ ] **Step 2: Testleri çalıştır — FAIL beklenir**

```bash
cd Uploader && .venv/Scripts/python.exe -m pytest tests/test_gui_logger.py -v 2>&1 | head -20
```

Beklenen: `ModuleNotFoundError: No module named 'gui_logger'`

- [ ] **Step 3: `gui_logger.py` yaz**

`Uploader/gui_logger.py` içeriği:

```python
import os
import threading
import time
from typing import Dict, Optional


class GUILogger:
    """GUI işlemlerini yapılandırılmış bloklarla gui.log'a yazar.

    Thread-safe. Dosya her çağrıda append modunda açılır.
    Tarih değişince otomatik === YYYY-MM-DD === separator ekler.
    """

    def __init__(self, log_path: str) -> None:
        self._log_path = log_path
        self._lock = threading.Lock()
        self._current_date: str = ""

    def log_operation(
        self,
        title: str,
        details: Dict[str, str],
        success: bool,
        error: Optional[str] = None,
        result_label: Optional[str] = None,
    ) -> None:
        """Dosyaya tek bir işlem bloğu yazar.

        Args:
            title:        İşlem başlığı (ör. "Firmware Yükleme")
            details:      Sıralı anahtar-değer çiftleri
            success:      True → BASARILI, False → HATA
            error:        Hata mesajı; sadece success=False ve error verilmişse eklenir
            result_label: Verilirse BASARILI/HATA yerine bu string yazılır
        """
        now = time.localtime()
        date_str = time.strftime("%Y-%m-%d", now)
        time_str = time.strftime("%H:%M:%S", now)
        result_str = result_label if result_label else ("BASARILI" if success else "HATA")

        # Hizalama sütunu: tüm key'ler + "Sonuç" + opsiyonel "Hata"
        all_keys = list(details.keys()) + ["Sonuç"]
        if not success and error:
            all_keys.append("Hata")
        col = max((len(k) for k in all_keys), default=5)

        def _fmt(key: str, value: str) -> str:
            pad = " " * (col - len(key))
            value_lines = str(value).split("\n")
            # İkinci ve sonraki satırlar için girinti: "  " + col + " : " genişliği
            indent = " " * (2 + col + 3)
            first = f"  {key}{pad} : {value_lines[0]}"
            rest = [indent + vl for vl in value_lines[1:]]
            return "\n".join([first] + rest)

        with self._lock:
            try:
                with open(self._log_path, "a", encoding="utf-8") as f:
                    if date_str != self._current_date:
                        f.write(f"\n=== {date_str} ===\n")
                        self._current_date = date_str
                    f.write(f"\n[{time_str}] {title}\n")
                    for key, value in details.items():
                        f.write(_fmt(key, value) + "\n")
                    f.write(_fmt("Sonuç", result_str) + "\n")
                    if not success and error:
                        f.write(_fmt("Hata", error) + "\n")
            except OSError:
                pass  # Log yazma hatası GUI'yi çöktürmemeli
```

- [ ] **Step 4: Testleri çalıştır — PASS beklenir**

```bash
cd Uploader && .venv/Scripts/python.exe -m pytest tests/test_gui_logger.py -v
```

Beklenen çıktı: `11 passed`

- [ ] **Step 5: Commit**

```bash
git add Uploader/gui_logger.py Uploader/tests/test_gui_logger.py
git commit -m "feat: add GUILogger class with operation block formatting"
```

---

### Task 2: Clear Button + GUILogger Init

**Files:**
- Modify: `Uploader/gui_uploader_qt.py:81-119` (instance vars)
- Modify: `Uploader/gui_uploader_qt.py:391-399` (status bar buttons)
- Modify: `Uploader/gui_uploader_qt.py:401-414` (_connect_ui)

- [ ] **Step 1: `gui_logger` import ekle**

`gui_uploader_qt.py` dosyasının import bölümüne ekle (satır 60 civarı, diğer local import'ların yanına):

Mevcut satır 60:
```python
from uploder import update_stm32_key, upload_firmware
```

Hemen altına ekle:
```python
from gui_logger import GUILogger
```

- [ ] **Step 2: `__init__`'e instance değişkenlerini ekle**

`__init__` içinde, satır 92 sonrasına (`self.upload_thread: Optional[threading.Thread] = None`):

Mevcut blok (satır 90-92):
```python
        self.pending_firmware_version: Optional[int] = None
        self.stop_requested = False
        self.upload_thread: Optional[threading.Thread] = None
```

Değiştir:
```python
        self.pending_firmware_version: Optional[int] = None
        self.stop_requested = False
        self.upload_thread: Optional[threading.Thread] = None
        self._upload_last_error: Optional[str] = None
        self._pending_upload_log: Optional[Dict[str, str]] = None
        self._upload_start_time: Optional[float] = None

        _log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gui.log")
        self.gui_logger = GUILogger(_log_path)
```

- [ ] **Step 3: Clear button widget'ını status bar'a ekle**

`_bind_widgets` içinde, satır 398-399 (`sb.addPermanentWidget(self._monitor_open_btn)`):

Mevcut:
```python
        sb.addPermanentWidget(self._monitor_open_btn)
```

Değiştir:
```python
        sb.addPermanentWidget(self._monitor_open_btn)

        self._log_clear_btn = QPushButton("Logu Temizle")
        self._log_clear_btn.setProperty("role", "subtle")
        self._log_clear_btn.setFixedWidth(110)
        sb.addPermanentWidget(self._log_clear_btn)
```

- [ ] **Step 4: Clear button'ı bağla**

`_connect_ui` içinde, satır 414 sonrasına (`self._monitor_open_btn.clicked.connect(...)`):

Mevcut:
```python
        self._monitor_open_btn.clicked.connect(self._open_device_monitor_dialog)
```

Değiştir:
```python
        self._monitor_open_btn.clicked.connect(self._open_device_monitor_dialog)
        self._log_clear_btn.clicked.connect(self.log_text.clear)
```

- [ ] **Step 5: Tüm mevcut testleri çalıştır — pass korunmalı**

```bash
cd Uploader && .venv/Scripts/python.exe -m pytest tests/ -v
```

Beklenen: tüm testler geçer (yeni test yok bu task'ta).

- [ ] **Step 6: Commit**

```bash
git add Uploader/gui_uploader_qt.py
git commit -m "feat: add clear log button and initialize GUILogger in app"
```

---

### Task 3: Upload Error Tracking + Firmware Upload Log Op

**Files:**
- Modify: `Uploader/gui_uploader_qt.py:1594-1597` (_append_log)
- Modify: `Uploader/gui_uploader_qt.py:2133-2166` (_start_upload — upload_cfg sonrası)
- Modify: `Uploader/gui_uploader_qt.py:2205-2245` (_on_upload_finished)

Bu task'ta test yoktur — `_append_log` ve upload flow GUI event loop'una bağlıdır. Mevcut 16 test korunur.

- [ ] **Step 1: `_append_log`'a upload hata yakalama ekle**

Mevcut `_append_log` (satır 1594-1597):
```python
    def _append_log(self, message: str) -> None:
        stamp = QDateTime.currentDateTime().toString("HH:mm:ss")
        self.log_text.appendPlainText(f"[{stamp}] {message}")
        self.log_text.verticalScrollBar().setValue(self.log_text.verticalScrollBar().maximum())
```

Değiştir:
```python
    def _append_log(self, message: str) -> None:
        stamp = QDateTime.currentDateTime().toString("HH:mm:ss")
        self.log_text.appendPlainText(f"[{stamp}] {message}")
        self.log_text.verticalScrollBar().setValue(self.log_text.verticalScrollBar().maximum())
        if self._upload_running:
            self._upload_last_error = message
```

- [ ] **Step 2: `_start_upload`'da `_pending_upload_log` oluştur**

`_start_upload` içinde `upload_cfg` dict'i bittikten hemen sonra (satır 2145 `}` kapanışından sonra), `self._append_log(...)` çağrısından önce:

Mevcut satırlar 2133-2166 kısmında, `upload_cfg = { ... }` bloğu satır 2145'te kapanıyor. Satır 2146-2166 arasına şunu ekle:

Mevcut satır 2145-2166:
```python
        }

        if target_download_token:
            proxy_error = self._ensure_local_proxy_available()
            if proxy_error:
                self._append_log(proxy_error)
                QMessageBox.warning(self._dialog_parent(), "Uyari", proxy_error)
                return

        self._set_completion_actions_visible(False)
        self._hide_success_overlay(animate=False)
        self._upload_running = True
        self._set_selection_controls_enabled(False, animated=False)
        self.stop_requested = False
        self._set_progress_busy(False)
        self.progress_bar.setValue(0)
        self.progress_label.setText("0%")
        self._set_progress_visual_mode("active")
        self.start_upload_button.setText("Durdur")
        self._set_upload_button_pulse(True)
        self._start_status_animation("Guncelleme devam ediyor")
        self._append_log(f"Secilen firmware: {file_item.get('name', '')} [{file_type}]")
```

Değiştir:
```python
        }

        if target_download_token:
            proxy_error = self._ensure_local_proxy_available()
            if proxy_error:
                self._append_log(proxy_error)
                QMessageBox.warning(self._dialog_parent(), "Uyari", proxy_error)
                return

        self._set_completion_actions_visible(False)
        self._hide_success_overlay(animate=False)
        self._upload_running = True
        self._set_selection_controls_enabled(False, animated=False)
        self.stop_requested = False
        self._set_progress_busy(False)
        self.progress_bar.setValue(0)
        self.progress_label.setText("0%")
        self._set_progress_visual_mode("active")
        self.start_upload_button.setText("Durdur")
        self._set_upload_button_pulse(True)
        self._start_status_animation("Guncelleme devam ediyor")
        self._append_log(f"Secilen firmware: {file_item.get('name', '')} [{file_type}]")

        ver = file_item.get("version")
        self._pending_upload_log = {
            "Cihaz": str(device.get("name", "?")),
            "Kanal": self._device_channel(device) or "-",
            "Firmware": str(file_item.get("name", "?")),
            "Tür": str(file_type),
            "Versiyon": f"v{ver}" if ver is not None else "?",
            "Boyut": f"{file_item.get('size', '?')} byte",
            "Mod": "RF" if upload_cfg["is_rf"] else "Seri",
            "COM Port": str(upload_cfg["serial_port"]),
            "Baud": str(upload_cfg["baud_rate"]),
            "Paket boyutu": f"{upload_cfg['packet_size']} byte",
        }
        self._upload_start_time = time.time()
        self._upload_last_error = None
```

- [ ] **Step 3: `_on_upload_finished`'da log yaz**

`_on_upload_finished` sonunda (satır 2205, `self._upload_running = False` satırından sonra) ekle.

Mevcut satırlar 2205-2246:
```python
    def _on_upload_finished(self, success: bool) -> None:
        self._upload_running = False
        self._set_selection_controls_enabled(True, animated=True)
        self.start_upload_button.setText("Guncellemeyi Baslat")
        self._set_upload_button_pulse(False)

        if success:
            ...
        elif self.stop_requested:
            ...
        else:
            ...
        self.pending_firmware_version = None
```

`self.pending_firmware_version = None` satırının hemen ÖNÜNE şunu ekle:

```python
        if self._pending_upload_log is not None and self._upload_start_time is not None:
            elapsed = time.time() - self._upload_start_time
            details = dict(self._pending_upload_log)
            details["Süre"] = f"{elapsed:.1f} sn"
            if self.stop_requested and not success:
                self.gui_logger.log_operation(
                    "Firmware Yükleme",
                    details,
                    success=False,
                    result_label="DURDURULDU",
                )
            else:
                self.gui_logger.log_operation(
                    "Firmware Yükleme",
                    details,
                    success=success,
                    error=self._upload_last_error if not success else None,
                )
        self._pending_upload_log = None
        self._upload_start_time = None
```

Yani `_on_upload_finished`'ın son hali:

```python
    def _on_upload_finished(self, success: bool) -> None:
        self._upload_running = False
        self._set_selection_controls_enabled(True, animated=True)
        self.start_upload_button.setText("Guncellemeyi Baslat")
        self._set_upload_button_pulse(False)

        if success:
            self._stop_status_animation("Guncelleme basarili.")
            self.progress_bar.setValue(100)
            self.progress_label.setText("100%")
            self._set_progress_visual_mode("success")
            self._set_completion_actions_visible(
                True,
                "Guncelleme Basarili. Yeni bir islem baslatabilir veya ayarlari duzenleyebilirsiniz.",
            )
            self._show_success_overlay()
            device = self._get_selected_device()
            if device and self.pending_firmware_version is not None:
                device["last_installed_version"] = self.pending_firmware_version
                self.pending_firmware_version = None
                if self.admin_password:
                    try:
                        save_config(self.config, self.admin_password)
                        self._append_log(f"Yuklu versiyon kaydedildi: v{device['last_installed_version']}")
                    except Exception:
                        self._append_log("Versiyon kaydedilemedi. Admin panelinden tekrar kaydedin.")
        elif self.stop_requested:
            self._stop_status_animation("Islem kullanici tarafindan durduruldu.")
            self._set_progress_visual_mode("error")
            self._set_completion_actions_visible(
                True,
                "Islem durduruldu. Ayarlari degistirip yeniden baslatabilirsiniz.",
            )
        else:
            self._stop_status_animation("Guncelleme basarisiz.")
            self._set_progress_visual_mode("error")
            self._set_completion_actions_visible(
                True,
                "Guncelleme basarisiz. Yeni deneme icin ayarlari kontrol edin.",
            )

        if self._pending_upload_log is not None and self._upload_start_time is not None:
            elapsed = time.time() - self._upload_start_time
            details = dict(self._pending_upload_log)
            details["Süre"] = f"{elapsed:.1f} sn"
            if self.stop_requested and not success:
                self.gui_logger.log_operation(
                    "Firmware Yükleme",
                    details,
                    success=False,
                    result_label="DURDURULDU",
                )
            else:
                self.gui_logger.log_operation(
                    "Firmware Yükleme",
                    details,
                    success=success,
                    error=self._upload_last_error if not success else None,
                )
        self._pending_upload_log = None
        self._upload_start_time = None
        self.pending_firmware_version = None

        if self._monitor_was_running and self._monitor_port:
            baud = self.config.get("baud_rate", 115200)
            self.device_monitor = DeviceMonitor(
                port=self._monitor_port,
                baud=baud,
                on_message=lambda level, code, msg, ts: self.signals.device_log.emit(
                    level, code, msg, ts
                ),
            )
            self.device_monitor.start()
            self._monitor_was_running = False
```

- [ ] **Step 4: `time` import kontrolü**

`gui_uploader_qt.py` başında `import time` olduğunu doğrula:

```bash
grep -n "^import time" Uploader/gui_uploader_qt.py
```

Yoksa mevcut import bloğuna `import time` ekle.

- [ ] **Step 5: Mevcut testler hâlâ geçiyor mu?**

```bash
cd Uploader && .venv/Scripts/python.exe -m pytest tests/ -v
```

Beklenen: tüm testler geçer.

- [ ] **Step 6: Commit**

```bash
git add Uploader/gui_uploader_qt.py
git commit -m "feat: log firmware upload operations with duration and error capture"
```

---

### Task 4: Kalan 7 İşlem Entegrasyonu

**Files:**
- Modify: `Uploader/gui_uploader_qt.py` (7 farklı metot)

Loglanan işlemler:
1. Config Yükleme (`_try_load_config`)
2. Proxy Başlatma (`_start_local_proxy_server`)
3. Proxy Durdurma (`_stop_local_proxy_server`)
4. Proxy Bağlantı Testi (`_test_proxy_connection`)
5. Katalog Sorgusu (`_on_scan_finished`)
6. COM Port Tarama (`_scan_ports`)
7. AES Key Güncelleme (`_on_key_update_finished`)

- [ ] **Step 1: Config Yükleme logu — `_try_load_config`**

Mevcut `_try_load_config` (satır 1606-1616):
```python
    def _try_load_config(self) -> None:
        if config_exists():
            try:
                self.config = load_config("admin")
                self._append_log("Config yuklendi (varsayilan sifre ile).")
            except ValueError:
                self._append_log("Config sifreli. Admin girisi yapmadan admin paneli acilmaz.")
        else:
            self._append_log("Config dosyasi yok. Admin panelinden cihaz ekleyin.")
        self._reload_download_clients()
        self._refresh_device_list()
```

Değiştir:
```python
    def _try_load_config(self) -> None:
        if config_exists():
            try:
                self.config = load_config("admin")
                self._append_log("Config yuklendi (varsayilan sifre ile).")
                self.gui_logger.log_operation(
                    "Config Yükleme",
                    {
                        "Durum": "Yuklendi",
                        "Cihaz sayısı": str(len(self.config.get("devices", []))),
                        "Proxy": "Yapilandirilmis" if self.config.get("proxy_backend") else "Tanimli degil",
                    },
                    success=True,
                )
            except ValueError:
                self._append_log("Config sifreli. Admin girisi yapmadan admin paneli acilmaz.")
                self.gui_logger.log_operation(
                    "Config Yükleme",
                    {"Durum": "Sifreli — admin girisi gerekli"},
                    success=False,
                    error="Config dosyasi sifre korumalı, otomatik yükleme yapılamadı",
                )
        else:
            self._append_log("Config dosyasi yok. Admin panelinden cihaz ekleyin.")
            self.gui_logger.log_operation(
                "Config Yükleme",
                {"Durum": "Dosya yok"},
                success=False,
                error="Config dosyası bulunamadı",
            )
        self._reload_download_clients()
        self._refresh_device_list()
```

- [ ] **Step 2: Proxy Başlatma logu — `_start_local_proxy_server`**

Mevcut `_start_local_proxy_server` (satır 1750-1789). `announce` bloğu satır 1784-1786:
```python
        if announce:
            self._append_log(f"Yerel proxy baslatildi: {settings.get('base_url', '')}")
            self._set_admin_status("Yerel proxy baslatildi.")
```

Değiştir:
```python
        if announce:
            self._append_log(f"Yerel proxy baslatildi: {settings.get('base_url', '')}")
            self._set_admin_status("Yerel proxy baslatildi.")
        self.gui_logger.log_operation(
            "Proxy Başlatma",
            {
                "Adres": str(settings.get("base_url", f"http://{settings.get('host','?')}:{settings.get('port','?')}")),
                "Servis JSON": str(settings.get("service_json", "?")),
                "Kanal haritası": str(settings.get("channel_map_file", "?")),
                "Token TTL": f"{settings.get('token_ttl', 120)} sn",
            },
            success=True,
        )
```

Aynı metodun hata durumu: satır 1764-1765:
```python
        except Exception as exc:
            return f"Yerel proxy baslatilamadi: {exc}"
```

Değiştir:
```python
        except Exception as exc:
            self.gui_logger.log_operation(
                "Proxy Başlatma",
                {
                    "Adres": str(settings.get("base_url", f"http://{settings.get('host','?')}:{settings.get('port','?')}")),
                    "Servis JSON": str(settings.get("service_json", "?")),
                    "Kanal haritası": str(settings.get("channel_map_file", "?")),
                },
                success=False,
                error=str(exc),
            )
            return f"Yerel proxy baslatilamadi: {exc}"
```

- [ ] **Step 3: Proxy Durdurma logu — `_stop_local_proxy_server`**

Mevcut satır 1809-1813:
```python
        self._reload_download_clients()
        if announce:
            self._append_log("Yerel proxy durduruldu.")
            self._set_admin_status("Yerel proxy durduruldu.")
        self._refresh_proxy_runtime_status()
        return None
```

Değiştir:
```python
        self._reload_download_clients()
        if announce:
            self._append_log("Yerel proxy durduruldu.")
            self._set_admin_status("Yerel proxy durduruldu.")
        _stopped_url = str((self._proxy_runtime_config or {}).get("base_url", "?"))
        self.gui_logger.log_operation(
            "Proxy Durdurma",
            {"Adres": _stopped_url},
            success=True,
        )
        self._refresh_proxy_runtime_status()
        return None
```

Hata durumu (satır 1802-1803):
```python
        except Exception as exc:
            return f"Yerel proxy durdurulamadi: {exc}"
```

Değiştir:
```python
        except Exception as exc:
            _url = str((self._proxy_runtime_config or {}).get("base_url", "?"))
            self.gui_logger.log_operation(
                "Proxy Durdurma",
                {"Adres": _url},
                success=False,
                error=str(exc),
            )
            return f"Yerel proxy durdurulamadi: {exc}"
```

- [ ] **Step 4: Proxy Bağlantı Testi logu — `_test_proxy_connection`**

Mevcut satır 1892-1904:
```python
        payload, error = client.health()
        if error:
            self._refresh_proxy_runtime_status()
            self._append_log(error)
            QMessageBox.warning(self._dialog_parent(), "Proxy Testi", error)
            return

        ts = "-"
        if isinstance(payload, dict):
            ts = str(payload.get("ts", "-"))
        self._refresh_proxy_runtime_status()
        self._append_log(f"Proxy testi basarili: {client.base_url} (ts={ts})")
        self._set_admin_status("Proxy baglanti testi basarili.")
        QMessageBox.information(self._dialog_parent(), "Proxy Testi", f"Proxy erisilebilir:\n{client.base_url}")
```

Değiştir:
```python
        payload, error = client.health()
        if error:
            self._refresh_proxy_runtime_status()
            self._append_log(error)
            self.gui_logger.log_operation(
                "Proxy Bağlantı Testi",
                {"Adres": client.base_url},
                success=False,
                error=error,
            )
            QMessageBox.warning(self._dialog_parent(), "Proxy Testi", error)
            return

        ts = "-"
        if isinstance(payload, dict):
            ts = str(payload.get("ts", "-"))
        self._refresh_proxy_runtime_status()
        self._append_log(f"Proxy testi basarili: {client.base_url} (ts={ts})")
        self._set_admin_status("Proxy baglanti testi basarili.")
        self.gui_logger.log_operation(
            "Proxy Bağlantı Testi",
            {"Adres": client.base_url, "Sunucu TS": ts},
            success=True,
        )
        QMessageBox.information(self._dialog_parent(), "Proxy Testi", f"Proxy erisilebilir:\n{client.base_url}")
```

- [ ] **Step 5: Katalog Sorgusu logu — `_on_scan_finished`**

Mevcut `_on_scan_finished` (satır 2024-2056). Hata dalı satır 2028-2036:
```python
        if not files:
            self.available_files = []
            self.firmware_combo.clear()
            self.firmware_info_label.setText("")
            self._apply_firmware_badges(None)
            self._stop_update_animation(str(error or "Dosya bulunamadi"))
            if error:
                self._append_log(str(error))
            return
```

Değiştir:
```python
        if not files:
            self.available_files = []
            self.firmware_combo.clear()
            self.firmware_info_label.setText("")
            self._apply_firmware_badges(None)
            self._stop_update_animation(str(error or "Dosya bulunamadi"))
            if error:
                self._append_log(str(error))
            _channel = self._device_channel(self._get_selected_device())
            _proxy_url = self.proxy_client.base_url if self.proxy_client else "-"
            self.gui_logger.log_operation(
                "Katalog Sorgusu",
                {"Kanal": _channel or "-", "Proxy": _proxy_url, "Bulunan": "0 dosya"},
                success=False,
                error=str(error) if error else "Dosya bulunamadı",
            )
            return
```

Başarı dalı satır 2052-2056:
```python
        msg = f"{len(self.available_files)} dosya bulundu"
        if error:
            msg += f" ({error})"
        self._stop_update_animation(msg)
        self._append_log("Klasor tarandi: " + msg)
```

Değiştir:
```python
        msg = f"{len(self.available_files)} dosya bulundu"
        if error:
            msg += f" ({error})"
        self._stop_update_animation(msg)
        self._append_log("Klasor tarandi: " + msg)

        _channel = self._device_channel(self._get_selected_device())
        _proxy_url = self.proxy_client.base_url if self.proxy_client else "-"
        _file_lines = []
        for item in self.available_files:
            ver = item.get("version")
            ver_str = f"v{ver}" if ver is not None else "v?"
            _file_lines.append(
                f"{item.get('name', '?')} ({item.get('type', '?')}, {item.get('size', '?')} byte, {ver_str})"
            )
        _files_value = "\n".join(_file_lines) if _file_lines else "-"
        self.gui_logger.log_operation(
            "Katalog Sorgusu",
            {
                "Kanal": _channel or "-",
                "Proxy": _proxy_url,
                "Dosyalar": _files_value,
                "Bulunan": f"{len(self.available_files)} dosya",
            },
            success=True,
            error=str(error) if error else None,
        )
```

- [ ] **Step 6: COM Port Tarama logu — `_scan_ports`**

Mevcut satır 1939-1955:
```python
        if serial_list_ports is None:
            self.port_combo.clear()
            self.port_combo.addItem("pyserial kurulu degil")
            self._append_log("COM port taramasi atlandi: pyserial bulunamadi.")
            return

        ports = [port.device for port in serial_list_ports.comports()]
        ...
        self._append_log("COM portlar tarandi: " + (", ".join(ports) if ports else "yok"))
```

Değiştir:
```python
        if serial_list_ports is None:
            self.port_combo.clear()
            self.port_combo.addItem("pyserial kurulu degil")
            self._append_log("COM port taramasi atlandi: pyserial bulunamadi.")
            self.gui_logger.log_operation(
                "COM Port Tarama",
                {"Bulunan": "yok"},
                success=False,
                error="pyserial kurulu değil",
            )
            return

        ports = [port.device for port in serial_list_ports.comports()]
        self.port_combo.clear()
        if ports:
            self.port_combo.addItems(ports)
            cfg_port = self.config.get("serial_port", "")
            if cfg_port in ports:
                self.port_combo.setCurrentText(cfg_port)
        else:
            self.port_combo.addItem("Port bulunamadi")
        self.baud_value_label.setText(str(self.config.get("baud_rate", 115200)))
        self._append_log("COM portlar tarandi: " + (", ".join(ports) if ports else "yok"))
        self.gui_logger.log_operation(
            "COM Port Tarama",
            {"Bulunan": ", ".join(ports) if ports else "yok"},
            success=True,
        )
```

- [ ] **Step 7: AES Key Güncelleme logu — `_on_key_update_finished`**

Mevcut satır 2749-2770:
```python
    def _on_key_update_finished(self, success: bool, context: Any) -> None:
        self._set_progress_busy(False)
        if not isinstance(context, dict):
            self._stop_status_animation("Hazir")
            return
        device_name = context.get("device_name", "")
        new_key = context.get("new_key", "")

        if success:
            ...
            self._append_log("Cihaz AES key guncellendi. ...")
            ...
        else:
            self._set_admin_status("STM32 key guncelleme basarisiz.")
            self._stop_status_animation("STM32 key guncelleme basarisiz.")
            QMessageBox.critical(...)
```

`if success:` bloğundaki `_append_log` çağrısından sonra ve `else:` bloğundaki `QMessageBox.critical` çağrısından sonra ekle:

`if success:` bloğu sonuna (satır 2766 `QMessageBox.information` sonrasına):
```python
            self.gui_logger.log_operation(
                "AES Key Güncelleme",
                {"Cihaz": device_name},
                success=True,
            )
```

`else:` bloğu sonuna (satır 2770 `QMessageBox.critical` sonrasına):
```python
            self.gui_logger.log_operation(
                "AES Key Güncelleme",
                {"Cihaz": device_name},
                success=False,
                error="STM32 key güncelleme başarısız",
            )
```

Tam hali:
```python
    def _on_key_update_finished(self, success: bool, context: Any) -> None:
        self._set_progress_busy(False)
        if not isinstance(context, dict):
            self._stop_status_animation("Hazir")
            return
        device_name = context.get("device_name", "")
        new_key = context.get("new_key", "")

        if success:
            for dev in self.config.get("devices", []):
                if dev.get("name") == device_name:
                    dev["aes_key_hex"] = new_key
                    break
            self._append_log("Cihaz AES key guncellendi. Kaydetmek icin 'Tum Ayarlari Kaydet' tusunu kullanin.")
            self._refresh_admin_device_list()
            self._set_admin_status("STM32 key guncellendi (kaydetmeyi unutmayin).")
            self._stop_status_animation("STM32 key guncelleme basarili.")
            QMessageBox.information(self._dialog_parent(), "Basarili", "STM32 key guncelleme basarili.")
            self.gui_logger.log_operation(
                "AES Key Güncelleme",
                {"Cihaz": device_name},
                success=True,
            )
        else:
            self._set_admin_status("STM32 key guncelleme basarisiz.")
            self._stop_status_animation("STM32 key guncelleme basarisiz.")
            QMessageBox.critical(self._dialog_parent(), "Hata", "STM32 key guncelleme basarisiz.")
            self.gui_logger.log_operation(
                "AES Key Güncelleme",
                {"Cihaz": device_name},
                success=False,
                error="STM32 key güncelleme başarısız",
            )
```

- [ ] **Step 8: Tüm testler geçiyor mu?**

```bash
cd Uploader && .venv/Scripts/python.exe -m pytest tests/ -v
```

Beklenen: tüm testler geçer.

- [ ] **Step 9: Commit**

```bash
git add Uploader/gui_uploader_qt.py
git commit -m "feat: integrate GUILogger for all 7 remaining operation types"
```
