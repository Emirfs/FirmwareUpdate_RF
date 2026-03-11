import argparse
import io
import json
import os
import re
import secrets
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import parse_qs, unquote, urlparse
import hmac

from google.oauth2 import service_account
from googleapiclient.discovery import build
from googleapiclient.http import MediaIoBaseDownload


SCOPES = ["https://www.googleapis.com/auth/drive.readonly"]


def _parse_version(filename: str) -> Optional[int]:
    if not filename:
        return None
    match = re.search(r"update[_\s-]*(\d+)", filename, re.IGNORECASE)
    if not match:
        return None
    return int(match.group(1))


def _safe_filename(value: str) -> str:
    text = re.sub(r"[^A-Za-z0-9._-]", "_", value.strip())
    return text or "firmware.bin"


class OneTimeTokenStore:
    def __init__(self, ttl_seconds: int) -> None:
        self.ttl_seconds = max(30, int(ttl_seconds))
        self._lock = threading.Lock()
        self._tokens: Dict[str, Dict[str, Any]] = {}

    def issue(self, payload: Dict[str, Any]) -> str:
        token = secrets.token_urlsafe(32)
        now = int(time.time())
        with self._lock:
            self._cleanup_locked(now)
            self._tokens[token] = {
                "payload": payload,
                "exp": now + self.ttl_seconds,
                "used": False,
            }
        return token

    def consume(self, token: str) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
        now = int(time.time())
        with self._lock:
            self._cleanup_locked(now)
            record = self._tokens.get(token)
            if not record:
                return None, "gecersiz_token"
            if record["used"]:
                self._tokens.pop(token, None)
                return None, "token_kullanildi"
            if record["exp"] < now:
                self._tokens.pop(token, None)
                return None, "token_suresi_doldu"
            record["used"] = True
            payload = dict(record["payload"])
            self._tokens.pop(token, None)
            return payload, None

    def _cleanup_locked(self, now_ts: int) -> None:
        expired = [tok for tok, rec in self._tokens.items() if rec["exp"] < now_ts or rec.get("used")]
        for tok in expired:
            self._tokens.pop(tok, None)


@dataclass
class ProxyConfig:
    host: str
    port: int
    api_key: str
    service_account_json: str
    channel_map_file: str
    token_ttl_seconds: int


class FirmwareProxyState:
    def __init__(self, cfg: ProxyConfig) -> None:
        if not cfg.api_key:
            raise ValueError("FIRMWARE_PROXY_API_KEY bos olamaz.")
        if not os.path.isfile(cfg.service_account_json):
            raise FileNotFoundError(f"Service account json bulunamadi: {cfg.service_account_json}")
        self.cfg = cfg
        self.api_key = cfg.api_key
        self.channel_map = self._load_channel_map(cfg.channel_map_file)
        creds = service_account.Credentials.from_service_account_file(
            cfg.service_account_json,
            scopes=SCOPES,
        )
        self.drive = build("drive", "v3", credentials=creds)
        self.token_store = OneTimeTokenStore(cfg.token_ttl_seconds)

    def _load_channel_map(self, path: str) -> Dict[str, str]:
        if not os.path.isfile(path):
            raise FileNotFoundError(f"Kanal haritasi bulunamadi: {path}")
        with open(path, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
        if not isinstance(raw, dict):
            raise ValueError("Kanal haritasi JSON object olmalidir.")
        mapped: Dict[str, str] = {}
        for channel, folder_id in raw.items():
            ch = str(channel).strip()
            fid = str(folder_id).strip()
            if ch and fid:
                mapped[ch] = fid
        if not mapped:
            raise ValueError("Kanal haritasi bos.")
        return mapped

    def build_catalog(self, channel: str) -> Tuple[List[Dict[str, Any]], Optional[str]]:
        folder_id = self.channel_map.get(channel)
        if not folder_id:
            return [], "kanal_bulunamadi"

        query = (
            f"'{folder_id}' in parents and trashed = false and "
            "mimeType != 'application/vnd.google-apps.folder'"
        )
        results = self.drive.files().list(
            q=query,
            fields="files(id,name,size,mimeType)",
            pageSize=200,
        ).execute()
        files = results.get("files", [])

        catalog: List[Dict[str, Any]] = []
        for item in files:
            name = str(item.get("name", "")).strip()
            lower_name = name.lower()
            if not (lower_name.endswith(".bin") or lower_name.endswith(".hex")):
                continue
            file_id = str(item.get("id", "")).strip()
            if not file_id:
                continue
            token = self.token_store.issue(
                {
                    "channel": channel,
                    "file_id": file_id,
                    "name": name,
                }
            )
            catalog.append(
                {
                    "token": token,
                    "name": name,
                    "version": _parse_version(name),
                    "type": "HEX" if lower_name.endswith(".hex") else "BIN",
                    "size": item.get("size", "?"),
                }
            )

        catalog.sort(key=lambda x: (x["version"] is not None, x["version"] or 0), reverse=True)
        if not catalog:
            return [], "kanalda_firmware_yok"
        return catalog, None

    def download_via_token(self, token: str) -> Tuple[Optional[bytes], Optional[str], Optional[str]]:
        payload, err = self.token_store.consume(token)
        if err:
            return None, None, err
        if not payload:
            return None, None, "gecersiz_token"
        file_id = str(payload.get("file_id", "")).strip()
        filename = _safe_filename(str(payload.get("name", "firmware.bin")))
        if not file_id:
            return None, None, "gecersiz_file_ref"

        request = self.drive.files().get_media(fileId=file_id)
        stream = io.BytesIO()
        downloader = MediaIoBaseDownload(stream, request)
        done = False
        while not done:
            _, done = downloader.next_chunk()
        stream.seek(0)
        return stream.read(), filename, None


class FirmwareProxyHandler(BaseHTTPRequestHandler):
    server_version = "FirmwareProxy/1.0"

    def _state(self) -> FirmwareProxyState:
        return self.server.state  # type: ignore[attr-defined]

    def _json(self, status: int, body: Dict[str, Any]) -> None:
        payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _check_api_key(self) -> bool:
        expected = self._state().api_key
        provided = self.headers.get("X-Proxy-Key", "")
        if not expected:
            return True
        return hmac.compare_digest(expected, provided)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path or "/"

        if path == "/api/v1/health":
            self._json(200, {"ok": True, "ts": int(time.time())})
            return

        if not self._check_api_key():
            self._json(401, {"error": "yetkisiz"})
            return

        if path == "/api/v1/catalog":
            qs = parse_qs(parsed.query)
            channel = ""
            if "channel" in qs and qs["channel"]:
                channel = str(qs["channel"][0]).strip()
            if not channel:
                self._json(400, {"error": "channel_zorunlu"})
                return
            try:
                files, message = self._state().build_catalog(channel)
            except Exception as exc:
                self._json(500, {"error": f"katalog_hatasi: {exc}"})
                return
            self._json(
                200,
                {
                    "channel": channel,
                    "ttl_seconds": self._state().token_store.ttl_seconds,
                    "message": message,
                    "files": files,
                },
            )
            return

        if path.startswith("/api/v1/download/"):
            token = unquote(path.split("/api/v1/download/", 1)[1]).strip()
            if not token:
                self._json(400, {"error": "token_zorunlu"})
                return
            try:
                data, filename, err = self._state().download_via_token(token)
            except Exception as exc:
                self._json(500, {"error": f"indirme_hatasi: {exc}"})
                return
            if err or data is None:
                self._json(404, {"error": err or "dosya_bulunamadi"})
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="{filename or "firmware.bin"}"')
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        self._json(404, {"error": "bulunamadi"})

    def log_message(self, fmt: str, *args: Any) -> None:
        now = time.strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{now}] {self.client_address[0]} - {fmt % args}")


