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
