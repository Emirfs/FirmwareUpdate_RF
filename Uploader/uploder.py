import serial
import time
import requests
import io
import os
import sys
import re
from Crypto.Cipher import AES
import zlib

try:
    from intelhex import IntelHex
    INTELHEX_AVAILABLE = True
except ImportError:
    INTELHEX_AVAILABLE = False

# X25519 ECDH key exchange
try:
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
    X25519_AVAILABLE = True
except ImportError:
    X25519_AVAILABLE = False

DRIVE_URL_TEMPLATE = "https://drive.google.com/uc?export=download&id={}"
DEFAULT_PACKET_SIZE = 128
KEY_UPDATE_MAGIC = b'\xA5\xA5\xA5\xA5'
RF_CMD_KEY_UPDATE = 0x08


def _compute_crc8(data: bytes) -> int:
    """CRC-8 (polinom 0x07) — boot_storage.c keystore_crc8 ile eslesir."""
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def _ecdh_rf_handshake(ser, new_master_key: bytes = None, aes_key_fallback: bytes = None):
    """
    RF mode ECDH handshake.

    Gonderir : 'W'(1) + pub_sender(32) = 33 byte
    Bekler   : pub_receiver(32) + ACK(1) = 33 byte
    Hesaplar : session_key = X25519(priv, pub_receiver)

    new_master_key verilirse KEY_UPDATE paketi de gonderilir.
    Donus: session_key (bytes, 32) veya None (basarisiz/ECDH yok)
    """
    if not X25519_AVAILABLE:
        # Eski protokol: sadece 'W' gonder, 1 byte ACK bekle
        # ECDH yok: 'W' gonder, C sender yine de 33 byte donduruyor
        # (pub_receiver 32B + ACK 1B). Hepsini oku, son byte ACK olmali.
        ser.write(b'W')
        response = ser.read(33)
        if len(response) < 33 or response[32:33] != b'\x06':
            return None
        return aes_key_fallback  # Session key yok — fallback AES key kullan

    private_key = X25519PrivateKey.generate()
    pub_sender = private_key.public_key().public_bytes_raw()  # 32 byte

    ser.write(b'W' + pub_sender)  # 33 byte

    response = ser.read(33)  # pub_receiver(32) + ACK(1)
    if len(response) < 33 or response[32:33] != b'\x06':
        return None

    pub_receiver_key = X25519PublicKey.from_public_bytes(response[:32])
    session_key = private_key.exchange(pub_receiver_key)

    # Opsiyonel: yeni master key gonder
    if new_master_key is not None and len(new_master_key) == 32:
        zero_iv = b'\x00' * 16
        cipher = AES.new(session_key, AES.MODE_CBC, zero_iv)
        enc_key = cipher.encrypt(new_master_key)
        crc8 = _compute_crc8(new_master_key)
        ser.write(bytes([RF_CMD_KEY_UPDATE]) + enc_key + bytes([crc8]))
        ku_resp = ser.read(1)
        if ku_resp != b'\x06':
            return None  # KEY_UPDATE basarisiz

    return session_key


def hex_to_bin(hex_data: bytes) -> bytes:
    """
    Intel HEX formatındaki veriyi raw binary'ye dönüştürür.
    intelhex kütüphanesi varsa onu kullanır, yoksa manuel parse eder.
    """
    if INTELHEX_AVAILABLE:
        ih = IntelHex()
        ih.loadhex(io.StringIO(hex_data.decode('ascii', errors='ignore')))
        start = ih.minaddr()
        end = ih.maxaddr()
        return ih.tobinarray(start=start, size=end - start + 1).tobytes()
    
    # Fallback: Manuel Intel HEX parser
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
        
        if record_type == 0x00:  # Data record
            data = bytes.fromhex(line[9:9 + byte_count * 2])
            full_addr = base_address + address
            for i, b in enumerate(data):
                data_blocks[full_addr + i] = b
        elif record_type == 0x02:  # Extended Segment Address
            base_address = int(line[9:13], 16) << 4
        elif record_type == 0x04:  # Extended Linear Address
            base_address = int(line[9:13], 16) << 16
        elif record_type == 0x01:  # End of File
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
    return zlib.crc32(data) & 0xFFFFFFFF


def progress_bar(current, total, width=40):
    """Terminal'de ilerleme çubuğu göster."""
    percent = current * 100 // total
    filled = width * current // total
    bar = '█' * filled + '░' * (width - filled)
    print(f"\r  [{bar}] {percent}% ({current}/{total})", end='', flush=True)


