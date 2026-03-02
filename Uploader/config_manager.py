"""
Config & Credentials Manager
- credentials.enc: Admin kullanıcı adı + şifre hash (ayrı dosya, reset'ten etkilenmez)
- config.enc: Cihaz profilleri + genel ayarlar (admin şifresiyle şifreli)
- Varsayılan admin/admin hardcoded hash ile
"""
import json
import os
import sys
import hashlib
from Crypto.Cipher import AES as CryptoAES
from Crypto.Util.Padding import pad, unpad
from Crypto.Random import get_random_bytes

# ── Dizin Tespiti ──
if getattr(sys, 'frozen', False):
    _APP_DIR = os.path.dirname(sys.executable)
else:
    _APP_DIR = os.path.dirname(os.path.abspath(__file__))

CONFIG_FILE = os.path.join(_APP_DIR, "config.enc")
CREDENTIALS_FILE = os.path.join(_APP_DIR, "credentials.enc")

PBKDF2_ITERATIONS = 100_000
SALT_SIZE = 16
IV_SIZE = 16

# ══════════════════════════════════════════════════════════════
# Varsayılan Admin Bilgileri (hardcoded)
# ══════════════════════════════════════════════════════════════

_DEFAULT_ADMIN_USERNAME = "admin"
_DEFAULT_ADMIN_PASSWORD = "admin"
# Sabit salt (hardcoded default için)
_DEFAULT_CRED_SALT = bytes.fromhex("a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6")
_DEFAULT_ADMIN_HASH = hashlib.pbkdf2_hmac(
    'sha256',
    _DEFAULT_ADMIN_PASSWORD.encode('utf-8'),
    _DEFAULT_CRED_SALT,
    PBKDF2_ITERATIONS
).hex()

# Credentials dosyasını şifrelemek için sabit anahtar (credentials.enc koruma)
_CRED_ENC_KEY = hashlib.sha256(b"SmartHomeFW_CredKey_2026").digest()

# ══════════════════════════════════════════════════════════════
# Varsayılan Config
# ══════════════════════════════════════════════════════════════

DEFAULT_CONFIG = {
    "devices": [],
    "serial_port": "COM7",
    "baud_rate": 115200,
    "packet_size": 128,
    "max_retries": 7,
    "service_account_json": "C:\\Users\\Emir Furkan\\Desktop\\FirmwareUpdate\\eng-name-487012-d5-f4a48c3112a6.json",  # Google Drive Service Account JSON dosya yolu
}


# ══════════════════════════════════════════════════════════════
# Şifreleme / Çözme Yardımcıları
# ══════════════════════════════════════════════════════════════

def _derive_key(password: str, salt: bytes) -> bytes:
    """PBKDF2 ile şifreden 32-byte AES key türet."""
    return hashlib.pbkdf2_hmac('sha256', password.encode('utf-8'), salt, PBKDF2_ITERATIONS)


def _encrypt_data(data: bytes, key: bytes) -> bytes:
    """AES-256-CBC ile şifrele. Dönen: salt(16) + iv(16) + ciphertext"""
    salt = get_random_bytes(SALT_SIZE)
    iv = get_random_bytes(IV_SIZE)
    cipher = CryptoAES.new(key, CryptoAES.MODE_CBC, iv)
    ciphertext = cipher.encrypt(pad(data, CryptoAES.block_size))
    return salt + iv + ciphertext


def _decrypt_data(raw: bytes, key: bytes) -> bytes:
    """AES-256-CBC ile çöz."""
    iv = raw[SALT_SIZE:SALT_SIZE + IV_SIZE]
    ciphertext = raw[SALT_SIZE + IV_SIZE:]
    cipher = CryptoAES.new(key, CryptoAES.MODE_CBC, iv)
    return unpad(cipher.decrypt(ciphertext), CryptoAES.block_size)


# ══════════════════════════════════════════════════════════════
# Credentials Yönetimi
# ══════════════════════════════════════════════════════════════

def _get_default_credentials() -> dict:
    """Hardcoded varsayılan credentials."""
    return {
        "username": _DEFAULT_ADMIN_USERNAME,
        "password_hash": _DEFAULT_ADMIN_HASH,
        "password_salt": _DEFAULT_CRED_SALT.hex(),
    }


