"""
rf_uploader.py — Si4432 RF uzerinden uzaktan STM32 Firmware Guncelleme

─── BU DOSYA NE YAPAR? ──────────────────────────────────────────────────────
PC → (UART) → Gonderici STM32 → (Si4432 433 MHz RF) → Alici STM32 Bootloader

PC bu scriptle gonderici cihaza (UART) baglanir. Gonderici cihaz RF koprusu
olarak gorev yapar; gelen UART komutlarini/verilerini RF paketlerine cevirerek
alici bootloader'a iletir.

─── UART PROTOKOLU ──────────────────────────────────────────────────────────
Adim 1: 'W' gonder (1 byte)
        Gonderici BOOT_REQUEST yayinlar, alici BOOT_ACK dondurur
        → ACK (0x06) bekle

Adim 2: Metadata gonder (12 byte, little-endian)
        [firmware_size:4][firmware_version:4][firmware_crc32:4]
        → ACK (0x06) bekle

Adim 3: Flash erase bekleme
        Alici 119 sayfa x 2KB = 238KB'yi siler, uzun surer
        → ACK (0x06) bekle (FLASH_ERASE_DONE geldiginde)

Adim 4: Paket dongusu
        Her paket: AES-256-CBC sifrele → [IV:16][Encrypted:128][CRC32:4] = 148 byte
        148 byte'i gonder → ACK bekle
        (Gonderici bu 148 byte'i 4 RF chunk olarak aliciya iletir)

Adim 5: Final dogrulama
        Alici tum veriyi cozup Flash CRC'sini kontrol eder
        → ACK (0x06) = basarili, NACK (0x15) = basarisiz

─── AES SIFRELEME ───────────────────────────────────────────────────────────
Her 128 byte'lik firmware blogu AES-256-CBC ile sifrelenr:
  - AES key: 32 byte (alici cihazin DEFAULT_AES_KEY ile ayni olmali)
  - IV: Her paket icin rastgele 16 byte (os.urandom)
  - CRC: Sifrelenmis verinin CRC-32'si (paket butunlugu icin)

─── DEGISTIRILECEK SEYLER ───────────────────────────────────────────────────
- DEFAULT_AES_KEY (--key arg veya config): Alici DEFAULT_AES_KEY ile ayni olmali
- RF_*_TIMEOUT sabitleri: RF ortam kosuluna gore arttirilabilir
- RF_PACKET_DELAY: Paketler arasi bekleme (RF stabilite icin)

─── BAGIMLILIKLAR ───────────────────────────────────────────────────────────
pycryptodome : pip install pycryptodome  (AES icin)
intelhex     : pip install intelhex      (HEX dosyasi destegi, opsiyonel)
pyserial     : pip install pyserial      (UART iletisimi)
"""

import serial
import time
import os
import sys
import io
import zlib

try:
    from Crypto.Cipher import AES
except ImportError:
    print("pycryptodome gerekli: pip install pycryptodome")
    sys.exit(1)

try:
    from intelhex import IntelHex
    INTELHEX_AVAILABLE = True
except ImportError:
    INTELHEX_AVAILABLE = False


# ═══════════════════════════════════════════════
# SABITLER
# ═══════════════════════════════════════════════

# Sifreli firmware paketi boyutu: sifresiz 128 byte blok boyutu
DEFAULT_PACKET_SIZE = 128

# RF modu timeout'lari — UART dogrudan baglantisindan cok daha uzun
# Cunku her paketin 4 RF chunk gonderimi + ACK bekleme suresi var
RF_HANDSHAKE_TIMEOUT = 45   # 'W' → ACK: BOOT_REQUEST yayini + BOOT_ACK donusu
RF_METADATA_TIMEOUT  = 15   # Metadata → ACK: RF ile iletme + alici onayı
RF_FLASH_ERASE_TIMEOUT = 90 # Flash erase: 119 sayfa x ~5ms = min ~600ms ama guvenli
RF_PACKET_TIMEOUT = 20      # Her 148-byte paket → ACK: 4 RF chunk + 4 ACK
RF_FINAL_TIMEOUT  = 45      # Final CRC dogrulama: tum Flash tarama + RF mesaj
RF_PACKET_DELAY   = 0.5     # Paketler arasi bekleme (RF RF stabilite icin)
RF_MAX_RETRIES    = 10      # NACK veya timeout durumunda tekrar deneme sayisi