def update_stm32_key(config, new_key_hex, log=None):
    """
    Alıcı STM32'deki AES master key'i RF üzerinden güvenli güncelle.

    Protokol (yeni — ECDH):
      Firmware güncelleme akışını tetikler + KEY_UPDATE paketi gönderir.
      ECDH session key ile şifreli → dinleyen çözemez.

    Gerçek firmware güncellemesi YAPMAZ — sadece dummy firmware
    ile handshake yapılır, KEY_UPDATE gönderilir, bağlantı kesilir.
    """
    def _log(msg):
        if log:
            log(msg)
        else:
            print(msg)

    # Mevcut key
    current_key_hex = config.get("aes_key_hex", "")
    try:
        current_key = bytes.fromhex(current_key_hex)
    except ValueError:
        current_key = current_key_hex.encode('utf-8')
    if len(current_key) != 32:
        _log("❌ Mevcut AES key 32 byte olmalı!")
        return False

    # Yeni key
    try:
        new_key = bytes.fromhex(new_key_hex)
    except ValueError:
        new_key = new_key_hex.encode('utf-8')
    if len(new_key) != 32:
        _log("❌ Yeni AES key 32 byte (64 hex karakter) olmalı!")
        return False

    # Yeni key geçerlilik kontrolü
    if new_key == b'\x00' * 32 or new_key == b'\xFF' * 32:
        _log("❌ Yeni key tamamen 0x00 veya 0xFF olamaz!")
        return False

    serial_port = config.get("serial_port", "COM7")
    baud_rate = config.get("baud_rate", 115200)

    ser = None
    try:
        _log(f"🔌 {serial_port} açılıyor...")
        ser = serial.Serial(serial_port, baud_rate, timeout=45)
        time.sleep(2)
        ser.reset_input_buffer()

        _log("🔐 ECDH key exchange + KEY_UPDATE başlatılıyor...")
        _log("   ⏳ Alıcının bootloader'a geçmesi bekleniyor (max 30 sn)...")

        session_key = _ecdh_rf_handshake(ser, new_master_key=new_key,
                                          aes_key_fallback=current_key)
        if session_key is None:
            _log("❌ ECDH handshake başarısız! Alıcı kapalı veya RF sorunu.")
            return False

        _log("✅ Yeni master key alıcı Flash'ına yazıldı (ECDH korumalı)!")
        _log("ℹ️  GUI'deki AES Key alanını yeni key ile güncelleyip kaydedin.")
        return True

    except serial.SerialException as e:
        _log(f"❌ Seri port hatası: {e}")
        return False
    except Exception as e:
        _log(f"❌ Hata: {e}")
        return False
    finally:
        if ser and ser.is_open:
            ser.close()