def credentials_exist() -> bool:
    return os.path.isfile(CREDENTIALS_FILE)


def load_credentials() -> dict:
    """credentials.enc varsa dosyadan yükle, yoksa hardcoded default döndür."""
    if not credentials_exist():
        return _get_default_credentials()
    try:
        with open(CREDENTIALS_FILE, 'rb') as f:
            raw = f.read()
        plaintext = _decrypt_data(raw, _CRED_ENC_KEY)
        return json.loads(plaintext.decode('utf-8'))
    except Exception:
        return _get_default_credentials()


def save_credentials(creds: dict):
    """Credentials'ı şifreli dosyaya kaydet."""
    plaintext = json.dumps(creds, ensure_ascii=False).encode('utf-8')
    encrypted = _encrypt_data(plaintext, _CRED_ENC_KEY)
    with open(CREDENTIALS_FILE, 'wb') as f:
        f.write(encrypted)


def verify_admin(username: str, password: str) -> bool:
    """Admin kullanıcı adı ve şifresini doğrula."""
    creds = load_credentials()
    if username != creds.get("username", ""):
        return False
    salt = bytes.fromhex(creds.get("password_salt", ""))
    expected_hash = creds.get("password_hash", "")
    actual_hash = hashlib.pbkdf2_hmac(
        'sha256', password.encode('utf-8'), salt, PBKDF2_ITERATIONS
    ).hex()
    return actual_hash == expected_hash


def change_admin_credentials(new_username: str, new_password: str):
    """Admin bilgilerini değiştir ve kaydet."""
    salt = get_random_bytes(SALT_SIZE)
    pw_hash = hashlib.pbkdf2_hmac(
        'sha256', new_password.encode('utf-8'), salt, PBKDF2_ITERATIONS
    ).hex()
    creds = {
        "username": new_username,
        "password_hash": pw_hash,
        "password_salt": salt.hex(),
    }
    save_credentials(creds)
    return creds


# ══════════════════════════════════════════════════════════════
# Config Yönetimi (cihaz profilleri + ayarlar)
# ══════════════════════════════════════════════════════════════

def config_exists() -> bool:
    return os.path.isfile(CONFIG_FILE)


def save_config(config: dict, admin_password: str):
    """Config'i admin şifresiyle AES-256-CBC ile şifrele."""
    salt = get_random_bytes(SALT_SIZE)
    key = _derive_key(admin_password, salt)
    iv = get_random_bytes(IV_SIZE)
    plaintext = json.dumps(config, ensure_ascii=False).encode('utf-8')
    cipher = CryptoAES.new(key, CryptoAES.MODE_CBC, iv)
    ciphertext = cipher.encrypt(pad(plaintext, CryptoAES.block_size))
    with open(CONFIG_FILE, 'wb') as f:
        f.write(salt + iv + ciphertext)


def load_config(admin_password: str) -> dict:
    """Config dosyasını admin şifresiyle çöz."""
    if not config_exists():
        return DEFAULT_CONFIG.copy()
    with open(CONFIG_FILE, 'rb') as f:
        data = f.read()
    salt = data[:SALT_SIZE]
    iv = data[SALT_SIZE:SALT_SIZE + IV_SIZE]
    ciphertext = data[SALT_SIZE + IV_SIZE:]
    key = _derive_key(admin_password, salt)
    cipher = CryptoAES.new(key, CryptoAES.MODE_CBC, iv)
    try:
        plaintext = unpad(cipher.decrypt(ciphertext), CryptoAES.block_size)
        loaded_config = json.loads(plaintext.decode('utf-8'))
        
        # Merge with default config to ensure new keys exist
        config = DEFAULT_CONFIG.copy()
        config.update(loaded_config)
        return config
    except (ValueError, json.JSONDecodeError):
        raise ValueError("Şifre yanlış veya config dosyası bozuk!")


def reset_config():
    """Config dosyasını sil (credentials.enc korunur!)."""
    if config_exists():
        os.remove(CONFIG_FILE)
