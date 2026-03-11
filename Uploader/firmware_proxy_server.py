import argparse
import base64
import hashlib
import hmac
import io
import json
import os
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional, Tuple

from drive_manager import DriveManager


def _json_bytes(payload: Dict[str, Any]) -> bytes:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")


def _b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64url_decode(data: str) -> bytes:
    padding = "=" * (-len(data) % 4)
    return base64.urlsafe_b64decode(data + padding)


class ProxyState:
    def __init__(self, api_key: str, service_json: str, channel_map_file: str, token_ttl: int) -> None:
        self.api_key = api_key
        self.service_json = service_json
        self.channel_map_file = channel_map_file
        self.token_ttl = token_ttl
        self.drive = DriveManager(service_account_json=service_json)
        self.channel_map = self._load_channel_map()

    def _load_channel_map(self) -> Dict[str, Any]:
        if not self.channel_map_file or not os.path.exists(self.channel_map_file):
            raise FileNotFoundError(f"Channel map bulunamadi: {self.channel_map_file}")
        with open(self.channel_map_file, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        if not isinstance(data, dict):
            raise ValueError("Channel map JSON obje olmali.")
        return data

    def reload(self) -> None:
        self.channel_map = self._load_channel_map()

    def resolve_folder_id(self, channel: str) -> Optional[str]:
        entry = self.channel_map.get(channel)
        if isinstance(entry, str):
            return entry.strip()
        if isinstance(entry, dict):
            return str(entry.get("folder_id", "")).strip()
        return None

    def make_download_token(self, channel: str, file_item: Dict[str, Any]) -> str:
        now = int(time.time())
        payload = {
            "channel": channel,
            "exp": now + self.token_ttl,
            "file_id": file_item.get("id", ""),
            "name": file_item.get("name", "firmware.bin"),
            "type": file_item.get("type", "BIN"),
        }
        payload_bytes = _json_bytes(payload)
        signature = hmac.new(self.api_key.encode("utf-8"), payload_bytes, hashlib.sha256).digest()
        return f"{_b64url_encode(payload_bytes)}.{_b64url_encode(signature)}"

    def verify_download_token(self, token: str) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
        try:
            payload_part, sig_part = token.split(".", 1)
        except ValueError:
            return None, "gecersiz token"

        try:
            payload_bytes = _b64url_decode(payload_part)
            given_sig = _b64url_decode(sig_part)
        except Exception:
            return None, "token cozumlenemedi"

        expected_sig = hmac.new(self.api_key.encode("utf-8"), payload_bytes, hashlib.sha256).digest()
        if not hmac.compare_digest(expected_sig, given_sig):
            return None, "token imzasi gecersiz"

        try:
            payload = json.loads(payload_bytes.decode("utf-8"))
        except Exception:
            return None, "token JSON hatali"

        if int(payload.get("exp", 0)) < int(time.time()):
            return None, "token suresi dolmus"

        return payload, None


def _send_json(handler: BaseHTTPRequestHandler, status: int, payload: Dict[str, Any]) -> None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


def _send_binary(handler: BaseHTTPRequestHandler, name: str, data: bytes) -> None:
    handler.send_response(200)
    handler.send_header("Content-Type", "application/octet-stream")
    quoted_name = urllib.parse.quote(name)
    handler.send_header("Content-Disposition", f"attachment; filename*=UTF-8''{quoted_name}")
    handler.send_header("Content-Length", str(len(data)))
    handler.end_headers()
    handler.wfile.write(data)


class FirmwareProxyHandler(BaseHTTPRequestHandler):
    server_version = "FirmwareProxy/1.0"

    @property
    def state(self) -> ProxyState:
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, format: str, *args: Any) -> None:
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
        sys.stdout.write(f"[{stamp}] {self.address_string()} - {format % args}\n")

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/v1/health":
            _send_json(self, 200, {"ok": True, "ts": int(time.time())})
            return

        if parsed.path == "/api/v1/catalog":
            if not self._authorized():
                _send_json(self, 401, {"error": "yetkisiz"})
                return

            query = urllib.parse.parse_qs(parsed.query)
            channel = str((query.get("channel") or [""])[0]).strip()
            if not channel:
                _send_json(self, 400, {"error": "channel zorunlu"})
                return

            folder_id = self.state.resolve_folder_id(channel)
            if not folder_id:
                _send_json(self, 404, {"error": "kanal bulunamadi"})
                return

            files, error = self.state.drive.list_all_files_in_folder(folder_id)
            if files is None:
                _send_json(self, 502, {"error": error or "drive catalog hatasi"})
                return

            result = []
            for item in files:
                result.append(
                    {
                        "token": self.state.make_download_token(channel, item),
                        "name": item.get("name", "firmware.bin"),
                        "version": item.get("version"),
                        "type": item.get("type", "BIN"),
                        "size": item.get("size", "?"),
                    }
                )

            _send_json(
                self,
                200,
                {
                    "channel": channel,
                    "ttl_seconds": self.state.token_ttl,
                    "message": error or "",
                    "files": result,
                },
            )
            return

        if parsed.path.startswith("/api/v1/download/"):
            if not self._authorized():
                _send_json(self, 401, {"error": "yetkisiz"})
                return

            token = parsed.path.rsplit("/", 1)[-1]
            payload, error = self.state.verify_download_token(token)
            if payload is None:
                _send_json(self, 401, {"error": error or "token gecersiz"})
                return

            file_id = str(payload.get("file_id", "")).strip()
            file_name = str(payload.get("name", "firmware.bin")).strip() or "firmware.bin"
            file_data, download_error = self.state.drive.download_file_to_memory(file_id)
            if file_data is None:
                _send_json(self, 502, {"error": download_error or "drive indirme hatasi"})
                return

            _send_binary(self, file_name, file_data.read())
            return

        _send_json(self, 404, {"error": "bulunamadi"})

    def _authorized(self) -> bool:
        expected = self.state.api_key
        provided = self.headers.get("X-Proxy-Key", "")
        return bool(expected and hmac.compare_digest(expected, provided))