def hex_to_bin(hex_data: bytes) -> bytes:
    """
    Intel HEX formatindaki veriyi raw binary'ye donusturur.

    Oncelikle intelhex kutuphanesi kullanilir (daha guvenilir).
    Yoksa basit bir el yazimi HEX parser kullanilir.

    Desteklenen record tipleri:
      0x00 = Data
      0x01 = End Of File
      0x02 = Extended Segment Address
      0x04 = Extended Linear Address
    """
    if INTELHEX_AVAILABLE:
        ih = IntelHex()
        ih.loadhex(io.StringIO(hex_data.decode('ascii', errors='ignore')))
        start = ih.minaddr()
        end = ih.maxaddr()
        return ih.tobinarray(start=start, size=end - start + 1).tobytes()
    
    records = hex_data.decode('ascii', errors='ignore').strip().split('\n')
    data_blocks = {}
    base_address = 0
    
    for line in records:
        line = line.strip()
        if not line.startswith(':'):
            continue
        byte_count = int(line[1:3], 16)
        address = int(line[3:7], 16)
        record_type = int(line[7:9], 16)
        
        if record_type == 0x00:
            data = bytes.fromhex(line[9:9 + byte_count * 2])
            full_addr = base_address + address
            for i, b in enumerate(data):
                data_blocks[full_addr + i] = b
        elif record_type == 0x02:
            base_address = int(line[9:13], 16) << 4
        elif record_type == 0x04:
            base_address = int(line[9:13], 16) << 16
        elif record_type == 0x01:
            break
    
    if not data_blocks:
        raise ValueError("HEX dosyasında veri bulunamadı!")
    
    min_addr = min(data_blocks.keys())
    max_addr = max(data_blocks.keys())
    result = bytearray(max_addr - min_addr + 1)
    for addr, byte in data_blocks.items():
        result[addr - min_addr] = byte
    
    return bytes(result)


def calculate_crc32(data):
    """CRC-32 hesapla (zlib/ISO-HDLC — STM32'deki Calculate_CRC32 ile ayni)."""
    return zlib.crc32(data) & 0xFFFFFFFF


def progress_bar(current, total, width=40):
    """Terminal'de ilerleme cubugu goster: [████░░░░] 50% (5/10)"""
    percent = current * 100 // total
    filled = width * current // total
    bar = '#' * filled + '.' * (width - filled)
    print(f"\r  [{bar}] {percent}% ({current}/{total})", end='', flush=True)


