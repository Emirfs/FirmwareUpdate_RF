import io
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import quote, urlparse

import requests


class FirmwareProxyManager:
    def __init__(self, base_url: str, api_key: str = "", timeout: int = 20) -> None:
        self.base_url = self._normalize_base_url(base_url)
        self.api_key = api_key.strip()
        self.timeout = timeout
        self.api_error: Optional[str] = None
        if not self.base_url:
            self.api_error = "Proxy URL tanimli degil."

    def _normalize_base_url(self, value: str) -> str:
        raw = value.strip()
        if not raw:
            return ""
        parsed = urlparse(raw)
        if parsed.scheme:
            return raw.rstrip("/")
        return ("http://" + raw).rstrip("/")

    def _headers(self) -> Dict[str, str]:
        headers = {"Accept": "application/json"}
        if self.api_key:
            headers["X-Proxy-Key"] = self.api_key
        return headers

    def _format_http_error(self, prefix: str, response: requests.Response) -> str:
        body = response.text.strip()
        if len(body) > 160:
            body = body[:157] + "..."
        if body:
            return f"{prefix}: HTTP {response.status_code} - {body}"
        return f"{prefix}: HTTP {response.status_code}"

    def list_all_files_in_folder(self, channel: str) -> Tuple[Optional[List[Dict[str, Any]]], Optional[str]]:
        if not self.base_url:
            return None, self.api_error or "Proxy URL tanimli degil."
        channel = channel.strip()
        if not channel:
            return None, "Firmware kanali tanimli degil."

        url = f"{self.base_url}/api/v1/catalog"
        try:
            resp = requests.get(
                url,
                params={"channel": channel},
                headers=self._headers(),
                timeout=self.timeout,
            )
        except requests.ConnectionError:
            return None, (
                f"Proxy baglanti hatasi: {self.base_url} ulasilamiyor. "
                "Proxy server calisiyor mu?"
            )
        except requests.Timeout:
            return None, "Proxy baglanti hatasi: istek zaman asimina ugradi."
        except Exception as exc:
            return None, f"Proxy baglanti hatasi: {exc}"

        if not resp.ok:
            return None, self._format_http_error("Katalog sorgusu basarisiz", resp)

        try:
            data = resp.json()
        except Exception as exc:
            return None, f"Proxy JSON hatasi: {exc}"

        items = data.get("files", [])
        if not isinstance(items, list):
            return None, "Proxy yaniti gecersiz: files listesi yok."

        normalized: List[Dict[str, Any]] = []
        for item in items:
            if not isinstance(item, dict):
                continue
            token = str(item.get("token", "")).strip()
            if not token:
                continue
            normalized.append(
                {
                    "id": token,  # GUI/uploader tarafinda opak referans olarak kullanilir
                    "name": str(item.get("name", "dosya")),
                    "version": item.get("version"),
                    "type": str(item.get("type", "BIN")).upper(),
                    "size": item.get("size", "?"),
                }
            )

        if not normalized:
            message = data.get("message", "Proxy kanalinda firmware bulunamadi.")
            return [], str(message)
        return normalized, None

    def download_file_to_memory(
        self, token: str, progress_callback: Optional[Any] = None
    ) -> Tuple[Optional[io.BytesIO], Optional[str]]:
        if not self.base_url:
            return None, self.api_error or "Proxy URL tanimli degil."
        token = token.strip()
        if not token:
            return None, "Indirme tokeni bos."

        safe_token = quote(token, safe="")
        url = f"{self.base_url}/api/v1/download/{safe_token}"
        try:
            resp = requests.get(url, headers=self._headers(), stream=True, timeout=self.timeout)
        except requests.ConnectionError:
            return None, (
                f"Proxy baglanti hatasi: {self.base_url} ulasilamiyor. "
                "Proxy server calisiyor mu?"
            )
        except requests.Timeout:
            return None, "Proxy baglanti hatasi: istek zaman asimina ugradi."
        except Exception as exc:
            return None, f"Proxy baglanti hatasi: {exc}"

        if not resp.ok:
            return None, self._format_http_error("Indirme basarisiz", resp)

        total = int(resp.headers.get("content-length", 0))
        received = 0
        buffer = io.BytesIO()
        try:
            for chunk in resp.iter_content(chunk_size=4096):
                if not chunk:
                    continue
                buffer.write(chunk)
                received += len(chunk)
                if progress_callback and total > 0:
                    progress_callback(int(received * 100 / total))
        except Exception as exc:
            return None, f"Indirme akis hatasi: {exc}"
        finally:
            resp.close()

        buffer.seek(0)
        return buffer, None