def main() -> None:
    parser = argparse.ArgumentParser(description="Firmware proxy server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--ttl", type=int, default=int(os.environ.get("FIRMWARE_PROXY_TOKEN_TTL", "120")))
    args = parser.parse_args()

    api_key = os.environ.get("FIRMWARE_PROXY_API_KEY", "").strip()
    service_json = os.environ.get("FIRMWARE_PROXY_SA_JSON", "").strip()
    channel_map_file = os.environ.get("FIRMWARE_PROXY_CHANNEL_MAP_FILE", "").strip()

    if not api_key:
        raise SystemExit("FIRMWARE_PROXY_API_KEY tanimli degil")
    if not service_json:
        raise SystemExit("FIRMWARE_PROXY_SA_JSON tanimli degil")
    if not channel_map_file:
        raise SystemExit("FIRMWARE_PROXY_CHANNEL_MAP_FILE tanimli degil")

    state = ProxyState(
        api_key=api_key,
        service_json=service_json,
        channel_map_file=channel_map_file,
        token_ttl=args.ttl,
    )

    server = ThreadingHTTPServer((args.host, args.port), FirmwareProxyHandler)
    server.state = state  # type: ignore[attr-defined]

    print("Firmware proxy server started")
    print(f"Address: http://{args.host}:{args.port}")
    print(f"Token TTL: {state.token_ttl} sec")
    print(f"Channel map: {state.channel_map_file}")
    print("Endpoints:")
    print("  GET /api/v1/health")
    print("  GET /api/v1/catalog?channel=<name>")
    print("  GET /api/v1/download/<token>")
    print("")
    print("Note: Production deployment should be behind TLS reverse proxy.")
    server.serve_forever()


if __name__ == "__main__":
    main()
