"""
Smart Home Firmware Güncelleme Aracı — Dual-Panel GUI
Kullanıcı Paneli: Giriş gerektirmez — cihaz seç, güncelleme kontrol, yükle
Admin Paneli: Kullanıcı adı + şifre ile giriş — tüm ayarları yönet
"""
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import threading
import requests
import serial.tools.list_ports
import os
import sys

from config_manager import (
    load_credentials, verify_admin, change_admin_credentials,
    load_config, save_config, config_exists, reset_config,
    DEFAULT_CONFIG, credentials_exist
)
from uploder import upload_firmware, update_stm32_key
from drive_manager import DriveManager

APP_TITLE = "🏠 Smart Home Firmware Güncelleme"
DRIVE_HEAD_URL = "https://drive.google.com/uc?export=download&id={}"

import re
import urllib.parse
Signature = "emirfurkansari.com"
# ══════════════════════════════════════════════════════════════
# Renk Paleti (Catppuccin Mocha)
# ══════════════════════════════════════════════════════════════
C = {
    "bg": "#1e1e1e", "surface": "#313244", "overlay": "#45475a",
    "text": "#cdd6f4", "subtext": "#a6adc8", "dim": "#6c7086",
    "blue": "#89b4fa", "green": "#a6e3a1", "red": "#f38ba8",
    "yellow": "#f9e2af", "peach": "#fab387", "teal": "#94e2d5",
    "mauve": "#cba6f7", "dark": "#11111b", "test": "#d8bfd8", "white": "#ffffff"
}


import traceback

# ══════════════════════════════════════════════════════════════
# Console Logging Redirect (No-Console Crash Fix)
# ══════════════════════════════════════════════════════════════
class Logger(object):
    def __init__(self):
        self.terminal = sys.stdout
        self.log = open("application.log", "a", encoding="utf-8")

    def write(self, message):
        if self.terminal:
            try:
                self.terminal.write(message)
            except:
                pass
        try:
            self.log.write(message)
            self.log.flush()
        except:
            pass

    def flush(self):
        if self.terminal:
            try:
                self.terminal.flush()
            except:
                pass
        try:
            self.log.flush()
        except:
            pass

# PyInstaller no-console modunda stdout/stderr None olabilir
if sys.stdout is None or sys.stderr is None:
    sys.stdout = Logger()
    sys.stderr = sys.stdout

# Global exception handler
def handle_exception(exc_type, exc_value, exc_traceback):
    if issubclass(exc_type, KeyboardInterrupt):
        sys.__excepthook__(exc_type, exc_value, exc_traceback)
        return
    
    err_msg = "".join(traceback.format_exception(exc_type, exc_value, exc_traceback))
    try:
        with open("crash_log.txt", "a", encoding="utf-8") as f:
            f.write("CRASH:\n" + err_msg + "\n" + "-"*30 + "\n")
    except:
        pass
    # Hata mesajını göster (GUI varsa)
    try:
        messagebox.showerror("Kritik Hata", f"Beklenmeyen bir hata oluştu:\n{exc_value}")
    except:
        pass

sys.excepthook = handle_exception