def upload_firmware_rf(config, log=None, on_progress=None, stop_flag=None):
    """
    RF üzerinden firmware güncelleme.
    
    Gönderici cihaz UART↔RF köprüsü olarak çalışır, bu yüzden UART
    protokolü mevcut bootloader ile aynıdır. Sadece timeout'lar farklıdır.
    
    Args:
        config (dict): Konfigürasyon
            - serial_port: COM port (gönderici bağlı)
            - baud_rate: UART hızı
            - aes_key_hex: AES-256 key (hex string)
            - firmware_version: Firmware versiyonu
            - firmware_file: Yerel firmware dosya yolu (opsiyonel)
            - file_type: "BIN" veya "HEX"
            - packet_size: Paket boyutu (varsayılan 128)
        log (func): Log callback'i
        on_progress (func): İlerleme callback'i (current, total)
        stop_flag (func): Durdurma kontrolü
    
    Returns:
        bool: Başarılı ise True
    """
    def _log(msg):
        if log:
            log(msg)
        else:
            print(msg)

    def _progress(cur, total):
        if on_progress:
            on_progress(cur, total)
        else:
            progress_bar(cur, total)

    def _stopped():
        return stop_flag() if stop_flag else False

    # ─── CONFIG PARAMETRELERI ───────────────────────────────────────────
    serial_port      = config.get("serial_port", "COM7")       # Gonderici bagli port
    baud_rate        = config.get("baud_rate", 115200)          # UART hizi
    aes_key_hex      = config.get("aes_key_hex", "")           # 32 byte = 64 hex karakter
    firmware_version = config.get("firmware_version", 1)       # Versiyon numarasi
    packet_size      = config.get("packet_size", DEFAULT_PACKET_SIZE)  # 128 byte
    firmware_file    = config.get("firmware_file", "")         # .bin veya .hex dosya yolu
    file_type        = config.get("file_type", "BIN").upper()  # "BIN" veya "HEX"

    # AES key donusumu: hex string → bytes
    # Ornek: "3132...3132" (64 hex karakter) → b'1234...12' (32 byte)
    try:
        aes_key = bytes.fromhex(aes_key_hex)
        if len(aes_key) != 32:
            _log("AES key 32 byte (64 hex karakter) olmalidir!")
            return False
    except ValueError:
        # Hex degil, ASCII string olarak dene (gelistirme/test icin)
        aes_key = aes_key_hex.encode('utf-8')
        if len(aes_key) != 32:
            _log("AES key 32 byte olmalidir!")
            return False

    ser = None
    firmware_data = None

    try:
        # ═══════════════════════════════════════════════
        # 1. FIRMWARE DOSYASINI OKU
        # ═══════════════════════════════════════════════
        _log(f"📥 Firmware dosyası okunuyor: {firmware_file}")

        if not firmware_file or not os.path.exists(firmware_file):
            _log("❌ Firmware dosyası bulunamadı!")
            return False

        with open(firmware_file, 'rb') as f:
            raw_firmware = f.read()

        if _stopped():
            return False

        # HEX dosyasıysa binary'ye dönüştür
        if file_type == "HEX" or firmware_file.lower().endswith('.hex'):
            _log("🔄 HEX → BIN dönüştürülüyor...")
            try:
                raw_firmware = hex_to_bin(raw_firmware)
                _log(f"✅ HEX dönüştürme başarılı: {len(raw_firmware)} byte")
            except Exception as e:
                _log(f"❌ HEX dönüştürme hatası: {e}")
                return False

        firmware_size = len(raw_firmware)
        firmware_crc = calculate_crc32(raw_firmware)
        total_packets = (firmware_size + packet_size - 1) // packet_size

        _log(f"✅ Boyut: {firmware_size} byte | CRC: 0x{firmware_crc:08X} | Paket: {total_packets}")
        
        # RF tahmini süre hesapla
        # Her paket: 3 RF chunk × (~50ms TX + ~50ms ACK bekleme) + overhead
        estimated_seconds = total_packets * 3  # Yaklaşık tahmin
        _log(f"⏱️  Tahmini süre: ~{estimated_seconds // 60} dk {estimated_seconds % 60} sn (RF üzerinden)")

        firmware_data = io.BytesIO(raw_firmware)

        # ═══════════════════════════════════════════════
        # 2. SERİ PORT AÇ (Gönderici cihaza bağlı)
        # ═══════════════════════════════════════════════
        _log(f"🔌 {serial_port} açılıyor (RF gönderici)...")
        ser = serial.Serial(serial_port, baud_rate, timeout=RF_HANDSHAKE_TIMEOUT)
        time.sleep(2)
        ser.reset_input_buffer()

        # ═══════════════════════════════════════════════
        # 3. HANDSHAKE: 'W' gönder → ACK bekle
        #    Gönderici BOOT_REQUEST gönderir, alıcı bootloader'a geçer
        # ═══════════════════════════════════════════════
        _log("📡 'W' komutu gönderiliyor (alıcı bootloader'a geçiyor)...")
        _log("   ⏳ Bu işlem 30 saniyeye kadar sürebilir...")
        
        ser.write(b'W')

        ack = ser.read(1)
        if ack != b'\x06':
            _log(f"❌ ACK gelmedi! Gelen: {ack.hex() if ack else 'boş'}")
            _log("   Olası sebepler:")
            _log("   - Alıcı cihaz kapalı veya menzil dışı")
            _log("   - Gönderici Si4432 sorunu")
            _log("   - COM port yanlış")
            return False
        _log("✅ Alıcı bootloader'a geçti! RF bağlantı aktif.")

        if _stopped():
            return False

        # ═══════════════════════════════════════════════
        # 4. METADATA GÖNDER → ACK bekle
        # ═══════════════════════════════════════════════
        ser.timeout = RF_METADATA_TIMEOUT
        
        metadata = (
            firmware_size.to_bytes(4, 'little') +
            firmware_version.to_bytes(4, 'little') +
            firmware_crc.to_bytes(4, 'little')
        )
        _log(f"📋 Metadata gönderiliyor (v{firmware_version}, {firmware_size} byte)...")
        ser.write(metadata)

        ack = ser.read(1)
        if ack != b'\x06':
            _log(f"❌ Metadata reddedildi! Gelen: {ack.hex() if ack else 'boş'}")
            return False
        _log("✅ Metadata kabul edildi!")

        # ═══════════════════════════════════════════════
        # 5. FLASH SİLME BEKLENİYOR
        # ═══════════════════════════════════════════════
        _log("⏳ Flash siliniyor (119 sayfa × 2KB = 238KB)...")
        _log("   Bu işlem ~60 saniye sürebilir...")
        
        ser.timeout = RF_FLASH_ERASE_TIMEOUT

        ack = ser.read(1)
        if ack != b'\x06':
            _log(f"❌ Flash silme başarısız! Gelen: {ack.hex() if ack else 'boş'}")
            return False
        _log("✅ Flash silindi!")

        if _stopped():
            return False

        # ═══════════════════════════════════════════════
        # 6. PAKET TRANSFERİ (RF ÜZERİNDEN)
        # ═══════════════════════════════════════════════
        _log(f"🚀 RF transfer başlıyor ({total_packets} paket, {total_packets * 3} RF chunk)...")
        
        packets_sent = 0
        ser.timeout = RF_PACKET_TIMEOUT
        ser.reset_input_buffer()
        
        start_time = time.time()

        while True:
            if _stopped():
                _log("⛔ İşlem kullanıcı tarafından durduruldu.")
                return False

            packet = firmware_data.read(packet_size)
            if not packet:
                break

            # Son paket kisaysa null ile tamamla (padding)
            packet = packet.ljust(packet_size, b'\x00')

            # AES-256-CBC sifreleme:
            #   iv        : Her paket icin 16 byte rastgele (os.urandom)
            #   encrypted : 128 byte sifrelenmis firmware blogu
            #   crc_val   : Sifrelenmis verinin CRC-32 (bozulma tespiti)
            # Gonderilecek: [IV:16][Encrypted:128][CRC32:4] = 148 byte
            iv = os.urandom(16)                        # Rastgele IV
            cipher = AES.new(aes_key, AES.MODE_CBC, iv)
            encrypted = cipher.encrypt(packet)         # 128 byte sifreli veri
            crc_val = calculate_crc32(encrypted)       # Sifreli verinin CRC'si

            # 148 byte'lik tam paket olustur
            payload = iv + encrypted + crc_val.to_bytes(4, 'little')

        # Paketi gonder — NACK veya timeout durumunda RF_MAX_RETRIES kez tekrar
            success = False
            for attempt in range(1, RF_MAX_RETRIES + 1):
                ser.write(payload)           # 148 byte UART'a gonder
                time.sleep(RF_PACKET_DELAY) # RF'in 4 chunk iletmesi icin bekle

                resp_byte = ser.read(1)     # ACK (0x06) veya NACK (0x15) bekle
                if resp_byte == b'\x06':    # ACK = basarili (gonderici alicidan ACK aldi)
                    packets_sent += 1
                    success = True
                    break
                elif resp_byte == b'\x15':  # NACK = alici bir chunk'u reddetti
                    _log(f"  NACK paket {packets_sent+1} (deneme {attempt}/{RF_MAX_RETRIES})")
                    time.sleep(0.2)
                else:
                    # Bos veya bilinmeyen yanit — timeout muhtemelen
                    _log(f"  Bilinmeyen: {resp_byte.hex() if resp_byte else 'timeout'} "
                         f"(deneme {attempt}/{RF_MAX_RETRIES})")
                    time.sleep(0.5)

            if not success:
                _log(f"❌ Paket {packets_sent+1} gönderilemedi! ({RF_MAX_RETRIES} deneme)")
                return False

            _progress(packets_sent, total_packets)
            
            # Her 10 pakette süre tahmini
            if packets_sent % 10 == 0 and packets_sent > 0:
                elapsed = time.time() - start_time
                rate = packets_sent / elapsed
                remaining = (total_packets - packets_sent) / rate if rate > 0 else 0
                _log(f"\n  📊 {packets_sent}/{total_packets} | "
                     f"Hız: {rate:.1f} pkt/s | "
                     f"Kalan: ~{int(remaining)}s")

        # ═══════════════════════════════════════════════
        # 7. FİNAL DOĞRULAMA
        # ═══════════════════════════════════════════════
        elapsed = time.time() - start_time
        _log(f"\n⏳ Firmware doğrulanıyor (CRC-32 kontrolü)...")
        
        ser.timeout = RF_FINAL_TIMEOUT

        ack = ser.read(1)
        if ack == b'\x06':
            _log(f"\n{'='*50}")
            _log(f"  ✅ RF FIRMWARE GÜNCELLEMESİ BAŞARILI!")
            _log(f"  📦 {packets_sent} paket ({packets_sent * 3} RF chunk)")
            _log(f"  📌 Versiyon: v{firmware_version}")
            _log(f"  🔒 CRC: 0x{firmware_crc:08X}")
            _log(f"  ⏱️  Süre: {int(elapsed)}s ({elapsed/60:.1f} dk)")
            _log(f"  📡 Hız: {firmware_size/elapsed:.0f} byte/s")
            _log(f"{'='*50}")
            return True
        elif ack == b'\x15':
            _log("❌ Final doğrulama başarısız! (CRC uyuşmazlığı)")
            return False
        else:
            _log(f"❌ Final cevabı alınamadı: {ack.hex() if ack else 'timeout'}")
            return False

    except serial.SerialException as e:
        _log(f"❌ Seri port hatası: {e}")
        return False
    except Exception as e:
        _log(f"❌ Hata: {e}")
        import traceback
        _log(traceback.format_exc())
        return False
    finally:
        if firmware_data:
            firmware_data.close()
        if ser and ser.is_open:
            ser.close()


