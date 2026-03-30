"""
device_monitor.py — UART Cihaz Monitor Thread

Gonderici STM32 cihazının UART ciktisini arka planda okur ve
yapilandirilmis mesajlari callback ile bildirir.

Mesaj formatlari (sender_uart_debug.h ile senkron):
  [E:XX] metin  -> Hata    (kirmizi)
  [W:XX] metin  -> Uyari   (sari)
  [I:XX] metin  -> Bilgi   (mavi)
  Diger metin   -> Genel log (gri)

Kullanim:
    monitor = DeviceMonitor("COM5", baud=115200, on_message=callback)
    monitor.start()
    ...
    monitor.stop()

Callback imzasi:
    def callback(level: str, code: int, msg: str, timestamp: str) -> None
    level: 'E' (hata), 'W' (uyari), 'I' (bilgi), 'G' (genel)
"""

import threading
import re
from datetime import datetime

try:
    import serial
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False


# Yapilandirilmis mesaj pattern'i: [E:01] mesaj
_DIAG_PATTERN = re.compile(r'^\[([EWI]):([0-9A-Fa-f]{2})\]\s*(.*)$')

# Bilinen hata kodlari (sender_uart_debug.h ile senkron)
DIAG_CODE_NAMES = {
    0x01: "Si4432 baslatilamadi",
    0x02: "RF zaman asimi",
    0x03: "UART alma hatasi",
    0x11: "BOOT_ACK yeniden deneme",
    0x21: "Gonderici hazir",
    0x22: "FW guncelleme tamamlandi",
}


class DeviceMonitor:
    """
    Arka plan UART okuma threadi.

    Baslatildiktan sonra belirtilen COM portunu dinler. Her satir
    geldiginde on_message callback'i cagirilir. Upload sirasinda
    port paylasim cakismasi olmasin diye stop()/start() ile
    duraklatilip yeniden baslatilabilir.
    """

    def __init__(self, port: str, baud: int = 115200, on_message=None):
        self.port = port
        self.baud = baud
        self.on_message = on_message
        self._thread = None
        self._stop_event = threading.Event()
        self._connected = False

    @property
    def is_running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    @property
    def is_connected(self) -> bool:
        return self._connected

    def start(self) -> None:
        if self.is_running:
            return
        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._run, name="DeviceMonitor", daemon=True
        )
        self._thread.start()

    def stop(self) -> None:
        """Monitor'u durdur. Thread'in bitmesini beklemez."""
        self._stop_event.set()

    def _emit(self, level: str, code: int, msg: str) -> None:
        if self.on_message:
            ts = datetime.now().strftime("%H:%M:%S")
            try:
                self.on_message(level, code, msg, ts)
            except Exception:
                pass

    def _run(self) -> None:
        if not SERIAL_AVAILABLE:
            self._emit("E", 0, "pyserial yuklu degil — pip install pyserial")
            return

        try:
            ser = serial.Serial(self.port, self.baud, timeout=0.3)
        except serial.SerialException as exc:
            self._emit("E", 0, f"Port acilamadi ({self.port}): {exc}")
            return

        self._connected = True
        self._emit("I", 0, f"Monitor baglandi: {self.port} @ {self.baud}")

        buf = b""
        try:
            while not self._stop_event.is_set():
                try:
                    chunk = ser.read(128)
                except serial.SerialException:
                    break
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        line_bytes, buf = buf.split(b"\n", 1)
                        text = line_bytes.decode("utf-8", errors="replace").strip("\r\n ")
                        if text:
                            self._parse_and_emit(text)
        finally:
            self._connected = False
            try:
                ser.close()
            except Exception:
                pass
            if not self._stop_event.is_set():
                self._emit("W", 0, "Cihaz baglantisi kesildi.")
            else:
                self._emit("I", 0, "Monitor durduruldu.")

    def _parse_and_emit(self, text: str) -> None:
        m = _DIAG_PATTERN.match(text)
        if m:
            level = m.group(1)
            code = int(m.group(2), 16)
            msg = m.group(3).strip() or DIAG_CODE_NAMES.get(code, f"Kod: 0x{code:02X}")
            self._emit(level, code, msg)
        else:
            # Yapilandirilmamis mesaj — Print() ciktisi
            self._emit("G", 0, text)