class FirmwareUpdaterApp:
    def __init__(self, root):
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("680x780")
        self.root.minsize(640, 720)
        self.root.configure(bg=C["bg"])

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        # Pencere restore edildiğinde görünürlügü zorla
        self.root.bind("<Map>", self._on_map)

        self.config = DEFAULT_CONFIG.copy()
        self.admin_unlocked = False
        self.admin_password = None
        self.upload_thread = None
        self.stop_requested = False
        self.current_panel = "user"  # "user" veya "admin"
        self.current_panel = "user"  # "user" veya "admin"
        self._admin_canvas = None  # Admin panel scroll canvas referansı
        self._mousewheel_binding = None
        self.drive_manager = None
        self._available_files = []  # Tarama sonucu bulunan dosyalar
        self._pending_drive_version = None

        self.style = ttk.Style()
        self.style.theme_use("clam")
        self._configure_styles()

        # Ana container
        self.container = ttk.Frame(self.root, style="Main.TFrame")
        self.container.pack(fill=tk.BOTH, expand=True)

        # Paneller
        self.user_frame = None
        self.admin_frame = None

        self._build_user_panel()
        self._try_load_config()
        self._scan_ports()
    
    def _on_map(self, event):
        """Pencere restore edildiğinde çalışır."""
        if event.widget == self.root:
            self.root.deiconify()

    def _on_close(self):
        """Pencere kapatma — temiz çıkış."""
        self.stop_requested = True
        self._unbind_mousewheel()
        try:
            self.root.destroy()
        except Exception:
            pass
        os._exit(0)  # Tüm thread'leri dahil süreci zorla sonlandır

    # ══════════════════════════════════════════════════════════
    # Stiller
    # ══════════════════════════════════════════════════════════

    def _reload_drive_manager(self):
        """Config'deki JSON yoluna göre DriveManager'ı yenile."""
        json_path = self.config.get("service_account_json", "")
        # Config'de boşsa DEFAULT_CONFIG'deki yolu kullan
        if not json_path:
            json_path = DEFAULT_CONFIG.get("service_account_json", "")
        self.drive_manager = DriveManager(json_path if json_path else None)
        if self.drive_manager.api_error:
            self._log_msg(f"⚠️ Drive API uyarısı: {self.drive_manager.api_error}")
        else:
            if json_path:
                self._log_msg("credential drive api ✅")

    def _configure_styles(self):
        s = self.style
        s.configure("Main.TFrame", background=C["bg"])
        s.configure("Surface.TFrame", background=C["surface"])
        s.configure("Card.TFrame", background=C["surface"])

        s.configure("Main.TLabel", background=C["bg"], foreground=C["text"], font=("Segoe UI", 10))
        s.configure("Title.TLabel", background=C["bg"], foreground=C["blue"], font=("Segoe UI", 16, "bold"))
        s.configure("Section.TLabel", background=C["bg"], foreground=C["yellow"], font=("Segoe UI", 11, "bold"))
        s.configure("Card.TLabel", background=C["surface"], foreground=C["text"], font=("Segoe UI", 10))
        s.configure("CardTitle.TLabel", background=C["surface"], foreground=C["yellow"], font=("Segoe UI", 10, "bold"))
        s.configure("Status.TLabel", background=C["dark"], foreground=C["dim"], font=("Segoe UI", 9))
        s.configure("Info.TLabel", background=C["bg"], foreground=C["subtext"], font=("Segoe UI", 9))
        s.configure("Success.TLabel", background=C["bg"], foreground=C["green"], font=("Segoe UI", 10, "bold"))
        s.configure("Error.TLabel", background=C["bg"], foreground=C["red"], font=("Segoe UI", 10, "bold"))

        s.configure("Accent.TButton", background=C["blue"], foreground=C["bg"], font=("Segoe UI", 10, "bold"))
        s.map("Accent.TButton", background=[("active", C["teal"])])
        s.configure("Start.TButton", background=C["green"], foreground=C["bg"], font=("Segoe UI", 12, "bold"), padding=(10, 8))
        s.map("Start.TButton", background=[("active", C["teal"])])
        s.configure("Stop.TButton", background=C["red"], foreground=C["bg"], font=("Segoe UI", 12, "bold"), padding=(10, 8))
        s.map("Stop.TButton", background=[("active", C["peach"])])
        s.configure("Small.TButton", background=C["surface"], foreground=C["text"], font=("Segoe UI", 9))
        s.map("Small.TButton", background=[("active", C["overlay"])])
        s.configure("Admin.TButton", background=C["mauve"], foreground=C["bg"], font=("Segoe UI", 10, "bold"))
        s.map("Admin.TButton", background=[("active", C["blue"])])
        s.configure("Lock.TButton", background=C["peach"], foreground=C["bg"], font=("Segoe UI", 9, "bold"))
        s.map("Lock.TButton", background=[("active", C["yellow"])])
        s.configure("Danger.TButton", background=C["red"], foreground=C["bg"], font=("Segoe UI", 9, "bold"))
        s.map("Danger.TButton", background=[("active", C["peach"])])

        s.configure("green.Horizontal.TProgressbar", troughcolor=C["surface"], background=C["white"])

    # ══════════════════════════════════════════════════════════
    # KULLANICI PANELİ
    # ══════════════════════════════════════════════════════════

    def _build_user_panel(self):
        self.user_frame = ttk.Frame(self.container, style="Main.TFrame", padding=16)
        self.user_frame.pack(fill=tk.BOTH, expand=True)
        f = self.user_frame

        # Başlık
        header = ttk.Frame(f, style="Main.TFrame")
        header.pack(fill=tk.X)
        ttk.Label(header, text="🏠 Smart Home Firmware Güncelleme", style="Title.TLabel").pack(side=tk.LEFT)
        ttk.Button(header, text="🔐 Admin", command=self._show_admin_login, style="Admin.TButton").pack(side=tk.RIGHT)

        ttk.Separator(f, orient="horizontal").pack(fill=tk.X, pady=(8, 12))

        # ── Cihaz Seçimi ──
        ttk.Label(f, text=" Cihaz Seçimi", style="Section.TLabel").pack(anchor="w")
        dev_frame = ttk.Frame(f, style="Main.TFrame")
        dev_frame.pack(fill=tk.X, pady=(4, 8))

        ttk.Label(dev_frame, text="Cihaz:", style="Main.TLabel").grid(row=0, column=0, sticky="w", padx=(0, 8))
        self.device_var = tk.StringVar()
        self.device_combo = ttk.Combobox(dev_frame, textvariable=self.device_var, width=30, state="readonly")
        self.device_combo.grid(row=0, column=1, sticky="ew", padx=(0, 8))
        self.device_combo.bind("<<ComboboxSelected>>", self._on_device_selected)

        ttk.Label(dev_frame, text="COM Port:", style="Main.TLabel").grid(row=0, column=2, sticky="w", padx=(8, 8))
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(dev_frame, textvariable=self.port_var, width=12, state="readonly")
        self.port_combo.grid(row=0, column=3, padx=(0, 4))
        ttk.Button(dev_frame, text="🔄", width=3, command=self._scan_ports, style="Small.TButton").grid(row=0, column=4)

        self.is_rf_var = tk.BooleanVar(value=False)
        self.is_rf_check = ttk.Checkbutton(dev_frame, text="RF Modu", variable=self.is_rf_var)
        self.is_rf_check.grid(row=0, column=5, padx=(12, 0))

        dev_frame.columnconfigure(1, weight=1)

        # ── Firmware Seçimi ──
        ttk.Separator(f, orient="horizontal").pack(fill=tk.X, pady=(4, 8))
        ttk.Label(f, text="Firmware Seçimi", style="Section.TLabel").pack(anchor="w")

        scan_frame = ttk.Frame(f, style="Main.TFrame")
        scan_frame.pack(fill=tk.X, pady=(4, 4))

        self.scan_btn = ttk.Button(scan_frame, text="Güncellemeyi Kontrol Et", command=self._scan_folder, style="Accent.TButton")
        self.scan_btn.pack(side=tk.LEFT, padx=(0, 12))

        self.update_status_var = tk.StringVar(value="Henüz taranmadı")
        self.update_status_label = ttk.Label(scan_frame, textvariable=self.update_status_var, style="Info.TLabel")
        self.update_status_label.pack(side=tk.LEFT, fill=tk.X, expand=True)

        # Firmware dropdown
        fw_frame = ttk.Frame(f, style="Main.TFrame")
        fw_frame.pack(fill=tk.X, pady=(4, 4))

        ttk.Label(fw_frame, text="Firmware:", style="Main.TLabel").pack(side=tk.LEFT, padx=(0, 8))
        self.firmware_var = tk.StringVar()
        self.firmware_combo = ttk.Combobox(fw_frame, textvariable=self.firmware_var, state="readonly", width=50)
        self.firmware_combo.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.firmware_combo.bind("<<ComboboxSelected>>", self._on_firmware_selected)

        # Firmware bilgi etiketi
        self.fw_info_var = tk.StringVar(value="")
        self.fw_info_label = ttk.Label(f, textvariable=self.fw_info_var, style="Info.TLabel")
        self.fw_info_label.pack(anchor="w", pady=(2, 4))

        # ── Başlat Butonu ──
        ttk.Separator(f, orient="horizontal").pack(fill=tk.X, pady=(4, 10))
        self.start_btn = ttk.Button(f, text="🚀  Güncellemeyi Başlat", command=self._start_upload, style="Start.TButton")
        self.start_btn.pack(fill=tk.X, ipady=4)

        # ── İlerleme ──
        prog_frame = ttk.Frame(f, style="Main.TFrame")
        prog_frame.pack(fill=tk.X, pady=(8, 2))
        self.progress_var = tk.DoubleVar()
        self.progress_bar = ttk.Progressbar(prog_frame, variable=self.progress_var, maximum=100, style="green.Horizontal.TProgressbar")
        self.progress_bar.pack(fill=tk.X, side=tk.LEFT, expand=True, padx=(0, 8))
        self.progress_label = ttk.Label(prog_frame, text="0%", style="Main.TLabel", width=14)
        self.progress_label.pack(side=tk.RIGHT)

        # ── Log ──
        ttk.Label(f, text="📋 Log", style="Section.TLabel").pack(anchor="w", pady=(8, 2))
        self.log_text = scrolledtext.ScrolledText(f, height=10, bg=C["dark"], fg=C["text"],
                                                   font=("Consolas", 9), insertbackground=C["text"],
                                                   relief="flat", state="disabled", wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=True)

        # ── Durum Çubuğu ──
        self.status_var = tk.StringVar(value="Hazır")
        ttk.Label(f, textvariable=self.status_var, style="Status.TLabel", anchor="w").pack(fill=tk.X, pady=(4, 0))

    # ══════════════════════════════════════════════════════════
    # Cihaz & Port
    # ══════════════════════════════════════════════════════════

    def _refresh_device_list(self):
        devices = self.config.get("devices", [])
        names = [d["name"] for d in devices]
        self.device_combo["values"] = names if names else ["Cihaz yok — Admin panelinden ekleyin"]
        if names:
            self.device_var.set(names[0])
        else:
            self.device_var.set("Cihaz yok — Admin panelinden ekleyin")

    def _on_device_selected(self, event=None):
        self.update_status_var.set("Henüz kontrol edilmedi")
        self.update_status_label.configure(style="Info.TLabel")

    def _get_selected_device(self):
        name = self.device_var.get()
        for d in self.config.get("devices", []):
            if d["name"] == name:
                return d
        return None

    def _scan_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports if ports else ["Port bulunamadı"]
        cfg_port = self.config.get("serial_port", "")
        if ports:
            if cfg_port in ports:
                self.port_var.set(cfg_port)
            else:
                self.port_var.set(ports[0])
        self._log_msg("🔄 COM portları tarandı: " + ", ".join(ports if ports else ["yok"]))

    # ══════════════════════════════════════════════════════════
    # Güncelleme Kontrolü (DriveManager ile)
    # ══════════════════════════════════════════════════════════

    def _scan_folder(self):
        """Drive klasörünü tara ve tüm firmware dosyalarını dropdown'a doldur."""
        device = self._get_selected_device()
        if not device:
            messagebox.showwarning("Uyarı", "Önce bir cihaz seçin!")
            return
        folder_id = device.get("drive_file_id", "")
        if not folder_id:
            self.update_status_var.set("❌ Drive Klasör ID tanımlı değil")
            self.update_status_label.configure(style="Error.TLabel")
            return

        self.update_status_var.set("⏳ Klasör taranıyor...")
        self.scan_btn.configure(state="disabled")
        self._available_files = []

        if not self.drive_manager:
            self._reload_drive_manager()

        def scan():
            try:
                files, error = self.drive_manager.list_all_files_in_folder(folder_id)
                if error and not files:
                    self.root.after(0, self._scan_folder_result, None, f"❌ {error}")
                    return
                self.root.after(0, self._scan_folder_result, files, error)
            except Exception as e:
                self.root.after(0, self._scan_folder_result, None, f"❌ Beklenmeyen hata: {e}")

        threading.Thread(target=scan, daemon=True).start()

    def _scan_folder_result(self, files, error):
        """Tarama sonuçlarını dropdown'a doldur."""
        self.scan_btn.configure(state="normal")

        if files is None or len(files) == 0:
            self.update_status_var.set(error or "Dosya bulunamadı")
            self.update_status_label.configure(style="Error.TLabel")
            self.firmware_combo["values"] = []
            self.firmware_var.set("")
            self.fw_info_var.set("")
            self._available_files = []
            self._log_msg(error or "Dosya bulunamadı")
            return

        self._available_files = files
        # Dropdown değerlerini oluştur: "update 5.bin [v5 | BIN]"
        display_names = []
        for f in files:
            ver_str = f"v{f['version']}" if f['version'] is not None else "v?"
            display_names.append(f"{f['name']}  [{ver_str} | {f['type']}]")

        self.firmware_combo["values"] = display_names
        self.firmware_combo.current(0)  # İlk (en yüksek versiyon) seçili
        self._on_firmware_selected()  # Bilgiyi göster

        count = len(files)
        msg = f"✅ {count} dosya bulundu"
        if error:
            msg += f" (⚠️ {error})"
        self.update_status_var.set(msg)
        self.update_status_label.configure(style="Success.TLabel")
        self._log_msg(f"📂 Klasör tarandı: {count} firmware dosyası bulundu")

    def _on_firmware_selected(self, event=None):
        """Dropdown'dan firmware seçildiğinde bilgi göster."""
        idx = self.firmware_combo.current()
        if idx < 0 or idx >= len(self._available_files):
            self.fw_info_var.set("")
            return
        f = self._available_files[idx]
        ver_str = f"v{f['version']}" if f['version'] is not None else "bilinmiyor"
        size_str = f"{int(f['size']):,} byte" if f['size'] != '?' else "boyut bilinmiyor"
        device = self._get_selected_device()
        installed = device.get("last_installed_version", 0) if device else 0
        self.fw_info_var.set(f"📦 Versiyon: {ver_str} | 📐 {size_str} | 🔧 Tür: {f['type']} | Yüklü: v{installed}")

    # ══════════════════════════════════════════════════════════
    # Upload (Firmware Güncelleme)
    # ══════════════════════════════════════════════════════════

    def _start_upload(self):
        device = self._get_selected_device()
        if not device:
            messagebox.showwarning("Uyarı", "Önce bir cihaz seçin!")
            return
        if not device.get("aes_key_hex"):
            messagebox.showwarning("Uyarı", "Seçili cihazın AES Key'i tanımlı değil!")
            return

        # Dropdown'dan seçili dosyayı al
        idx = self.firmware_combo.current()
        if idx < 0 or idx >= len(self._available_files):
            messagebox.showwarning("Uyarı", "Lütfen önce 'Klasörü Tara' ile dosyaları listeleyin ve bir firmware seçin.")
            return

        selected_file = self._available_files[idx]
        target_file_id = selected_file["id"]
        file_type = selected_file["type"]  # "BIN" veya "HEX"
        file_version = selected_file.get("version")
        self._pending_drive_version = file_version

        self._log_msg(f"📄 Seçilen: {selected_file['name']} [{file_type}]")

        config = {
            "serial_port": self.port_var.get(),
            "baud_rate": self.config.get("baud_rate", 115200),
            "drive_file_id": target_file_id,
            "aes_key_hex": device["aes_key_hex"],
            "packet_size": self.config.get("packet_size", 128),
            "max_retries": self.config.get("max_retries", 7),
            "firmware_version": device.get("firmware_version", 1),
            "file_type": file_type,  # HEX veya BIN
            "filename": selected_file["name"],
            "is_rf": self.is_rf_var.get(),
        }

        self.start_btn.configure(text="⛔  Durdur", command=self._stop_upload, style="Stop.TButton")
        self.stop_requested = False
        self.progress_var.set(0)
        self.progress_label.configure(text="0%")
        self.status_var.set("Güncelleme devam ediyor...")

        self.upload_thread = threading.Thread(target=self._upload_worker, args=(config,), daemon=True)
        self.upload_thread.start()

    def _stop_upload(self):
        self.stop_requested = True
        self._log_msg("⛔ Durdurma isteği gönderildi...")

    def _upload_worker(self, config):
        if not self.drive_manager:
            self._reload_drive_manager()
            
        success = upload_firmware(
            config=config, log=self._log_callback,
            on_progress=self._progress_callback,
            stop_flag=lambda: self.stop_requested,
            drive_manager=self.drive_manager
        )
        self.root.after(0, self._upload_finished, success)

    def _upload_finished(self, success):
        self.start_btn.configure(text="🚀  Güncellemeyi Başlat", command=self._start_upload, style="Start.TButton")
        if success:
            self.status_var.set("✅ Güncelleme başarılı!")
            self.progress_var.set(100)
            self.progress_label.configure(text="100%")
            # Yüklenen versiyonu kaydet
            device = self._get_selected_device()
            if device and hasattr(self, '_pending_drive_version') and self._pending_drive_version:
                device["last_installed_version"] = self._pending_drive_version
                self._pending_drive_version = None
                # Config'i otomatik kaydet
                if self.admin_password:
                    try:
                        save_config(self.config, self.admin_password)
                        self._log_msg(f"💾 Yüklü versiyon güncellendi: v{device['last_installed_version']}")
                    except Exception:
                        self._log_msg("⚠️ Versiyon kaydedilemedi — Admin girişi ile kaydedin.")
        elif self.stop_requested:
            self.status_var.set("⛔ Kullanıcı tarafından durduruldu")
        else:
            self.status_var.set("❌ Güncelleme başarısız!")

    # ══════════════════════════════════════════════════════════
    # Log & Progress
    # ══════════════════════════════════════════════════════════

    def _log_msg(self, msg):
        self.log_text.configure(state="normal")
        self.log_text.insert(tk.END, msg + "\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state="disabled")

    def _log_callback(self, msg):
        self.root.after(0, self._log_msg, msg)

    def _progress_callback(self, current, total):
        if total > 0:
            pct = current * 100 / total
            self.root.after(0, self._update_progress, pct, current, total)

    def _update_progress(self, pct, current, total):
        self.progress_var.set(pct)
        self.progress_label.configure(text=f"{int(pct)}% ({current}/{total})")

    # ══════════════════════════════════════════════════════════
    # Config Auto-Load
    # ══════════════════════════════════════════════════════════

    def _try_load_config(self):
        """Admin şifresi olmadan sadece cihaz listesini yüklemeye çalış."""
        if config_exists():
            # Varsayılan şifre ile dene
            try:
                self.config = load_config("admin")
                self._refresh_device_list()
                self._reload_drive_manager()
                self._log_msg("📁 Config yüklendi.")
                return
            except ValueError:
                pass
            self._log_msg("🔐 Config şifreli — Admin girişi ile yüklenecek.")
        else:
            self._log_msg("📝 Config dosyası yok — Admin panelinden cihaz ekleyin.")
        self._refresh_device_list()
        # DriveManager'ı varsayılan JSON ile başlat (en azından hazır olsun)
        self._reload_drive_manager()

    # ══════════════════════════════════════════════════════════
    # ADMİN GİRİŞİ
    # ══════════════════════════════════════════════════════════

    def _show_admin_login(self):
        dialog = tk.Toplevel(self.root)
        dialog.title("🔐 Admin Girişi")
        dialog.geometry("380x200")
        dialog.resizable(False, False)
        dialog.configure(bg=C["bg"])
        dialog.transient(self.root)
        dialog.grab_set()

        frame = ttk.Frame(dialog, style="Main.TFrame", padding=20)
        frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frame, text="Admin Kullanıcı Adı:", style="Main.TLabel").pack(anchor="w")
        user_var = tk.StringVar()
        user_entry = ttk.Entry(frame, textvariable=user_var, width=30)
        user_entry.pack(fill=tk.X, pady=(2, 6))
        user_entry.focus_set()

        ttk.Label(frame, text="Admin Şifresi:", style="Main.TLabel").pack(anchor="w")
        pwd_var = tk.StringVar()
        pwd_entry = ttk.Entry(frame, textvariable=pwd_var, show="●", width=30)
        pwd_entry.pack(fill=tk.X, pady=(2, 8))

        def do_login(event=None):
            username = user_var.get().strip()
            password = pwd_var.get().strip()
            if not username or not password:
                messagebox.showwarning("Uyarı", "Kullanıcı adı ve şifre boş olamaz!", parent=dialog)
                return
            if verify_admin(username, password):
                self.admin_password = password
                self.admin_unlocked = True
                try:
                    self.config = load_config(password)
                except ValueError:
                    if not config_exists():
                        self.config = DEFAULT_CONFIG.copy()
                    else:
                        # Eski şifre ile şifrelenmiş olabilir, default kullan
                        self.config = DEFAULT_CONFIG.copy()
                self._refresh_device_list()
                self._reload_drive_manager()
                self._log_msg("✅ Admin girişi başarılı!")
                dialog.destroy()
                self._show_admin_panel()
            else:
                messagebox.showerror("Hata", "❌ Kullanıcı adı veya şifre yanlış!", parent=dialog)

        pwd_entry.bind("<Return>", do_login)
        user_entry.bind("<Return>", do_login)
        ttk.Button(frame, text="Giriş", command=do_login, style="Accent.TButton").pack(pady=(4, 0))

    # ══════════════════════════════════════════════════════════
    # ADMİN PANELİ
    # ══════════════════════════════════════════════════════════

    def _show_admin_panel(self):
        self.user_frame.pack_forget()
        self.current_panel = "admin"

        self.admin_frame = ttk.Frame(self.container, style="Main.TFrame", padding=16)
        self.admin_frame.pack(fill=tk.BOTH, expand=True)
        f = self.admin_frame

        # Başlık
        header = ttk.Frame(f, style="Main.TFrame")
        header.pack(fill=tk.X)
        ttk.Label(header, text="⚙️ Admin Paneli", style="Title.TLabel").pack(side=tk.LEFT)
        ttk.Button(header, text="◀ Kullanıcı Paneline Dön", command=self._back_to_user, style="Lock.TButton").pack(side=tk.RIGHT)

        ttk.Separator(f, orient="horizontal").pack(fill=tk.X, pady=(8, 12))

        # Scrollable content
        canvas = tk.Canvas(f, bg=C["bg"], highlightthickness=0)
        scrollbar = ttk.Scrollbar(f, orient="vertical", command=canvas.yview)
        scroll_frame = ttk.Frame(canvas, style="Main.TFrame")

        scroll_frame.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=scroll_frame, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)

        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        # Canvas referansını sakla (temizlik için)
        self._admin_canvas = canvas

        # Mouse wheel scroll — canvas'a scoped
        self._bind_mousewheel(canvas)

        sf = scroll_frame

        # ── CİHAZ YÖNETİMİ ──
        ttk.Label(sf, text="📱 Cihaz Yönetimi", style="Section.TLabel").pack(anchor="w", pady=(0, 6))

        self.device_list_frame = ttk.Frame(sf, style="Main.TFrame")
        self.device_list_frame.pack(fill=tk.X, pady=(0, 6))
        self._refresh_admin_device_list()

        btn_row = ttk.Frame(sf, style="Main.TFrame")
        btn_row.pack(fill=tk.X, pady=(0, 12))
        ttk.Button(btn_row, text="➕ Yeni Cihaz Ekle", command=self._add_device_dialog, style="Accent.TButton").pack(side=tk.LEFT)

        # ── GENEL AYARLAR ──
        ttk.Separator(sf, orient="horizontal").pack(fill=tk.X, pady=(4, 8))
        ttk.Label(sf, text="🔧 Genel Ayarlar", style="Section.TLabel").pack(anchor="w", pady=(0, 6))

        settings_card = ttk.Frame(sf, style="Surface.TFrame", padding=10)
        settings_card.pack(fill=tk.X, pady=(0, 8))

        r1 = ttk.Frame(settings_card, style="Surface.TFrame")
        r1.pack(fill=tk.X, pady=2)
        ttk.Label(r1, text="🔧 Baud Rate:", style="Card.TLabel", width=16).pack(side=tk.LEFT)
        self.admin_baud_var = tk.StringVar(value=str(self.config.get("baud_rate", 115200)))
        ttk.Entry(r1, textvariable=self.admin_baud_var, width=10).pack(side=tk.LEFT)

        r2 = ttk.Frame(settings_card, style="Surface.TFrame")
        r2.pack(fill=tk.X, pady=2)
        ttk.Label(r2, text="🔄 Max Retry:", style="Card.TLabel", width=16).pack(side=tk.LEFT)
        self.admin_retry_var = tk.StringVar(value=str(self.config.get("max_retries", 7)))
        ttk.Entry(r2, textvariable=self.admin_retry_var, width=6).pack(side=tk.LEFT, padx=(0, 16))
        ttk.Label(r2, text="📐 Paket Boyutu:", style="Card.TLabel").pack(side=tk.LEFT)
        self.admin_pkt_var = tk.StringVar(value=str(self.config.get("packet_size", 128)))
        ttk.Entry(r2, textvariable=self.admin_pkt_var, width=6).pack(side=tk.LEFT)

        r3 = ttk.Frame(settings_card, style="Surface.TFrame")
        r3.pack(fill=tk.X, pady=2)
        ttk.Label(r3, text="🔌 Varsayılan Port:", style="Card.TLabel", width=16).pack(side=tk.LEFT)
        self.admin_port_var = tk.StringVar(value=self.config.get("serial_port", "COM7"))
        ttk.Entry(r3, textvariable=self.admin_port_var, width=10).pack(side=tk.LEFT)

        r4 = ttk.Frame(settings_card, style="Surface.TFrame")
        r4.pack(fill=tk.X, pady=2)
        ttk.Label(r4, text="🔑 Svc Account JSON:", style="Card.TLabel", width=16).pack(side=tk.LEFT)
        self.admin_json_var = tk.StringVar(value=self.config.get("service_account_json", ""))
        ttk.Entry(r4, textvariable=self.admin_json_var, width=30).pack(side=tk.LEFT)


        # ── GÜVENLİK ──
        ttk.Separator(sf, orient="horizontal").pack(fill=tk.X, pady=(4, 8))
        ttk.Label(sf, text="🔒 Güvenlik", style="Section.TLabel").pack(anchor="w", pady=(0, 6))

        sec_row = ttk.Frame(sf, style="Main.TFrame")
        sec_row.pack(fill=tk.X, pady=(0, 4))
        ttk.Button(sec_row, text="🔑 Şifre Değiştir", command=self._change_password_dialog, style="Lock.TButton").pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(sec_row, text="🗑 Config Sıfırla", command=self._reset_config, style="Danger.TButton").pack(side=tk.LEFT)

        # ── STM32 Key Güncelleme ──
        ttk.Separator(sf, orient="horizontal").pack(fill=tk.X, pady=(4, 8))
        ttk.Label(sf, text="🔑 STM32 AES Key Güncelleme", style="Section.TLabel").pack(anchor="w", pady=(0, 6))

        stm_row = ttk.Frame(sf, style="Main.TFrame")
        stm_row.pack(fill=tk.X, pady=(0, 12))
        ttk.Button(stm_row, text="🔄 STM32 Key Güncelle", command=self._update_stm32_key_dialog, style="Accent.TButton").pack(side=tk.LEFT)

        # ── KAYDET ──
        ttk.Separator(sf, orient="horizontal").pack(fill=tk.X, pady=(8, 10))
        save_row = ttk.Frame(sf, style="Main.TFrame")
        save_row.pack(fill=tk.X)
        ttk.Button(save_row, text="💾 Tüm Ayarları Kaydet", command=self._save_all_config, style="Start.TButton").pack(fill=tk.X, ipady=2)

    def _bind_mousewheel(self, canvas):
        """Canvas'a scoped mousewheel bağla (global değil)."""
        def _on_mousewheel(event):
            try:
                canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
            except tk.TclError:
                pass
        def _bind_to_mousewheel(event):
            canvas.bind_all("<MouseWheel>", _on_mousewheel)
        def _unbind_from_mousewheel(event):
            canvas.unbind_all("<MouseWheel>")
        canvas.bind("<Enter>", _bind_to_mousewheel)
        canvas.bind("<Leave>", _unbind_from_mousewheel)
        self._mousewheel_binding = (_bind_to_mousewheel, _unbind_from_mousewheel)

    def _unbind_mousewheel(self):
        """Mousewheel binding'i temizle."""
        try:
            self.root.unbind_all("<MouseWheel>")
        except Exception:
            pass
        self._admin_canvas = None
        self._mousewheel_binding = None

    def _back_to_user(self):
        self._unbind_mousewheel()
        if self.admin_frame:
            self.admin_frame.destroy()
            self.admin_frame = None
        self.current_panel = "user"
        self.user_frame.pack(fill=tk.BOTH, expand=True)
        self._refresh_device_list()

    # ══════════════════════════════════════════════════════════
    # Admin: Cihaz Yönetimi
    # ══════════════════════════════════════════════════════════

    def _refresh_admin_device_list(self):
        for w in self.device_list_frame.winfo_children():
            w.destroy()

        devices = self.config.get("devices", [])
        if not devices:
            ttk.Label(self.device_list_frame, text="Henüz cihaz eklenmemiş.", style="Info.TLabel").pack(anchor="w")
            return

        for i, dev in enumerate(devices):
            card = ttk.Frame(self.device_list_frame, style="Surface.TFrame", padding=8)
            card.pack(fill=tk.X, pady=2)
            info = ttk.Frame(card, style="Surface.TFrame")
            info.pack(fill=tk.X)
            ttk.Label(info, text=f"📱 {dev['name']}", style="CardTitle.TLabel").pack(side=tk.LEFT)
            ttk.Label(info, text=f"v{dev.get('firmware_version', '?')}", style="Card.TLabel").pack(side=tk.LEFT, padx=(8, 0))

            btn_frame = ttk.Frame(card, style="Surface.TFrame")
            btn_frame.pack(fill=tk.X, pady=(4, 0))
            ttk.Label(btn_frame, text=f"Folder ID: {dev.get('drive_file_id', 'Tanımsız')[:20]}...", style="Card.TLabel").pack(side=tk.LEFT)
            idx = i
            ttk.Button(btn_frame, text="🗑", width=3, command=lambda j=idx: self._delete_device(j), style="Small.TButton").pack(side=tk.RIGHT, padx=(4, 0))
            ttk.Button(btn_frame, text="✏️", width=3, command=lambda j=idx: self._edit_device_dialog(j), style="Small.TButton").pack(side=tk.RIGHT)

    def _add_device_dialog(self):
        self._device_dialog("Yeni Cihaz Ekle", {}, is_new=True)

    def _edit_device_dialog(self, index):
        devices = self.config.get("devices", [])
        if index < len(devices):
            self._device_dialog("Cihaz Düzenle", devices[index].copy(), is_new=False, index=index)

    def _device_dialog(self, title, device, is_new=True, index=None):
        dialog = tk.Toplevel(self.root)
        dialog.title(f"📱 {title}")
        dialog.geometry("480x300")
        dialog.resizable(False, False)
        dialog.configure(bg=C["bg"])
        dialog.transient(self.root)
        dialog.grab_set()

        frame = ttk.Frame(dialog, style="Main.TFrame", padding=20)
        frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frame, text="Cihaz Adı:", style="Main.TLabel").pack(anchor="w")
        name_var = tk.StringVar(value=device.get("name", ""))
        ttk.Entry(frame, textvariable=name_var, width=50).pack(fill=tk.X, pady=(2, 6))

        ttk.Label(frame, text="Google Drive Klasör ID:", style="Main.TLabel").pack(anchor="w")
        drive_var = tk.StringVar(value=device.get("drive_file_id", ""))
        ttk.Entry(frame, textvariable=drive_var, width=50).pack(fill=tk.X, pady=(2, 6))

        ttk.Label(frame, text="AES Key (64 hex karakter):", style="Main.TLabel").pack(anchor="w")
        key_var = tk.StringVar(value=device.get("aes_key_hex", ""))
        ttk.Entry(frame, textvariable=key_var, width=50).pack(fill=tk.X, pady=(2, 6))

        ttk.Label(frame, text="Firmware Versiyon:", style="Main.TLabel").pack(anchor="w")
        ver_var = tk.StringVar(value=str(device.get("firmware_version", 1)))
        ttk.Entry(frame, textvariable=ver_var, width=10).pack(anchor="w", pady=(2, 8))

        def do_save():
            name = name_var.get().strip()
            if not name:
                messagebox.showwarning("Uyarı", "Cihaz adı boş olamaz!", parent=dialog)
                return
            new_dev = {
                "name": name,
                "drive_file_id": drive_var.get().strip(),
                "aes_key_hex": key_var.get().strip(),
                "firmware_version": int(ver_var.get().strip() or "1"),
            }
            devices = self.config.get("devices", [])
            if is_new:
                devices.append(new_dev)
            else:
                devices[index] = new_dev
            self.config["devices"] = devices
            self._refresh_admin_device_list()
            dialog.destroy()

        ttk.Button(frame, text="💾 Kaydet", command=do_save, style="Accent.TButton").pack(pady=(4, 0))

    def _delete_device(self, index):
        devices = self.config.get("devices", [])
        if index < len(devices):
            name = devices[index]["name"]
            if messagebox.askyesno("Onay", f"'{name}' cihazını silmek istediğinize emin misiniz?"):
                devices.pop(index)
                self.config["devices"] = devices
                self._refresh_admin_device_list()

    # ══════════════════════════════════════════════════════════
    # Admin: Kaydetme
    # ══════════════════════════════════════════════════════════

    def _save_all_config(self):
        if not self.admin_password:
            messagebox.showwarning("Uyarı", "Admin girişi gerekli!")
            return
        try:
            self.config["baud_rate"] = int(self.admin_baud_var.get())
            self.config["max_retries"] = int(self.admin_retry_var.get())
            self.config["packet_size"] = int(self.admin_pkt_var.get())
            self.config["serial_port"] = self.admin_port_var.get().strip()
            self.config["service_account_json"] = self.admin_json_var.get().strip()

            # Cihaz listesi zaten self.config["devices"] içinde güncel
            save_config(self.config, self.admin_password)
            self._reload_drive_manager()
            messagebox.showinfo("Başarılı", "✅ Tüm ayarlar ve cihazlar kaydedildi!")
        except Exception as e:
            messagebox.showerror("Hata", f"Kayıt hatası: {e}")

    # ══════════════════════════════════════════════════════════
    # Admin: Şifre Değiştirme
    # ══════════════════════════════════════════════════════════

    def _change_password_dialog(self):
        dialog = tk.Toplevel(self.root)
        dialog.title("🔑 Şifre Değiştir")
        dialog.geometry("400x320")
        dialog.resizable(False, False)
        dialog.configure(bg=C["bg"])
        dialog.transient(self.root)
        dialog.grab_set()

        frame = ttk.Frame(dialog, style="Main.TFrame", padding=20)
        frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frame, text="Yeni Kullanıcı Adı:", style="Main.TLabel").pack(anchor="w")
        creds = load_credentials()
        new_user_var = tk.StringVar(value=creds.get("username", "admin"))
        ttk.Entry(frame, textvariable=new_user_var, width=30).pack(fill=tk.X, pady=(2, 6))

        ttk.Label(frame, text="Mevcut Şifre:", style="Main.TLabel").pack(anchor="w")
        old_var = tk.StringVar()
        ttk.Entry(frame, textvariable=old_var, show="●", width=30).pack(fill=tk.X, pady=(2, 6))

        ttk.Label(frame, text="Yeni Şifre:", style="Main.TLabel").pack(anchor="w")
        new_var = tk.StringVar()
        ttk.Entry(frame, textvariable=new_var, show="●", width=30).pack(fill=tk.X, pady=(2, 6))

        ttk.Label(frame, text="Yeni Şifre (tekrar):", style="Main.TLabel").pack(anchor="w")
        new2_var = tk.StringVar()
        ttk.Entry(frame, textvariable=new2_var, show="●", width=30).pack(fill=tk.X, pady=(2, 8))

        def do_change():
            old_pwd = old_var.get().strip()
            new_pwd = new_var.get().strip()
            new_user = new_user_var.get().strip()
            if not verify_admin(creds.get("username", ""), old_pwd):
                messagebox.showerror("Hata", "Mevcut şifre yanlış!", parent=dialog)
                return
            if not new_pwd:
                messagebox.showwarning("Uyarı", "Yeni şifre boş olamaz!", parent=dialog)
                return
            if new_pwd != new2_var.get().strip():
                messagebox.showerror("Hata", "Yeni şifreler eşleşmiyor!", parent=dialog)
                return
            if not new_user:
                messagebox.showwarning("Uyarı", "Kullanıcı adı boş olamaz!", parent=dialog)
                return
            try:
                change_admin_credentials(new_user, new_pwd)
                # Config'i yeni şifre ile yeniden kaydet
                self.admin_password = new_pwd
                save_config(self.config, new_pwd)
                messagebox.showinfo("Başarılı", "✅ Admin bilgileri değiştirildi!", parent=dialog)
                dialog.destroy()
            except Exception as e:
                messagebox.showerror("Hata", f"Hata: {e}", parent=dialog)

        ttk.Button(frame, text="Şifreyi Değiştir", command=do_change, style="Accent.TButton").pack(pady=(4, 0))

    # ══════════════════════════════════════════════════════════
    # Admin: Config Sıfırlama
    # ══════════════════════════════════════════════════════════

    def _reset_config(self):
        if not messagebox.askyesno("⚠️ Config Sıfırla",
                "Bu işlem cihaz profilleri ve ayarları SİLER.\n\n"
                "Admin kullanıcı adı ve şifresi ETKİLENMEZ.\n\n"
                "Devam edilsin mi?"):
            return
        try:
            reset_config()
            self.config = DEFAULT_CONFIG.copy()
            self._refresh_admin_device_list()
            # Ayar alanlarını güncelle
            self.admin_baud_var.set("115200")
            self.admin_retry_var.set("7")
            self.admin_pkt_var.set("128")
            self.admin_port_var.set("COM7")
            messagebox.showinfo("Başarılı", "🗑 Config sıfırlandı. Admin bilgileri korundu.")
        except Exception as e:
            messagebox.showerror("Hata", f"Sıfırlama hatası: {e}")

    # ══════════════════════════════════════════════════════════
    # Admin: STM32 Key Güncelleme
    # ══════════════════════════════════════════════════════════

    def _update_stm32_key_dialog(self):
        devices = self.config.get("devices", [])
        if not devices:
            messagebox.showwarning("Uyarı", "Önce bir cihaz ekleyin!")
            return

        dialog = tk.Toplevel(self.root)
        dialog.title("🔄 STM32 AES Key Güncelle")
        dialog.geometry("500x340")
        dialog.resizable(False, False)
        dialog.configure(bg=C["bg"])
        dialog.transient(self.root)
        dialog.grab_set()

        frame = ttk.Frame(dialog, style="Main.TFrame", padding=20)
        frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frame, text="Cihaz Seçin:", style="Main.TLabel").pack(anchor="w")
        dev_var = tk.StringVar(value=devices[0]["name"])
        dev_combo = ttk.Combobox(frame, textvariable=dev_var, values=[d["name"] for d in devices], state="readonly", width=40)
        dev_combo.pack(fill=tk.X, pady=(2, 8))

        ttk.Label(frame, text="Yeni AES Key (64 hex karakter = 32 byte):", style="Main.TLabel").pack(anchor="w")
        new_var = tk.StringVar()
        ttk.Entry(frame, textvariable=new_var, width=66).pack(fill=tk.X, pady=(2, 4))

        ttk.Label(frame, text="Yeni AES Key (tekrar):", style="Main.TLabel").pack(anchor="w")
        new2_var = tk.StringVar()
        ttk.Entry(frame, textvariable=new2_var, width=66).pack(fill=tk.X, pady=(2, 8))

        ttk.Label(frame, text="STM32 bootloader modunda ve UART bağlı olmalıdır.", style="Status.TLabel").pack(anchor="w", pady=(0, 6))

        def do_update():
            new_key = new_var.get().strip()
            if not new_key or new_key != new2_var.get().strip():
                messagebox.showerror("Hata", "Key'ler eşleşmiyor veya boş!", parent=dialog)
                return
            if len(new_key) != 64:
                messagebox.showerror("Hata", "Key 64 hex karakter olmalı!", parent=dialog)
                return
            try:
                bytes.fromhex(new_key)
            except ValueError:
                messagebox.showerror("Hata", "Geçerli hex formatında değil!", parent=dialog)
                return

            sel_name = dev_var.get()
            sel_dev = None
            for d in devices:
                if d["name"] == sel_name:
                    sel_dev = d
                    break
            if not sel_dev:
                return

            if not messagebox.askyesno("Onay", "STM32'deki AES key değiştirilecek.\n⚠️ Bu işlem geri alınamaz!\n\nDevam?", parent=dialog):
                return

            config = {
                "serial_port": self.port_var.get(),
                "baud_rate": self.config.get("baud_rate", 115200),
                "aes_key_hex": sel_dev["aes_key_hex"],
            }
            dialog.destroy()

            def worker():
                success = update_stm32_key(config, new_key, log=self._log_callback)
                if success:
                    sel_dev["aes_key_hex"] = new_key
                    self.root.after(0, lambda: self._log_msg("ℹ️ Cihaz AES Key güncellendi. 'Tüm Ayarları Kaydet' ile kaydedin."))

            threading.Thread(target=worker, daemon=True).start()

        ttk.Button(frame, text="🔄 Güncelle", command=do_update, style="Accent.TButton").pack(pady=(4, 0))


# ══════════════════════════════════════════════════════════════
# Uygulama Başlatma
# ══════════════════════════════════════════════════════════════

def main():
    root = tk.Tk()
    try:
        root.iconbitmap(default='')
    except Exception:
        pass
    app = FirmwareUpdaterApp(root)
    root.mainloop()
    sys.exit(0)


if __name__ == "__main__":
    main()
