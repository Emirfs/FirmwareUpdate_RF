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
        assert "BASARILI" not in content
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
        op_lines = re.findall(r"^\[\d{2}:\d{2}:\d{2}\]", content, re.MULTILINE)
        assert len(op_lines) == 10, f"Beklenen 10 blok, bulunan: {len(op_lines)}"
    finally:
        os.unlink(path)
