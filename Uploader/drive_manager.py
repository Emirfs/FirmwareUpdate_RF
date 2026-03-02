
import os
import io
import re
import urllib.parse
import requests

try:
    from google.oauth2 import service_account
    from googleapiclient.discovery import build
    from googleapiclient.http import MediaIoBaseDownload
    DRIVE_API_AVAILABLE = True
except ImportError:
    DRIVE_API_AVAILABLE = False

SCOPES = ['https://www.googleapis.com/auth/drive.readonly']
DRIVE_HEAD_URL = "https://drive.google.com/uc?export=download&id={}"

class DriveManager:
    def __init__(self, service_account_json=None):
        self.service = None
        self.service_account_json = service_account_json
        self.api_error = None
        
        if service_account_json and os.path.exists(service_account_json):
            if DRIVE_API_AVAILABLE:
                try:
                    creds = service_account.Credentials.from_service_account_file(
                        service_account_json, scopes=SCOPES)
                    self.service = build('drive', 'v3', credentials=creds)
                except Exception as e:
                    self.api_error = str(e)
            else:
                self.api_error = "Google API kütüphaneleri eksik (pip install google-api-python-client google-auth-httplib2 google-auth-oauthlib)"

    def check_file_version(self, file_id):
        """
        Dosya meta verilerini kontrol eder ve versiyonu döner.
        
        Returns:
            (version, filename, error_message)
            version: int veya None (eğer bulunamazsa)
            filename: str veya None
            error_message: str veya None
        """
        # 1. YÖNTEM: Google Drive API (Varsa)
        if self.service:
            try:
                # Dosya adı ve silinmemiş olma durumu
                results = self.service.files().get(fileId=file_id, fields="id, name, trashed").execute()
                if results.get('trashed'):
                    return None, None, "Dosya çöp kutusunda"
                
                filename = results.get('name')
                version = self._parse_version(filename)
                return version, filename, None
            except Exception as e:
                return None, None, f"API Hatası: {str(e)}"

        # 2. YÖNTEM: Public Link (Fallback)
        # Service account yoksa veya API hatası varsa public link deneriz
        return self._check_public_link(file_id)

    def _get_service_error(self):
        """Service yoksa uygun hata mesajını döner."""
        if not self.service_account_json:
            return "Service Account JSON yolu tanımlı değil (Admin panelinden ayarlayın)"
        elif not os.path.exists(self.service_account_json):
            return f"JSON dosyası bulunamadı: {self.service_account_json}"
        elif self.api_error:
            return f"Drive API hatası: {self.api_error}"
        else:
            return "Service Account başlatılamadı (bilinmeyen hata)"

    def list_all_files_in_folder(self, folder_id):
        """
        Klasör içindeki tüm firmware dosyalarını (.bin ve .hex) listeler.
        
        Returns:
            (files_list, error_message)
            files_list: [{"id", "name", "version", "type"}, ...] versiyona göre azalan sıralı
        """
        if not self.service:
            return None, self._get_service_error()

        try:
            query = f"'{folder_id}' in parents and trashed = false and mimeType != 'application/vnd.google-apps.folder'"
            results = self.service.files().list(
                q=query, fields="files(id, name, size)",
                pageSize=100
            ).execute()
            files = results.get('files', [])

            firmware_files = []
            for file in files:
                name = file.get('name', '')
                lower_name = name.lower()
                # Sadece .bin ve .hex dosyalarını al
                if not (lower_name.endswith('.bin') or lower_name.endswith('.hex')):
                    continue
                ver = self._parse_version(name)
                file_type = 'HEX' if lower_name.endswith('.hex') else 'BIN'
                firmware_files.append({
                    "id": file['id'],
                    "name": name,
                    "version": ver,
                    "type": file_type,
                    "size": file.get('size', '?'),
                })

            # Versiyona göre azalan sırala (None olanlar sona)
            firmware_files.sort(key=lambda x: (x['version'] is not None, x['version'] or 0), reverse=True)

            if not firmware_files:
                return [], "Klasörde .bin veya .hex dosyası bulunamadı"

            return firmware_files, None

        except Exception as e:
            return None, f"API Hatası: {str(e)}"

    def check_updates_in_folder(self, folder_id):
        """
        Klasör içindeki 'update X' dosyalarını tarar ve en yüksek versiyonu döner.
        
        Returns:
            (max_version, file_id, filename, error_message)
        """
        if not self.service:
            return None, None, None, self._get_service_error()

        try:
            query = f"'{folder_id}' in parents and trashed = false and mimeType != 'application/vnd.google-apps.folder'"
            results = self.service.files().list(q=query, fields="files(id, name)").execute()
            files = results.get('files', [])

            max_ver = -1
            max_file = None
            
            for file in files:
                name = file.get('name', '')
                try:
                    ver = self._parse_version(name)
                    if ver is not None and ver > max_ver:
                        max_ver = ver
                        max_file = file
                except:
                    continue
            
            if max_file:
                return max_ver, max_file['id'], max_file['name'], None
            else:
                return None, None, None, "Klasörde uygun güncelleme dosyası bulunamadı"

        except Exception as e:
            return None, None, None, f"API Hatası: {str(e)}"

    def download_file_to_memory(self, file_id, progress_callback=None):
        """
        Dosyayı RAM'e (io.BytesIO) indirir.
        
        Returns:
            (file_data: BytesIO, error_message)
        """
        if not self.service:
            # Fallback: Requests ile indirip BytesIO'ya al
            try:
                url = DRIVE_HEAD_URL.format(file_id)
                resp = requests.get(url, stream=True, timeout=30)
                resp.raise_for_status()
                total = int(resp.headers.get('content-length', 0))
                
                fh = io.BytesIO()
                dl = 0
                for chunk in resp.iter_content(chunk_size=4096):
                    dl += len(chunk)
                    fh.write(chunk)
                    if progress_callback and total > 0:
                        progress_callback(int(dl * 100 / total))
                
                fh.seek(0)
                return fh, None
            except Exception as e:
                return None, f"İndirme hatası: {e}"

        try:
            request = self.service.files().get_media(fileId=file_id)
            fh = io.BytesIO()
            downloader = MediaIoBaseDownload(fh, request)
            done = False
            while done is False:
                status, done = downloader.next_chunk()
                if progress_callback and status:
                    progress_callback(int(status.progress() * 100))
            fh.seek(0)
            return fh, None
        except Exception as e:
            return None, f"API İndirme Hatası: {str(e)}"

    def download_file(self, file_id, dest_path, progress_callback=None):
        """
        Dosyayı indirir.
        
        Returns:
            (success, error_message)
        """
        # 1. YÖNTEM: Google Drive API
        if self.service:
            try:
                request = self.service.files().get_media(fileId=file_id)
                fh = io.FileIO(dest_path, 'wb')
                downloader = MediaIoBaseDownload(fh, request)
                done = False
                while done is False:
                    status, done = downloader.next_chunk()
                    if progress_callback and status:
                        progress_callback(int(status.progress() * 100))
                return True, None
            except Exception as e:
                return False, f"API İndirme Hatası: {str(e)}"

        # 2. YÖNTEM: Public Link (Requests)
        return self._download_public_link(file_id, dest_path, progress_callback)

    def _check_public_link(self, file_id):
        try:
            url = DRIVE_HEAD_URL.format(file_id)
            resp = requests.get(url, timeout=10, stream=True, allow_redirects=True)
            resp.close()

            # Content-Disposition header'ından dosya adını çıkar
            cd = resp.headers.get("Content-Disposition", "")
            filename = None
            if cd:
                match = re.search(r"filename\*=UTF-8''(.+)", cd)
                if match:
                    filename = urllib.parse.unquote(match.group(1).strip())
                else:
                    match = re.search(r'filename="(.+?)"', cd)
                    if match:
                        filename = match.group(1).strip()
                    else:
                        match = re.search(r'filename=([^;\s]+)', cd)
                        if match:
                            filename = match.group(1).strip()
            
            if not filename:
                # Eğer filename bulunamadıysa content-type kontrol et
                ct = resp.headers.get("Content-Type", "")
                if "text/html" in ct:
                    return None, None, "Dosya herkese açık değil veya virüs tarama uyarısına takılıyor (API kullanın)"
                return None, None, "Dosya adı alınamadı"

            version = self._parse_version(filename)
            return version, filename, None

        except Exception as e:
            return None, None, f"Bağlantı Hatası: {str(e)}"

    def _download_public_link(self, file_id, dest_path, progress_callback):
        try:
            url = DRIVE_HEAD_URL.format(file_id)
            with requests.get(url, stream=True, allow_redirects=True) as r:
                r.raise_for_status()
                total_length = r.headers.get('content-length')
                
                with open(dest_path, 'wb') as f:
                    if total_length is None: # no content length header
                        f.write(r.content)
                        if progress_callback: progress_callback(100)
                    else:
                        dl = 0
                        total_length = int(total_length)
                        for data in r.iter_content(chunk_size=4096):
                            dl += len(data)
                            f.write(data)
                            if progress_callback:
                                progress_callback(int(dl * 100 / total_length))
            return True, None
        except Exception as e:
            return False, f"İndirme Hatası: {str(e)}"

    def _parse_version(self, filename):
        if not filename: return None
        # "update 2.bin", "update_3.hex", "update-5.bin" -> 2, 3, 5
        match = re.search(r'update[_\s-]*(\d+)', filename, re.IGNORECASE)
        if match:
            return int(match.group(1))
        return None