# ── CLI modu: doğrudan çalıştırma ──
if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(
        description="RF üzerinden STM32 firmware güncelleme",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Örnek kullanım:
  python rf_uploader.py firmware.bin
  python rf_uploader.py firmware.bin --port COM3 --version 5
  python rf_uploader.py firmware.hex --type HEX
        """
    )
    parser.add_argument("firmware_file", help="Firmware dosyası (.bin veya .hex)")
    parser.add_argument("--port", default="COM7", help="Seri port (varsayılan: COM7)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (varsayılan: 115200)")
    parser.add_argument("--version", type=int, default=1, help="Firmware versiyonu (varsayılan: 1)")
    parser.add_argument("--key", default="3132333435363738393031323334353637383930313233343536373839303132",
                       help="AES-256 key (hex string, 64 karakter)")
    parser.add_argument("--type", choices=["BIN", "HEX"], default="BIN",
                       help="Dosya formatı (varsayılan: BIN)")
    
    args = parser.parse_args()
    
    config = {
        "serial_port": args.port,
        "baud_rate": args.baud,
        "firmware_file": args.firmware_file,
        "aes_key_hex": args.key,
        "firmware_version": args.version,
        "file_type": args.type,
        "packet_size": DEFAULT_PACKET_SIZE,
    }
    
    print(f"\n{'='*50}")
    print(f"  RF Firmware Uploader")
    print(f"  Port: {args.port} @ {args.baud}")
    print(f"  Dosya: {args.firmware_file}")
    print(f"  Versiyon: {args.version}")
    print(f"{'='*50}\n")
    
    success = upload_firmware_rf(config)
    sys.exit(0 if success else 1)
