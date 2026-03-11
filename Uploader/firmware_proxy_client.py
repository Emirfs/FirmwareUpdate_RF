import io
from typing import Any, Dict, List, Optional, Tuple

import requests


def _normalize_base_url(value: str) -> str:
    return value.rstrip("/")


class FirmwareProxyClient:
    def __init__(self, base_url: str = "", api_key: str = "", timeout: int = 30) -> None:
        self.base_url = _normalize_base_url(str(base_url or "").strip())
        self.api_key = str(api_key or "").strip()
        self.timeout = int(timeout)
        self.config_error: Optional[str] = None

        if self.base_url and not self.base_url.startswith(("http://", "https://")):
            self.config_error = "Proxy adresi http:// veya https:// ile baslamali."
        elif self.base_url and not self.api_key:
            self.config_error = "Proxy API key eksik. Admin panelde backend alanina 'url|api_key' girip kaydedin."

    @classmethod
    def from_backend_spec(cls, backend_spec: str, timeout: int = 30) -> "FirmwareProxyClient":
        raw = str(backend_spec or "").strip()
        if not raw:
            client = cls("", "", timeout=timeout)
            client.config_error = "Proxy API key eksik. Admin panelde backend alanina 'url|api_key' girip kaydedin."
            return client

        if "|" not in raw:
            client = cls(raw, "", timeout=timeout)
            client.config_error = "Backend formati hatali. 'url|api_key' olarak kaydedin."
            return client

        base_url, api_key = raw.split("|", 1)
        return cls(base_url=base_url, api_key=api_key, timeout=timeout)

    def is_ready(self) -> bool:
        return bool(self.base_url and self.api_key and not self.config_error)

    def _headers(self) -> Dict[str, str]:
        headers: Dict[str, str] = {}
        if self.api_key:
            headers["X-Proxy-Key"] = self.api_key
        return headers

    def health(self) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
        if self.config_error:
            return None, self.config_error
        if not self.is_ready():
            return None, "Proxy backend tanimli degil."
        try:
            response = requests.get(
                f"{self.base_url}/api/v1/health",
                headers=self._headers(),
                timeout=self.timeout,
            )
            response.raise_for_status()
            return response.json(), None
        except requests.RequestException as exc:
            return None, f"Proxy baglanti hatasi: {self.base_url} ulasilamiyor. Proxy server calisiyor mu? ({exc})"

    def list_channel_files(self, channel: str) -> Tuple[Optional[List[Dict[str, Any]]], Optional[str]]:
        if self.config_error:
            return None, self.config_error
        channel_name = str(channel or "").strip()
        if not channel_name:
            return None, "Kanal adi bos."
        try:
            response = requests.get(
                f"{self.base_url}/api/v1/catalog",
                params={"channel": channel_name},
                headers=self._headers(),
                timeout=self.timeout,
            )
            if response.status_code >= 400:
                detail = response.text.strip()
                return None, f"Katalog sorgusu basarisiz: HTTP {response.status_code} - {detail}"

            payload = response.json()
            files = payload.get("files", [])
            if not isinstance(files, list):
                return None, "Proxy yaniti gecersiz: files alani liste degil."
            return files, None
        except requests.RequestException as exc:
            return None, f"Proxy baglanti hatasi: {self.base_url} ulasilamiyor. Proxy server calisiyor mu? ({exc})"
        except ValueError as exc:
            return None, f"Proxy yaniti okunamadi: {exc}"

    def download_file_to_memory(self, token: str, progress_callback=None) -> Tuple[Optional[io.BytesIO], Optional[str]]:
        if self.config_error:
            return None, self.config_error
        token_value = str(token or "").strip()
        if not token_value:
            return None, "Indirme token'i bos."

        try:
            response = requests.get(
                f"{self.base_url}/api/v1/download/{token_value}",
                headers=self._headers(),
                timeout=self.timeout,
                stream=True,
            )
            if response.status_code >= 400:
                detail = response.text.strip()
                return None, f"Indirme basarisiz: HTTP {response.status_code} - {detail}"

            total = int(response.headers.get("Content-Length", "0") or 0)
            data = io.BytesIO()
            downloaded = 0

            for chunk in response.iter_content(chunk_size=4096):
                if not chunk:
                    continue
                data.write(chunk)
                downloaded += len(chunk)
                if progress_callback and total > 0:
                    progress_callback(int(downloaded * 100 / total))

            data.seek(0)
            return data, None
        except requests.RequestException as exc:
            return None, f"Proxy indirme hatasi: {exc}"
