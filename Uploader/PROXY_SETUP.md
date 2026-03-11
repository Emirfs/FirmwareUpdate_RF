# Proxy Setup (Drive ID gizli model)

Bu modelde istemci tarafina Drive ID verilmez. GUI sadece `firmware_channel` ile katalog ister, backend tek kullanimlik kisa omurlu token dondurur.

## 1) Kanal haritasini hazirla

`Uploader/proxy_channels.json` dosyasinda kanal -> Drive klasor ID eslesmesi server tarafinda tutulur:

```json
{
  "urun-a-seri-1": "GOOGLE_DRIVE_FOLDER_ID_1"
}
```

## 2) Ortam degiskenleri

PowerShell:

```powershell
$env:FIRMWARE_PROXY_API_KEY="cok-gizli-api-key"
$env:FIRMWARE_PROXY_SA_JSON="C:\path\to\service_account.json"
$env:FIRMWARE_PROXY_CHANNEL_MAP_FILE="C:\path\to\proxy_channels.json"
$env:FIRMWARE_PROXY_TOKEN_TTL_SEC="120"
```

## 3) Proxy server'i calistir

```powershell
python Uploader\firmware_proxy_server.py --host 0.0.0.0 --port 8787
```

## 4) GUI tarafi

- `DEFAULT_CONFIG` proxy-first olarak ayarlidir.
- Admin panelindeki backend alanina `https://proxy-adresi` gir.
- Cihazlarda `drive_file_id` yerine `firmware_channel` kullan.

## Notlar

- `GET /api/v1/catalog?channel=...` token listesi dondurur.
- `GET /api/v1/download/<token>` token'i tek sefer kullanir.
- Token suresi doldugunda yeni katalog cagrisi gerekir.
