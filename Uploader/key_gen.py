"""
key_gen.py — Ed25519 anahtar çifti üretici ve firmware imzalama aracı

Kullanım:
    pip install cryptography
    python key_gen.py              # Yeni anahtar çifti üret
    python key_gen.py --verify     # Mevcut anahtar çiftini doğrula

Çıktılar:
    private_key.pem      — Ed25519 özel anahtar (ASLA paylaşma, ASLA repoya ekleme!)
    public_key_bytes.txt — rf_bootloader.h'e kopyalanacak C dizisi

Güvenlik notları:
    - private_key.pem yalnızca bu PC'de saklanır.
    - public_key_bytes.txt bootloader flash'ına gömülür (paylaşmak güvenlidir).
    - Özel anahtarsız geçerli imza üretilemez.
    - Ed25519 kullanımı: RFC 8032, Bölüm 5.1 (SHA-512 tabanlı).
"""

import os
import sys
import hashlib

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey, Ed25519PublicKey
    )
    from cryptography.hazmat.primitives import serialization
except ImportError:
    print("Eksik kütüphane: pip install cryptography")
    sys.exit(1)


def generate_keys(output_dir="."):
    """Ed25519 anahtar çifti üret ve dosyalara kaydet."""
    priv_path = os.path.join(output_dir, "private_key.pem")
    pub_path  = os.path.join(output_dir, "public_key_bytes.txt")

    if os.path.exists(priv_path):
        ans = input(f"UYARI: {priv_path} zaten mevcut! Üzerine yaz? (evet/hayir): ")
        if ans.strip().lower() != "evet":
            print("İptal edildi.")
            return

    # Ed25519 anahtar çifti üret
    private_key = Ed25519PrivateKey.generate()
    public_key  = private_key.public_key()

    # Özel anahtarı şifresiz PEM olarak kaydet
    pem = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()
    )
    with open(priv_path, "wb") as f:
        f.write(pem)
    print(f"[OK] {priv_path} oluşturuldu")

    # Açık anahtar: 32 byte raw (Ed25519 = sadece y koordinatı)
    pub_raw = public_key.public_bytes_raw()  # 32 byte
    assert len(pub_raw) == 32

    # rf_bootloader.h için C dizisi formatla
    lines = ["static const uint8_t ED25519_PUBLIC_KEY[32] = {"]
    for i in range(0, 32, 8):
        chunk = pub_raw[i:i+8]
        row = "    " + ", ".join(f"0x{b:02X}" for b in chunk)
        if i + 8 < 32:
            row += ","
        lines.append(row)
    lines.append("};")
    c_array = "\n".join(lines)

    with open(pub_path, "w") as f:
        f.write(c_array + "\n")
    print(f"[OK] {pub_path} oluşturuldu")

    print("\n" + "=" * 64)
    print("rf_bootloader.h içindeki ED25519_PUBLIC_KEY'i şununla değiştirin:")
    print("=" * 64)
    print(c_array)
    print("=" * 64)
    print("\nSONRAKİ ADIM:")
    print("  Yukarıdaki diziyi alici_cihaz/Core/Inc/rf_bootloader.h'deki")
    print("  ED25519_PUBLIC_KEY tanımının üzerine yapıştırın.")
    print("  Sonra alici_cihaz projesini yeniden derleyin ve flash'layın.")


def sign_firmware(firmware_bytes: bytes,
                  private_key_path: str = "private_key.pem") -> bytes:
    """
    Firmware binary'sini Ed25519 ile imzala.

    İmzalanan veri: SHA-256(padded_firmware) — 32 byte
    Neden SHA-256 hash: Firmware büyük olabilir; Ed25519 arbitrary mesaj
    alır ama hash'i dışarıda hesaplamak protokol senkronizasyonunu kolaylaştırır.
    Bootloader da aynı SHA-256'yı zaten akış sırasında hesaplıyor.

    Dönüş: 64 byte Ed25519 imzası (R[32] || S[32], küçük-endian)
    """
    with open(private_key_path, "rb") as f:
        private_key = serialization.load_pem_private_key(f.read(), password=None)

    if not isinstance(private_key, Ed25519PrivateKey):
        raise TypeError("private_key.pem bir Ed25519 anahtarı değil! key_gen.py ile yeniden üretin.")

    # Firmware hash'i — padding dahil (receiver ile aynı hesap)
    # Not: Padding uploder.py'de hesaplanmış firmware_bytes'a zaten uygulanmış olmalı
    fw_hash = hashlib.sha256(firmware_bytes).digest()  # 32 byte

    # Ed25519 imzala (dahili SHA-512 ile RFC 8032 Bölüm 5.1)
    signature = private_key.sign(fw_hash)  # 64 byte
    assert len(signature) == 64
    return signature


def verify_signature(firmware_bytes: bytes,
                     signature: bytes,
                     public_key_path: str = "public_key_bytes.txt") -> bool:
    """Python tarafında doğrulama (test/debug için)."""
    # public_key_bytes.txt'ten C dizisini parse et
    with open(public_key_path) as f:
        content = f.read()

    hex_vals = []
    for token in content.replace("{", "").replace("}", "").replace(";", "").split(","):
        token = token.strip().split()[-1] if token.strip().split() else ""
        if token.startswith("0x") or token.startswith("0X"):
            try:
                hex_vals.append(int(token, 16))
            except ValueError:
                pass

    if len(hex_vals) != 32:
        raise ValueError(f"Public key 32 byte olmalı, {len(hex_vals)} bulundu")

    pub_raw = bytes(hex_vals)
    public_key = Ed25519PublicKey.from_public_bytes(pub_raw)
    fw_hash = hashlib.sha256(firmware_bytes).digest()

    try:
        public_key.verify(signature, fw_hash)
        return True
    except Exception:
        return False


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Ed25519 anahtar üretici")
    parser.add_argument("--verify", action="store_true",
                        help="Mevcut anahtar çiftini test et")
    args = parser.parse_args()

    output_dir = os.path.dirname(os.path.abspath(__file__))

    if args.verify:
        priv = os.path.join(output_dir, "private_key.pem")
        pub  = os.path.join(output_dir, "public_key_bytes.txt")
        if not os.path.exists(priv):
            print(f"HATA: {priv} bulunamadı. Önce key_gen.py çalıştırın.")
            sys.exit(1)
        test_data = b"test firmware payload" * 100
        sig = sign_firmware(test_data, priv)
        ok = verify_signature(test_data, sig, pub)
        print(f"Doğrulama: {'BAŞARILI ✓' if ok else 'BAŞARISIZ ✗'}")
        sys.exit(0 if ok else 1)
    else:
        generate_keys(output_dir=output_dir)