class FirmwareProxyHTTPServer(ThreadingHTTPServer):
    def __init__(self, server_address: Tuple[str, int], state: FirmwareProxyState):
        super().__init__(server_address, FirmwareProxyHandler)
        self.state = state


def _default_config() -> ProxyConfig:
    base_dir = os.path.dirname(os.path.abspath(__file__))
    return ProxyConfig(
        host=os.environ.get("FIRMWARE_PROXY_HOST", "127.0.0.1"),
        port=int(os.environ.get("FIRMWARE_PROXY_PORT", "8787")),
        api_key=os.environ.get("FIRMWARE_PROXY_API_KEY", "").strip(),
        service_account_json=os.environ.get("FIRMWARE_PROXY_SA_JSON", "").strip(),
        channel_map_file=os.environ.get(
            "FIRMWARE_PROXY_CHANNEL_MAP_FILE",
            os.path.join(base_dir, "proxy_channels.json"),
        ).strip(),
        token_ttl_seconds=int(os.environ.get("FIRMWARE_PROXY_TOKEN_TTL_SEC", "120")),
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Firmware proxy server (Drive IDs server-side only).")
    parser.add_argument("--host", default=None, help="Bind address")
    parser.add_argument("--port", type=int, default=None, help="Bind port")
    parser.add_argument("--api-key", default=None, help="X-Proxy-Key value")
    parser.add_argument("--sa-json", default=None, help="Google service account JSON path")
    parser.add_argument("--channel-map", default=None, help="Channel -> Drive folder map JSON")
    parser.add_argument("--ttl-sec", type=int, default=None, help="Token TTL in seconds")
    args = parser.parse_args()

    cfg = _default_config()
    if args.host:
        cfg.host = args.host
    if args.port:
        cfg.port = args.port
    if args.api_key is not None:
        cfg.api_key = args.api_key.strip()
    if args.sa_json:
        cfg.service_account_json = args.sa_json
    if args.channel_map:
        cfg.channel_map_file = args.channel_map
    if args.ttl_sec:
        cfg.token_ttl_seconds = args.ttl_sec

    state = FirmwareProxyState(cfg)
    server = FirmwareProxyHTTPServer((cfg.host, cfg.port), state)

    print("Firmware proxy server started")
    print(f"Address: http://{cfg.host}:{cfg.port}")
    print(f"Token TTL: {state.token_store.ttl_seconds} sec")
    print(f"Channel map: {cfg.channel_map_file}")
    print("Endpoints:")
    print("  GET /api/v1/health")
    print("  GET /api/v1/catalog?channel=<name>")
    print("  GET /api/v1/download/<token>")
    print("")
    print("Note: Production deployment should be behind TLS reverse proxy.")
    server.serve_forever()


if __name__ == "__main__":
    main()