def upload_firmware(config, log=None, on_progress=None, stop_flag=None, download_client=None):
    """
    Firmware güncelleme işlemini yönetir.
    
    Args:
        config (dict): Konfigürasyon (port, baud, indirme token'i veya file_id, key, vb.)
        log (func): Log mesajlarını ekrana/GUI'ye basan callback
        on_progress (func): İlerleme durumunu (current, total) bildiren callback
        stop_flag (func): İşlemi durdurmak için True dönen fonksiyon
        download_client: Firmware indirme istemcisi (proxy veya direct Drive)
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

    # Config'den ayarları oku
    serial_port = config.get("serial_port", "COM7")
    baud_rate = config.get("baud_rate", 115200)
    drive_file_id = config.get("drive_file_id", "")
    download_token = config.get("download_token", "")
    aes_key_hex = config.get("aes_key_hex", "")
    max_retries = config.get("max_retries", 7)
    firmware_version = config.get("firmware_version", 1)
    packet_size = config.get("packet_size", DEFAULT_PACKET_SIZE)
    file_type = config.get("file_type", "BIN").upper()
    filename = config.get("filename", "")
    is_rf = config.get("is_rf", False)

    # Dinamik timeout'lar (Kablolu vs RF)
    TIMEOUT_HANDSHAKE = 45 if is_rf else 15
    TIMEOUT_METADATA = 15 if is_rf else 15
    TIMEOUT_FLASH = 90 if is_rf else 30
    TIMEOUT_PACKET = 30 if is_rf else 2
    TIMEOUT_FINAL = 90 if is_rf else 15
    MAX_RETRIES = 10 if is_rf else max_retries
    PACKET_DELAY = 0.5 if is_rf else 0.05

    # AES key dönüşümü
    try:
        aes_key = bytes.fromhex(aes_key_hex)
        if len(aes_key) != 32:
            _log("❌ AES key 32 byte (64 hex karakter) olmalıdır!")
            return False
    except ValueError:
        # Hex değilse ASCII olarak dene
        aes_key = aes_key_hex.encode('utf-8')
        if len(aes_key) != 32:
            _log("❌ AES key 32 byte olmalıdır!")
            return False

    bin_file_url = DRIVE_URL_TEMPLATE.format(drive_file_id)

    ser = None
    firmware_data = None

    try:
        # ═══════════════════════════════════════════════
        # 1. DOSYAYI İNDİR
        # ═══════════════════════════════════════════════
        _log(f"📥 Firmware indiriliyor... ({file_type})")
        
        firmware_data = None
        
        if download_client:
            download_ref = download_token or drive_file_id
            if not download_ref:
                _log("❌ İndirme kaynağı tanımlı değil!")
                return False

            # RAM'e indir (BytesIO)
            f_data, err = download_client.download_file_to_memory(download_ref, progress_callback=lambda p: None)
            if not f_data:
                _log(f"❌ İndirme başarısız: {err}")
                return False
            
            raw_firmware = f_data.read()

        # Yoksa eski yöntem (Requests)
        else:
            if not drive_file_id:
                _log("❌ Drive dosya ID tanımlı değil!")
                return False
            resp = requests.get(bin_file_url, timeout=30)
            resp.raise_for_status()
            if 'text/html' in resp.headers.get('Content-Type', ''):
                _log("❌ İndirilen dosya binary değil! Drive ID'yi kontrol edin.")
                return False
            raw_firmware = resp.content

        if _stopped():
            return False

        # HEX dosyasıysa binary'ye dönüştür
        if file_type == "HEX" or filename.lower().endswith('.hex'):
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
        
        if is_rf:
            estimated = total_packets * 3
            _log(f"⏱️  Tahmini süre (RF): ~{estimated // 60} dk {estimated % 60} sn")

        firmware_data = io.BytesIO(raw_firmware)

        # ═══════════════════════════════════════════════
        # 2. SERİ PORT AÇ
        # ═══════════════════════════════════════════════
        _log(f"🔌 {serial_port} açılıyor... (RF Modu: {'AÇIK' if is_rf else 'KAPALI'})")
        ser = serial.Serial(serial_port, baud_rate, timeout=TIMEOUT_HANDSHAKE)
        time.sleep(3 if is_rf else 2)
        ser.reset_input_buffer()

        # ═══════════════════════════════════════════════
        # 3. HANDSHAKE (RF modunda ECDH, kabloluda eski protokol)
        # ═══════════════════════════════════════════════
        time.sleep(0.1)
        ser.reset_input_buffer()

        if is_rf:
            _log("🔐 ECDH key exchange başlatılıyor (alıcı bootloader'a geçiyor)...")
            _log("   ⏳ Bu işlem 30 saniyeye kadar sürebilir...")
            new_master_key_hex = config.get("new_master_key_hex", None)
            new_master_key = bytes.fromhex(new_master_key_hex) if new_master_key_hex else None
            session_key = _ecdh_rf_handshake(ser, new_master_key=new_master_key,
                                              aes_key_fallback=aes_key)
            if session_key is None:
                _log("❌ ECDH handshake başarısız! Alıcı kapalı veya RF sorunu.")
                return False
            if X25519_AVAILABLE:
                _log("✅ ECDH tamamlandı — session key türetildi (havada görünmez)")
            else:
                _log("✅ ACK alındı (ECDH yok — fallback key)")
        else:
            _log("📡 'W' komutu gönderiliyor...")
            ser.write(b'W')
            ack = ser.read(1)
            if ack != b'\x06':
                _log(f"❌ ACK gelmedi! Gelen: {ack.hex() if ack else 'boş'}")
                return False
            session_key = aes_key
            _log("✅ ACK alındı — STM32 hazır!")

        if _stopped():
            return False

        # ═══════════════════════════════════════════════
        # 4. KOMUT BYTE + METADATA GÖNDER → ACK bekle
        #    C sender her zaman 1 byte komut bekler:
        #    0x00 = KEY_UPDATE yok, doğrudan metadata
        #    (KEY_UPDATE zaten handshake sırasında gönderildi)
        # ═══════════════════════════════════════════════
        ser.timeout = TIMEOUT_METADATA
        ser.write(b'\x00')  # Komut byte: KEY_UPDATE yok
        metadata = (
            firmware_size.to_bytes(4, 'little') +
            firmware_version.to_bytes(4, 'little') +
            firmware_crc.to_bytes(4, 'little')
        )
        ser.write(metadata)

        ack = ser.read(1)
        if ack != b'\x06':
            _log(f"❌ Metadata reddedildi! Gelen: {ack.hex() if ack else 'boş'}")
            return False
        _log("✅ Metadata kabul edildi!")

        # ═══════════════════════════════════════════════
        # 5. FLASH SİLME BEKLENİYOR
        # ═══════════════════════════════════════════════
        _log(f"⏳ Flash siliniyor (bu ~{TIMEOUT_FLASH//2 if not is_rf else TIMEOUT_FLASH} saniye sürebilir)...")
        ser.timeout = TIMEOUT_FLASH

        ack = ser.read(1)
        if ack != b'\x06':
            _log(f"❌ Flash silme başarısız! Gelen: {ack.hex() if ack else 'boş'}")
            return False
        _log("✅ Flash silindi!")

        if _stopped():
            return False

        # ═══════════════════════════════════════════════
        # 6. PAKET TRANSFERİ
        # ═══════════════════════════════════════════════
        _log(f"🚀 Transfer başlıyor ({total_packets} paket)...")
        packets_sent = 0
        ser.timeout = TIMEOUT_PACKET
        ser.reset_input_buffer()
        
        start_time = time.time()

        while True:
            if _stopped():
                _log("⛔ İşlem kullanıcı tarafından durduruldu.")
                return False

            packet = firmware_data.read(packet_size)
            if not packet:
                break

            packet = packet.ljust(packet_size, b'\x00')
            iv = os.urandom(16)
            cipher = AES.new(session_key, AES.MODE_CBC, iv)
            encrypted = cipher.encrypt(packet)
            crc_val = calculate_crc32(encrypted)

            payload = iv + encrypted + crc_val.to_bytes(4, 'little')

            success = False
            for attempt in range(1, MAX_RETRIES + 1):
                ser.write(payload)
                time.sleep(PACKET_DELAY)
                resp_byte = ser.read(1)
                if resp_byte == b'\x06':
                    packets_sent += 1
                    success = True
                    break
                elif resp_byte == b'\x15':
                    _log(f"  ⚠️  NAK paket {packets_sent+1} (deneme {attempt}/{MAX_RETRIES})")
                    time.sleep(0.1)
                else:
                    _log(f"  ❓ Bilinmeyen: {resp_byte.hex() if resp_byte else 'boş/timeout'} (deneme {attempt}/{MAX_RETRIES})")
                    time.sleep(0.5)

            if not success:
                _log(f"❌ Paket {packets_sent+1} gönderilemedi! ({MAX_RETRIES} deneme)")
                return False

            _progress(packets_sent, total_packets)
            
            # RF modunda ek hız bilgisi
            if is_rf and packets_sent % 10 == 0 and packets_sent > 0:
                elapsed = time.time() - start_time
                rate = packets_sent / elapsed
                remaining = (total_packets - packets_sent) / rate if rate > 0 else 0
                _log(f"\n  📊 {packets_sent}/{total_packets} | Kalan: ~{int(remaining)}s")

        # ═══════════════════════════════════════════════
        # 7. FİNAL DOĞRULAMA
        # ═══════════════════════════════════════════════
        _log("\n⏳ Firmware doğrulanıyor (CRC-32)...")
        ser.timeout = TIMEOUT_FINAL

        ack = ser.read(1)
        if ack == b'\x06':
            _log(f"{'='*45}")
            _log(f"  ✅ GÜNCELLEME BAŞARILI!")
            _log(f"  📦 {packets_sent} paket | v{firmware_version}")
            _log(f"  🔒 CRC: 0x{firmware_crc:08X}")
            _log(f"{'='*45}")
            return True
        else:
            _log("❌ Final doğrulama başarısız!")
            return False

    except serial.SerialException as e:
        _log(f"❌ Seri port hatası: {e}")
        return False
    except requests.RequestException as e:
        _log(f"❌ İndirme hatası: {e}")
        return False
    except Exception as e:
        _log(f"❌ Hata: {e}")
        return False
    finally:
        if firmware_data:
            firmware_data.close()
        if ser and ser.is_open:
            ser.close()


# ── CLI modu: doğrudan çalıştırma ──
if __name__ == "__main__":
    # Geriye uyumluluk: eski sabit değerlerle çalış
    config = {
        "serial_port": "COM7",
        "baud_rate": 115200,
        "drive_file_id": "1YOQiPoHZ2D2RTP8xroTUG9fAXh1dliGZ",
        "aes_key_hex": "3132333435363738393031323334353637383930313233343536373839303132",
        "packet_size": 128,
        "max_retries": 7,
        "firmware_version": 2
    }
    upload_firmware(config)
