import os
import sys
import threading
import urllib.parse
from typing import Any, Dict, List, Optional, Tuple

try:
    import serial.tools.list_ports as serial_list_ports
except ImportError:
    serial_list_ports = None

try:
    from PySide6.QtCore import QObject, QFile, Signal, Qt, QTimer, QPropertyAnimation, QEasingCurve, QDateTime, QPoint
    from PySide6.QtUiTools import QUiLoader
    from PySide6.QtWidgets import (
        QApplication,
        QCheckBox,
        QComboBox,
        QDialog,
        QDialogButtonBox,
        QFormLayout,
        QGroupBox,
        QLineEdit,
        QListWidget,
        QMainWindow,
        QMessageBox,
        QPlainTextEdit,
        QProgressBar,
        QPushButton,
        QListWidgetItem,
        QTabWidget,
        QStackedWidget,
        QLabel,
        QWidget,
        QGraphicsOpacityEffect,
        QFrame,
        QHBoxLayout,
        QVBoxLayout,
    )
except ImportError as exc:
    raise SystemExit(
        "PySide6 bulunamadi. Once `pip install PySide6` veya "
        "`pip install -r requirements.txt` calistirin."
    ) from exc

from config_manager import (
    DEFAULT_CONFIG,
    change_admin_credentials,
    config_exists,
    load_config,
    load_credentials,
    reset_config,
    save_config,
    verify_admin,
)
from device_monitor import DeviceMonitor
from drive_manager import DriveManager
from firmware_proxy_client import FirmwareProxyClient
from firmware_proxy_server import create_proxy_server
from uploder import update_stm32_key, upload_firmware


def resource_path(*parts: str) -> str:
    if getattr(sys, "frozen", False):
        base_dir = getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
    else:
        base_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(base_dir, *parts)


class UiSignals(QObject):
    log = Signal(str)
    progress = Signal(int, int)
    scan_finished = Signal(object, object)
    upload_finished = Signal(bool)
    key_update_finished = Signal(bool, object)
    device_log = Signal(str, int, str, str)  # level, code, msg, timestamp


class FirmwareUpdaterQtApp:
    def __init__(self) -> None:
        self.config: Dict[str, Any] = DEFAULT_CONFIG.copy()
        self.admin_password: Optional[str] = None
        self.drive_manager: Optional[DriveManager] = None
        self.proxy_client: Optional[FirmwareProxyClient] = None
        self.proxy_server: Optional[Any] = None
        self.proxy_server_thread: Optional[threading.Thread] = None
        self._proxy_runtime_config: Optional[Dict[str, Any]] = None
        self.available_files: List[Dict[str, Any]] = []
        self.pending_firmware_version: Optional[int] = None
        self.stop_requested = False
        self.upload_thread: Optional[threading.Thread] = None

        self.device_monitor: Optional[DeviceMonitor] = None
        self._monitor_dialog: Optional[QDialog] = None
        self._monitor_log_widget: Optional[QPlainTextEdit] = None
        self._monitor_status_label: Optional[QLabel] = None
        self._monitor_connect_btn: Optional[QPushButton] = None
        self._monitor_was_running: bool = False
        self._monitor_port: Optional[str] = None

        self.signals = UiSignals()
        self.signals.log.connect(self._append_log)
        self.signals.progress.connect(self._on_worker_progress)
        self.signals.scan_finished.connect(self._on_scan_finished)
        self.signals.upload_finished.connect(self._on_upload_finished)
        self.signals.key_update_finished.connect(self._on_key_update_finished)
        self.signals.device_log.connect(self._on_device_log_message)

        self.window = self._load_main_window()
        self._bind_widgets()
        self._connect_ui()

        self.current_step = 0
        self.connection_mode: Optional[str] = None
        self._show_admin_aes = False
        self._config_reveal_done = False
        self._shake_animations: List[QPropertyAnimation] = []
        self._upload_running = False

        self.admin_window: Optional[QMainWindow] = None
        self.admin_tabs: Optional[QTabWidget] = None
        self.device_list_widget: Optional[QListWidget] = None
        self.device_search_edit: Optional[QLineEdit] = None
        self.device_count_label: Optional[QLabel] = None
        self.meta_name_value: Optional[QLabel] = None
        self.meta_fw_value: Optional[QLabel] = None
        self.meta_last_value: Optional[QLabel] = None
        self.meta_drive_value: Optional[QLabel] = None
        self.meta_aes_value: Optional[QLabel] = None
        self.toggle_aes_button: Optional[QPushButton] = None
        self.close_admin_button: Optional[QPushButton] = None
        self.baud_edit: Optional[QLineEdit] = None
        self.retry_edit: Optional[QLineEdit] = None
        self.packet_edit: Optional[QLineEdit] = None
        self.default_port_edit: Optional[QLineEdit] = None
        self.service_json_edit: Optional[QLineEdit] = None
        self.proxy_service_account_edit: Optional[QLineEdit] = None
        self.proxy_channel_map_edit: Optional[QLineEdit] = None
        self.add_device_button: Optional[QPushButton] = None
        self.edit_device_button: Optional[QPushButton] = None
        self.delete_device_button: Optional[QPushButton] = None
        self.save_all_button: Optional[QPushButton] = None
        self.change_password_button: Optional[QPushButton] = None
        self.reset_config_button: Optional[QPushButton] = None
        self.update_stm32_key_button: Optional[QPushButton] = None
        self.proxy_start_button: Optional[QPushButton] = None
        self.proxy_stop_button: Optional[QPushButton] = None
        self.proxy_health_button: Optional[QPushButton] = None
        self.proxy_runtime_status_label: Optional[QLabel] = None
        self.admin_status_label: Optional[QLabel] = None

        self._loading_frames = [" .", " ..", " ...", " ...."]
        self._status_anim_base = ""
        self._status_anim_step = 0
        self._update_anim_base = ""
        self._update_anim_step = 0
        self._status_anim_timer = QTimer(self.window)
        self._status_anim_timer.setInterval(260)
        self._status_anim_timer.timeout.connect(self._tick_status_animation)
        self._update_anim_timer = QTimer(self.window)
        self._update_anim_timer.setInterval(260)
        self._update_anim_timer.timeout.connect(self._tick_update_animation)
        self._clock_timer = QTimer(self.window)
        self._clock_timer.setInterval(1000)
        self._clock_timer.timeout.connect(self._update_clock)
        self._progress_shimmer_phase = 0.0
        self._progress_visual_mode = "idle"
        self._progress_shimmer_timer = QTimer(self.window)
        self._progress_shimmer_timer.setInterval(80)
        self._progress_shimmer_timer.timeout.connect(self._tick_progress_shimmer)
        self._progress_busy_refs = 0
        self._upload_button_opacity = QGraphicsOpacityEffect(self.start_upload_button)
        self._upload_button_opacity.setOpacity(1.0)
        self.start_upload_button.setGraphicsEffect(self._upload_button_opacity)
        self._upload_button_pulse = QPropertyAnimation(self._upload_button_opacity, b"opacity", self.window)
        self._upload_button_pulse.setDuration(920)
        self._upload_button_pulse.setStartValue(1.0)
        self._upload_button_pulse.setKeyValueAt(0.5, 0.72)
        self._upload_button_pulse.setEndValue(1.0)
        self._upload_button_pulse.setLoopCount(-1)
        self._window_fade_animation: Optional[QPropertyAnimation] = None
        self._admin_window_fade_animation: Optional[QPropertyAnimation] = None
        self._rf_pulse_effect = QGraphicsOpacityEffect(self.rf_pulse_icon)
        self.rf_pulse_icon.setGraphicsEffect(self._rf_pulse_effect)
        self._rf_pulse_animation = QPropertyAnimation(self._rf_pulse_effect, b"opacity", self.window)
        self._rf_pulse_animation.setDuration(1100)
        self._rf_pulse_animation.setStartValue(0.25)
        self._rf_pulse_animation.setKeyValueAt(0.5, 1.0)
        self._rf_pulse_animation.setEndValue(0.25)
        self._rf_pulse_animation.setLoopCount(-1)
        self._success_overlay: Optional[QFrame] = None
        self._success_card: Optional[QFrame] = None
        self._success_check_label: Optional[QLabel] = None
        self._success_text_label: Optional[QLabel] = None
        self._success_overlay_opacity: Optional[QGraphicsOpacityEffect] = None
        self._success_check_opacity: Optional[QGraphicsOpacityEffect] = None
        self._success_overlay_anim: Optional[QPropertyAnimation] = None
        self._success_check_anim: Optional[QPropertyAnimation] = None

        self._configure_styles()
        self._build_success_overlay()
        self._initialize_wizard_state()
        self._set_progress_visual_mode("idle")

        self._try_load_config()
        self._scan_ports()
        self._refresh_admin_panel()
        self._update_clock()
        self._clock_timer.start()
        self._set_status("Hazir")
        self._append_log("Wizard arayuzu acildi.")

    def _load_main_window(self) -> QMainWindow:
        ui_path = resource_path("ui", "main_window.ui")
        ui_file = QFile(ui_path)
        if not ui_file.open(QFile.ReadOnly):
            raise RuntimeError(f"UI dosyasi acilamadi: {ui_path}")

        loader = QUiLoader()
        loaded = loader.load(ui_file)
        ui_file.close()

        if not isinstance(loaded, QMainWindow):
            raise RuntimeError("Yuklenen UI bir QMainWindow degil.")
        return loaded

    def _w(self, cls, name: str):
        widget = self.window.findChild(cls, name)
        if widget is None:
            raise RuntimeError(f"Widget bulunamadi: {name}")
        return widget

    def _load_admin_window(self) -> QMainWindow:
        ui_path = resource_path("ui", "admin_window.ui")
        ui_file = QFile(ui_path)
        if not ui_file.open(QFile.ReadOnly):
            raise RuntimeError(f"Admin UI dosyasi acilamadi: {ui_path}")

        loader = QUiLoader()
        loaded = loader.load(ui_file)
        ui_file.close()

        if not isinstance(loaded, QMainWindow):
            raise RuntimeError("Yuklenen admin UI bir QMainWindow degil.")
        return loaded

    def _aw(self, cls, name: str):
        if self.admin_window is None:
            raise RuntimeError("Admin penceresi yuklenmedi.")
        widget = self.admin_window.findChild(cls, name)
        if widget is None:
            raise RuntimeError(f"Admin widget bulunamadi: {name}")
        return widget

    def _ensure_admin_window(self) -> None:
        if self.admin_window is not None:
            return

        self.admin_window = self._load_admin_window()
        self._bind_admin_widgets()
        self._connect_admin_ui()
        self._configure_admin_styles()
        self._refresh_admin_panel()

    def _bind_admin_widgets(self) -> None:
        self.admin_tabs = self._aw(QTabWidget, "adminTabs")
        self.device_list_widget = self._aw(QListWidget, "deviceListWidget")
        self.device_search_edit = self._aw(QLineEdit, "deviceSearchEdit")
        self.device_count_label = self._aw(QLabel, "deviceCountLabel")
        self.meta_name_value = self._aw(QLabel, "metaNameValue")
        self.meta_fw_value = self._aw(QLabel, "metaFwValue")
        self.meta_last_value = self._aw(QLabel, "metaLastValue")
        self.meta_drive_value = self._aw(QLabel, "metaDriveValue")
        self.meta_aes_value = self._aw(QLabel, "metaAesValue")
        self.toggle_aes_button = self._aw(QPushButton, "toggleAesButton")
        self.close_admin_button = self._aw(QPushButton, "closeAdminButton")

        self.add_device_button = self._aw(QPushButton, "addDeviceButton")
        self.edit_device_button = self._aw(QPushButton, "editDeviceButton")
        self.delete_device_button = self._aw(QPushButton, "deleteDeviceButton")
        self.baud_edit = self._aw(QLineEdit, "baudEdit")
        self.retry_edit = self._aw(QLineEdit, "retryEdit")
        self.packet_edit = self._aw(QLineEdit, "packetEdit")
        self.default_port_edit = self._aw(QLineEdit, "defaultPortEdit")
        self.service_json_edit = self._aw(QLineEdit, "serviceJsonEdit")
        self.proxy_service_account_edit = self._aw(QLineEdit, "proxyServiceAccountEdit")
        self.proxy_channel_map_edit = self._aw(QLineEdit, "proxyChannelMapEdit")
        self.save_all_button = self._aw(QPushButton, "saveAllButton")
        self.change_password_button = self._aw(QPushButton, "changePasswordButton")
        self.reset_config_button = self._aw(QPushButton, "resetConfigButton")
        self.update_stm32_key_button = self._aw(QPushButton, "updateStm32KeyButton")
        self.proxy_start_button = self._aw(QPushButton, "proxyStartButton")
        self.proxy_stop_button = self._aw(QPushButton, "proxyStopButton")
        self.proxy_health_button = self._aw(QPushButton, "proxyHealthButton")
        self.proxy_runtime_status_label = self._aw(QLabel, "proxyRuntimeStatusLabel")
        self.admin_status_label = self._aw(QLabel, "adminStatusLabel")

    def _connect_admin_ui(self) -> None:
        if self.close_admin_button is not None and self.admin_window is not None:
            self.close_admin_button.clicked.connect(self.admin_window.close)
        if self.toggle_aes_button is not None:
            self.toggle_aes_button.clicked.connect(self._toggle_admin_aes_visibility)

        if self.device_search_edit is not None:
            self.device_search_edit.textChanged.connect(self._apply_admin_device_filter)
        if self.device_list_widget is not None:
            self.device_list_widget.currentItemChanged.connect(self._on_admin_device_selection_changed)

        if self.add_device_button is not None:
            self.add_device_button.clicked.connect(self._add_device)
        if self.edit_device_button is not None:
            self.edit_device_button.clicked.connect(self._edit_selected_device)
        if self.delete_device_button is not None:
            self.delete_device_button.clicked.connect(self._delete_selected_device)
        if self.save_all_button is not None:
            self.save_all_button.clicked.connect(self._save_all_config)
        if self.change_password_button is not None:
            self.change_password_button.clicked.connect(self._change_password_dialog)
        if self.reset_config_button is not None:
            self.reset_config_button.clicked.connect(self._reset_config_dialog)
        if self.update_stm32_key_button is not None:
            self.update_stm32_key_button.clicked.connect(self._update_stm32_key_dialog)
        if self.proxy_start_button is not None:
            self.proxy_start_button.clicked.connect(self._start_local_proxy_from_admin)
        if self.proxy_stop_button is not None:
            self.proxy_stop_button.clicked.connect(self._stop_local_proxy_from_admin)
        if self.proxy_health_button is not None:
            self.proxy_health_button.clicked.connect(self._test_proxy_connection)

    def _open_admin_window(self) -> None:
        self._ensure_admin_window()
        if self.admin_window is None:
            return
        self._refresh_admin_panel()
        self.admin_window.show()
        self.admin_window.raise_()
        self.admin_window.activateWindow()
        self._play_admin_intro_animation()
        self._refresh_proxy_runtime_status()
        self._set_admin_status("Admin paneli acik")

    def _dialog_parent(self) -> QWidget:
        if self.admin_window is not None and self.admin_window.isVisible():
            return self.admin_window
        return self.window

    def _bind_widgets(self) -> None:
        self.wizard_stack: QStackedWidget = self._w(QStackedWidget, "wizardStack")
        self.step1_badge: QLabel = self._w(QLabel, "step1Badge")
        self.step2_badge: QLabel = self._w(QLabel, "step2Badge")
        self.step3_badge: QLabel = self._w(QLabel, "step3Badge")
        self.back_step_button: QPushButton = self._w(QPushButton, "backStepButton")
        self.next_step_button: QPushButton = self._w(QPushButton, "nextStepButton")
        self.mode_cards_container: QFrame = self._w(QFrame, "modeCardsContainer")
        self.mode_serial_card: QPushButton = self._w(QPushButton, "modeSerialCard")
        self.mode_rf_card: QPushButton = self._w(QPushButton, "modeRfCard")
        self.mode_validation_label: QLabel = self._w(QLabel, "modeValidationLabel")
        self.config_hint_label: QLabel = self._w(QLabel, "configHintLabel")
        self.baud_value_label: QLabel = self._w(QLabel, "baudValueLabel")
        self.rf_pulse_icon: QLabel = self._w(QLabel, "rfPulseIcon")
        self.device_group_box: QGroupBox = self._w(QGroupBox, "deviceGroupBox")

        self.admin_login_button: QPushButton = self._w(QPushButton, "adminLoginButton")
        self.device_combo: QComboBox = self._w(QComboBox, "deviceCombo")
        self.port_combo: QComboBox = self._w(QComboBox, "portCombo")
        self.refresh_ports_button: QPushButton = self._w(QPushButton, "refreshPortsButton")
        self.rf_mode_check: QCheckBox = self._w(QCheckBox, "rfModeCheck")
        self.scan_folder_button: QPushButton = self._w(QPushButton, "scanFolderButton")
        self.update_status_label: QLabel = self._w(QLabel, "updateStatusLabel")
        self.firmware_combo: QComboBox = self._w(QComboBox, "firmwareCombo")
        self.firmware_info_label: QLabel = self._w(QLabel, "firmwareInfoLabel")
        self.start_upload_button: QPushButton = self._w(QPushButton, "startUploadButton")
        self.progress_bar: QProgressBar = self._w(QProgressBar, "progressBar")
        self.progress_label: QLabel = self._w(QLabel, "progressLabel")
        self.completion_container: QFrame = self._w(QFrame, "completionContainer")
        self.completion_message_label: QLabel = self._w(QLabel, "completionMessageLabel")
        self.new_update_button: QPushButton = self._w(QPushButton, "newUpdateButton")
        self.edit_settings_button: QPushButton = self._w(QPushButton, "editSettingsButton")
        self.log_text: QPlainTextEdit = self._w(QPlainTextEdit, "logTextEdit")
        self.status_label: QLabel = self._w(QLabel, "statusLabel")
        self.clock_label: QLabel = self._w(QLabel, "clockLabel")
        self.version_badge: QLabel = self._w(QLabel, "versionBadge")
        self.size_badge: QLabel = self._w(QLabel, "sizeBadge")
        self.type_badge: QLabel = self._w(QLabel, "typeBadge")

        # Wrap long log lines for easier reading.
        self.log_text.setLineWrapMode(QPlainTextEdit.WidgetWidth)

        # Cihaz Logu butonu — status bar'a ekle
        self._monitor_open_btn = QPushButton("Cihaz Logu")
        self._monitor_open_btn.setProperty("role", "subtle")
        self._monitor_open_btn.setFixedWidth(110)
        sb = self.window.statusBar()
        sb.setStyleSheet(
            "QStatusBar { background: #0D1117; color: #8b949e;"
            " border-top: 1px solid #30363d; padding: 2px 8px; }"
        )
        sb.addPermanentWidget(self._monitor_open_btn)

    def _connect_ui(self) -> None:
        self.admin_login_button.clicked.connect(self._show_admin_login_dialog)
        self.back_step_button.clicked.connect(self._go_step_back)
        self.next_step_button.clicked.connect(self._go_step_next)
        self.mode_serial_card.clicked.connect(lambda: self._select_connection_mode("serial"))
        self.mode_rf_card.clicked.connect(lambda: self._select_connection_mode("rf"))
        self.refresh_ports_button.clicked.connect(self._scan_ports)
        self.scan_folder_button.clicked.connect(self._scan_folder)
        self.device_combo.currentIndexChanged.connect(self._on_device_selected)
        self.firmware_combo.currentIndexChanged.connect(self._on_firmware_selected)
        self.start_upload_button.clicked.connect(self._start_or_stop_upload)
        self.new_update_button.clicked.connect(self._reset_for_new_update)
        self.edit_settings_button.clicked.connect(self._jump_to_settings)
        self._monitor_open_btn.clicked.connect(self._open_device_monitor_dialog)

    def _hide_legacy_admin_tab(self) -> None:
        return

    def _shared_theme_stylesheet(self) -> str:
        return """
            QMainWindow, QWidget#centralwidget {
                background: #0D1117;
                color: #e6edf3;
                font-family: "Segoe UI Variable", "Segoe UI", sans-serif;
                font-size: 10pt;
            }
            QGroupBox {
                border: 1px solid #30363d;
                border-radius: 12px;
                margin-top: 12px;
                padding: 14px;
                background: rgba(22, 27, 34, 220);
                font-weight: 600;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 12px;
                padding: 0 6px;
                color: #d0deea;
            }
            QLineEdit, QComboBox, QListWidget, QPlainTextEdit {
                background: rgba(13, 17, 23, 230);
                color: #e6edf3;
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 8px;
                selection-background-color: #58A6FF;
            }
            QLineEdit:focus, QComboBox:focus, QListWidget:focus, QPlainTextEdit:focus {
                border: 1px solid #58A6FF;
            }
            QComboBox::drop-down {
                border: none;
                width: 28px;
            }
            QPushButton {
                background: rgba(22, 27, 34, 245);
                color: #e6edf3;
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 9px 14px;
                font-weight: 600;
            }
            QPushButton:hover {
                border: 1px solid #58A6FF;
                background: rgba(88, 166, 255, 36);
            }
            QPushButton:pressed {
                background: rgba(13, 17, 23, 245);
            }
            QPushButton:disabled {
                color: #6e7681;
                background: rgba(22, 27, 34, 130);
                border-color: #2a2f36;
            }
            QPushButton[role="primary"] {
                border: 1px solid #58A6FF;
                color: #ffffff;
                background: qlineargradient(
                    x1:0, y1:0, x2:1, y2:0,
                    stop:0 #2f81f7, stop:1 #58A6FF
                );
            }
            QPushButton[role="subtle"] {
                color: #c6d7eb;
                border: 1px solid #334155;
                background: rgba(22, 27, 34, 225);
            }
            QPushButton[role="danger"] {
                background: #64343b;
                border: 1px solid #96545d;
                color: #ffecef;
            }
            QPushButton[role="danger"]:hover {
                background: #7b424a;
                border: 1px solid #b86b74;
            }
            QLabel#statusLabel, QLabel#adminStatusLabel {
                background: rgba(22, 27, 34, 220);
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 8px 10px;
                color: #c9d1d9;
                font-weight: 600;
            }
            QCheckBox {
                color: #c9d1d9;
                spacing: 8px;
            }
            QCheckBox::indicator {
                width: 16px;
                height: 16px;
                border-radius: 4px;
                background: #0D1117;
                border: 1px solid #3d4a5d;
            }
            QCheckBox::indicator:checked {
                border: 1px solid #58A6FF;
                background: #58A6FF;
            }
            QProgressBar {
                background: rgba(13, 17, 23, 245);
                border: 1px solid #30363d;
                border-radius: 12px;
                min-height: 22px;
                text-align: center;
                color: #dbeafe;
            }
            QProgressBar::chunk {
                border-radius: 10px;
                background: #58A6FF;
            }
        """

    def _configure_styles(self) -> None:
        primary_buttons = [self.start_upload_button, self.next_step_button]
        subtle_buttons = [self.scan_folder_button, self.refresh_ports_button, self.admin_login_button, self.back_step_button]
        loopback_buttons = [self.new_update_button, self.edit_settings_button]
        all_buttons = [
            self.admin_login_button,
            self.refresh_ports_button,
            self.scan_folder_button,
            self.start_upload_button,
            self.back_step_button,
            self.next_step_button,
            self.new_update_button,
            self.edit_settings_button,
        ]

        self.mode_serial_card.setProperty("modeCard", "true")
        self.mode_rf_card.setProperty("modeCard", "true")
        self.mode_serial_card.setCursor(Qt.PointingHandCursor)
        self.mode_rf_card.setCursor(Qt.PointingHandCursor)

        for btn in all_buttons:
            btn.setProperty("role", "default")
            btn.setCursor(Qt.PointingHandCursor)
        for btn in primary_buttons:
            btn.setProperty("role", "primary")
        for btn in subtle_buttons:
            btn.setProperty("role", "subtle")
        for btn in loopback_buttons:
            btn.setProperty("role", "loopback")

        self.window.setStyleSheet(
            self._shared_theme_stylesheet()
            + """
            QMainWindow, QWidget#centralwidget {
                background: #0D1117;
                color: #e6edf3;
                font-family: "Segoe UI Variable", "Segoe UI", sans-serif;
                font-size: 10pt;
            }
            QLabel#headerLabel {
                color: #f0f6fc;
                font-size: 26px;
                font-weight: 700;
            }
            QLabel#clockLabel {
                background: rgba(22, 27, 34, 235);
                border: 1px solid rgba(88, 166, 255, 120);
                border-radius: 12px;
                color: #c9d9ef;
                font-weight: 600;
                padding: 7px 12px;
                min-width: 182px;
            }
            QLabel#rfPulseIcon {
                background: rgba(35, 134, 54, 120);
                border: 1px solid #238636;
                border-radius: 12px;
                color: #3fb950;
                font-weight: 700;
                padding: 6px 10px;
                min-width: 34px;
            }
            QLabel#step1Badge, QLabel#step2Badge, QLabel#step3Badge {
                background: rgba(22, 27, 34, 210);
                border: 1px solid #30363d;
                border-radius: 12px;
                color: #8b949e;
                padding: 9px;
                font-weight: 600;
            }
            QLabel[stepState="active"] {
                border: 1px solid #58A6FF;
                color: #dbeafe;
                background: rgba(88, 166, 255, 36);
            }
            QLabel[stepState="done"] {
                border: 1px solid #238636;
                color: #b8f5ca;
                background: rgba(35, 134, 54, 30);
            }
            QStackedWidget#wizardStack {
                background: rgba(22, 27, 34, 200);
                border: 1px solid #30363d;
                border-radius: 12px;
            }
            QGroupBox {
                border: 1px solid #30363d;
                border-radius: 12px;
                margin-top: 12px;
                padding: 14px;
                background: rgba(22, 27, 34, 220);
                font-weight: 600;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 12px;
                padding: 0 6px;
                color: #d0deea;
            }
            QLabel#modeTitleLabel, QLabel#configTitleLabel, QLabel#firmwareTitleLabel {
                color: #f0f6fc;
                font-size: 19px;
                font-weight: 700;
            }
            QLabel#modeSubtitleLabel, QLabel#configHintLabel, QLabel#firmwareInfoLabel {
                color: #9fb1c5;
            }
            QLabel#modeValidationLabel {
                color: #ff7b72;
                font-weight: 600;
                padding: 4px 2px;
            }
            QPushButton[modeCard="true"] {
                background: rgba(22, 27, 34, 230);
                border: 1px solid #30363d;
                border-radius: 12px;
                color: #c9d1d9;
                font-size: 15px;
                font-weight: 700;
                text-align: left;
                padding: 18px;
            }
            QPushButton[modeCard="true"]:hover {
                border: 1px solid #58A6FF;
                background: rgba(88, 166, 255, 35);
                color: #ffffff;
                padding: 19px;
            }
            QPushButton[modeCardState="selected"] {
                border: 1px solid #58A6FF;
                background: rgba(88, 166, 255, 48);
                color: #ffffff;
            }
            QLineEdit, QComboBox, QListWidget, QPlainTextEdit {
                background: rgba(13, 17, 23, 230);
                color: #e6edf3;
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 8px;
                selection-background-color: #58A6FF;
            }
            QLineEdit:focus, QComboBox:focus, QListWidget:focus, QPlainTextEdit:focus {
                border: 1px solid #58A6FF;
            }
            QComboBox::drop-down {
                border: none;
                width: 28px;
            }
            QPushButton {
                background: rgba(22, 27, 34, 245);
                color: #e6edf3;
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 9px 14px;
                font-weight: 600;
            }
            QPushButton:hover {
                border: 1px solid #58A6FF;
                background: rgba(88, 166, 255, 36);
                padding: 10px 15px;
            }
            QPushButton:pressed {
                background: rgba(13, 17, 23, 245);
            }
            QPushButton:disabled {
                color: #6e7681;
                background: rgba(22, 27, 34, 130);
                border-color: #2a2f36;
            }
            QPushButton[role="primary"] {
                border: 1px solid #58A6FF;
                color: #ffffff;
                background: qlineargradient(
                    x1:0, y1:0, x2:1, y2:0,
                    stop:0 #2f81f7, stop:1 #58A6FF
                );
            }
            QPushButton[role="subtle"] {
                color: #c6d7eb;
                border: 1px solid #334155;
                background: rgba(22, 27, 34, 225);
            }
            QLabel#versionBadge, QLabel#sizeBadge, QLabel#typeBadge {
                background: rgba(13, 17, 23, 240);
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 6px 10px;
                color: #8b949e;
                font-weight: 600;
            }
            QLabel[badgeState="active"] {
                border: 1px solid #58A6FF;
                color: #dbeafe;
            }
            QLabel#statusLabel {
                background: rgba(22, 27, 34, 220);
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 8px 10px;
                color: #c9d1d9;
                font-weight: 600;
            }
            QLabel#updateStatusLabel {
                background: rgba(13, 17, 23, 240);
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 8px 10px;
                color: #9fb1c5;
            }
            QPlainTextEdit#logTextEdit {
                background: rgba(13, 17, 23, 250);
                color: #f0f6fc;
                border: 1px solid #3d4858;
                border-radius: 12px;
                padding: 10px;
                font-family: "Consolas", "Cascadia Mono", "Segoe UI Mono", monospace;
                font-size: 10.5pt;
            }
            QCheckBox {
                color: #c9d1d9;
                spacing: 8px;
            }
            QCheckBox::indicator {
                width: 16px;
                height: 16px;
                border-radius: 4px;
                background: #0D1117;
                border: 1px solid #3d4a5d;
            }
            QCheckBox::indicator:checked {
                border: 1px solid #58A6FF;
                background: #58A6FF;
            }
            QProgressBar {
                background: rgba(13, 17, 23, 245);
                border: 1px solid #30363d;
                border-radius: 12px;
                min-height: 22px;
                text-align: center;
                color: #dbeafe;
            }
            QProgressBar::chunk {
                border-radius: 10px;
                background: #58A6FF;
            }
            QFrame#completionContainer {
                background: rgba(22, 27, 34, 210);
                border: 1px solid #30363d;
                border-radius: 12px;
                padding: 10px;
            }
            QLabel#completionMessageLabel {
                color: #b8f5ca;
                font-size: 12pt;
                font-weight: 700;
            }
            QPushButton[role="loopback"] {
                border: 1px solid #4f8dd1;
                background: rgba(32, 50, 74, 210);
                color: #eaf3ff;
            }
            QPushButton[role="loopback"]:hover {
                border: 1px solid #58A6FF;
                background: rgba(88, 166, 255, 44);
            }
            QFrame#successOverlay {
                background: rgba(13, 17, 23, 166);
            }
            QFrame#successOverlayCard {
                background: rgba(22, 27, 34, 238);
                border: 1px solid #238636;
                border-radius: 14px;
            }
            QLabel#successOverlayCheck {
                color: #3fb950;
                font-size: 50px;
                font-weight: 800;
            }
            QLabel#successOverlayText {
                color: #d2f7df;
                font-size: 13pt;
                font-weight: 700;
            }
            """
        )

        self.update_status_label.setText("Liste bekleniyor")
        self.status_label.setText("Hazir")
        self.mode_validation_label.setVisible(False)
        self.rf_pulse_icon.setVisible(False)
        self.refresh_ports_button.setText("Yenile")
        self.scan_folder_button.setText("Listeyi Yenile")
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_label.setText("0%")
        self.completion_container.setVisible(False)
        self.completion_message_label.setText("Guncelleme tamamlandi.")
        self.version_badge.setProperty("badgeState", "idle")
        self.size_badge.setProperty("badgeState", "idle")
        self.type_badge.setProperty("badgeState", "idle")
        self._refresh_dynamic_style(self.version_badge)
        self._refresh_dynamic_style(self.size_badge)
        self._refresh_dynamic_style(self.type_badge)

    def _refresh_dynamic_style(self, widget: QWidget) -> None:
        widget.style().unpolish(widget)
        widget.style().polish(widget)
        widget.update()

    def _initialize_wizard_state(self) -> None:
        self.current_step = 0
        self._set_step(0, animated=False)
        self.rf_mode_check.setVisible(False)
        self.rf_mode_check.setChecked(False)
        self.mode_serial_card.setProperty("modeCardState", "idle")
        self.mode_rf_card.setProperty("modeCardState", "idle")
        self._refresh_dynamic_style(self.mode_serial_card)
        self._refresh_dynamic_style(self.mode_rf_card)
        self.baud_value_label.setText(str(self.config.get("baud_rate", 115200)))
        self._set_mode_validation("")
        self._apply_firmware_badges(None)
        self._set_completion_actions_visible(False)
        self._set_selection_controls_enabled(True, animated=False)
        self._hide_success_overlay(animate=False)

    def _set_mode_validation(self, text: str) -> None:
        self.mode_validation_label.setText(text)
        self.mode_validation_label.setVisible(bool(text))

    def _build_success_overlay(self) -> None:
        central = self.window.centralWidget()
        if central is None:
            return

        overlay = QFrame(central)
        overlay.setObjectName("successOverlay")
        overlay.setVisible(False)
        overlay.setGeometry(central.rect())

        root_layout = QVBoxLayout(overlay)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.addStretch(1)

        card_row = QHBoxLayout()
        card_row.addStretch(1)

        card = QFrame(overlay)
        card.setObjectName("successOverlayCard")
        card.setMinimumWidth(360)
        card_layout = QVBoxLayout(card)
        card_layout.setContentsMargins(28, 24, 28, 24)
        card_layout.setSpacing(8)

        check_label = QLabel("✓", card)
        check_label.setObjectName("successOverlayCheck")
        check_label.setAlignment(Qt.AlignCenter)
        text_label = QLabel("Guncelleme Basarili", card)
        text_label.setObjectName("successOverlayText")
        text_label.setAlignment(Qt.AlignCenter)

        card_layout.addWidget(check_label)
        card_layout.addWidget(text_label)
        card_row.addWidget(card)
        card_row.addStretch(1)

        root_layout.addLayout(card_row)
        root_layout.addStretch(1)

        overlay_opacity = QGraphicsOpacityEffect(overlay)
        overlay_opacity.setOpacity(0.0)
        overlay.setGraphicsEffect(overlay_opacity)

        check_opacity = QGraphicsOpacityEffect(check_label)
        check_opacity.setOpacity(1.0)
        check_label.setGraphicsEffect(check_opacity)

        self._success_overlay = overlay
        self._success_card = card
        self._success_check_label = check_label
        self._success_text_label = text_label
        self._success_overlay_opacity = overlay_opacity
        self._success_check_opacity = check_opacity

    def _set_completion_actions_visible(self, visible: bool, message: str = "Guncelleme tamamlandi.") -> None:
        self.completion_message_label.setText(message)
        self.completion_container.setVisible(visible)
        if visible:
            self._animate_widget_fade(self.completion_container)

    def _set_selection_controls_enabled(self, enabled: bool, animated: bool = True) -> None:
        controls: List[QWidget] = [
            self.mode_serial_card,
            self.mode_rf_card,
            self.device_combo,
            self.port_combo,
            self.refresh_ports_button,
            self.rf_mode_check,
            self.scan_folder_button,
            self.firmware_combo,
            self.back_step_button,
            self.next_step_button,
        ]

        for control in controls:
            control.setEnabled(enabled)

        if enabled and animated:
            for widget in (self.mode_cards_container, self.device_group_box, self.firmware_combo):
                self._animate_widget_fade(widget)

    def _show_success_overlay(self) -> None:
        if self._success_overlay is None or self._success_overlay_opacity is None:
            return

        central = self.window.centralWidget()
        if central is not None:
            self._success_overlay.setGeometry(central.rect())
        self._success_overlay.show()
        self._success_overlay.raise_()

        if self._success_overlay_anim is not None:
            self._success_overlay_anim.stop()
        if self._success_check_anim is not None:
            self._success_check_anim.stop()

        self._success_overlay_anim = QPropertyAnimation(self._success_overlay_opacity, b"opacity", self.window)
        self._success_overlay_anim.setDuration(240)
        self._success_overlay_anim.setStartValue(0.0)
        self._success_overlay_anim.setEndValue(1.0)
        self._success_overlay_anim.setEasingCurve(QEasingCurve.OutCubic)
        self._success_overlay_anim.start()

        if self._success_check_opacity is not None:
            self._success_check_anim = QPropertyAnimation(self._success_check_opacity, b"opacity", self.window)
            self._success_check_anim.setDuration(700)
            self._success_check_anim.setStartValue(0.40)
            self._success_check_anim.setKeyValueAt(0.50, 1.0)
            self._success_check_anim.setEndValue(0.65)
            self._success_check_anim.setLoopCount(2)
            self._success_check_anim.start()

        QTimer.singleShot(1850, lambda: self._hide_success_overlay(animate=True))

    def _hide_success_overlay(self, animate: bool = True) -> None:
        if self._success_overlay is None or self._success_overlay_opacity is None:
            return
        if not self._success_overlay.isVisible():
            return

        if not animate:
            self._success_overlay_opacity.setOpacity(0.0)
            self._success_overlay.hide()
            return

        if self._success_overlay_anim is not None:
            self._success_overlay_anim.stop()
        self._success_overlay_anim = QPropertyAnimation(self._success_overlay_opacity, b"opacity", self.window)
        self._success_overlay_anim.setDuration(220)
        self._success_overlay_anim.setStartValue(self._success_overlay_opacity.opacity())
        self._success_overlay_anim.setEndValue(0.0)
        self._success_overlay_anim.setEasingCurve(QEasingCurve.InCubic)
        self._success_overlay_anim.finished.connect(self._success_overlay.hide)
        self._success_overlay_anim.start()

    def _reset_for_new_update(self) -> None:
        if self._upload_running:
            return
        self._hide_success_overlay(animate=False)
        self._set_completion_actions_visible(False)
        self._set_selection_controls_enabled(True, animated=True)
        self._set_progress_visual_mode("idle")
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_label.setText("0%")
        self.start_upload_button.setText("Guncellemeyi Baslat")
        self._set_upload_button_pulse(False)
        self.connection_mode = None
        self.mode_serial_card.setProperty("modeCardState", "idle")
        self.mode_rf_card.setProperty("modeCardState", "idle")
        self._refresh_dynamic_style(self.mode_serial_card)
        self._refresh_dynamic_style(self.mode_rf_card)
        self.rf_mode_check.setVisible(False)
        self.rf_mode_check.setChecked(False)
        self._set_rf_indicator(False)
        self._set_mode_validation("")
        self._set_step(0)
        self._set_status("Yeni guncelleme icin hazir.")
        self._append_log("Yeni guncelleme dongusu baslatildi.")

    def _jump_to_settings(self) -> None:
        if self._upload_running:
            return
        self._hide_success_overlay(animate=False)
        self._set_completion_actions_visible(False)
        self._set_selection_controls_enabled(True, animated=True)
        target_step = 1 if self.connection_mode else 0
        self._set_step(target_step)
        self._set_status("Ayarlar duzenleme moduna donuldu.")

    def _set_step(self, step: int, animated: bool = True) -> None:
        self.current_step = max(0, min(2, step))
        self.wizard_stack.setCurrentIndex(self.current_step)
        self._update_step_badges()
        self._update_step_nav()

        if self.current_step == 1 and animated:
            self._animate_widget_fade(self.device_group_box)
        if self.current_step == 1:
            self._apply_mode_on_step2()
        if self.current_step == 2:
            self._set_mode_validation("")

    def _go_step_next(self) -> None:
        if self._upload_running:
            return

        if self.current_step == 0 and not self.connection_mode:
            self._set_mode_validation("Baglanti modu secilmeden devam edemezsiniz.")
            self._shake_widget(self.mode_cards_container)
            return

        if self.current_step == 1:
            if self.device_combo.count() == 0 or "Cihaz yok" in self.device_combo.currentText():
                self._set_status("Once bir cihaz tanimlamaniz gerekiyor.")
                self._shake_widget(self.device_combo)
                return
            if self.port_combo.count() == 0 or "bulunamadi" in self.port_combo.currentText().lower():
                self._set_status("Gecerli bir port secmeden devam edemezsiniz.")
                self._shake_widget(self.port_combo)
                return

        if self.current_step < 2:
            self._set_step(self.current_step + 1)

    def _go_step_back(self) -> None:
        if self._upload_running:
            return
        if self.current_step > 0:
            self._set_step(self.current_step - 1)

    def _update_step_nav(self) -> None:
        self.back_step_button.setEnabled(self.current_step > 0)
        if self.current_step < 2:
            self.next_step_button.setEnabled(True)
            self.next_step_button.setText("Ileri")
        else:
            self.next_step_button.setEnabled(False)
            self.next_step_button.setText("Son Adim")

    def _update_step_badges(self) -> None:
        badges = [self.step1_badge, self.step2_badge, self.step3_badge]
        for idx, badge in enumerate(badges):
            state = "idle"
            if idx < self.current_step:
                state = "done"
            elif idx == self.current_step:
                state = "active"
            badge.setProperty("stepState", state)
            self._refresh_dynamic_style(badge)

    def _select_connection_mode(self, mode: str) -> None:
        if self._upload_running:
            return
        self.connection_mode = mode
        self.mode_serial_card.setProperty("modeCardState", "selected" if mode == "serial" else "idle")
        self.mode_rf_card.setProperty("modeCardState", "selected" if mode == "rf" else "idle")
        self._refresh_dynamic_style(self.mode_serial_card)
        self._refresh_dynamic_style(self.mode_rf_card)
        self._set_mode_validation("")
        self._apply_mode_on_step2()

    def _apply_mode_on_step2(self) -> None:
        is_rf = self.connection_mode == "rf"
        self.rf_mode_check.setChecked(is_rf)
        self.rf_mode_check.setVisible(is_rf)
        if is_rf:
            self.device_group_box.setTitle("RF Modu - Donanim Ayarlari")
            self.config_hint_label.setText("RF gateway baglantisi aktif. Portu dogrulayin ve devam edin.")
            self._set_rf_indicator(True)
            self._set_status("RF modu secildi.")
        else:
            self.device_group_box.setTitle("Seri Port Modu - Donanim Ayarlari")
            self.config_hint_label.setText("USB uzerinden dogrudan seri port guncellemesi.")
            self._set_rf_indicator(False)
            self._set_status("Seri port modu secildi.")
        self._animate_widget_fade(self.device_group_box)

    def _set_rf_indicator(self, enabled: bool) -> None:
        self.rf_pulse_icon.setVisible(enabled)
        if enabled:
            self._rf_pulse_animation.start()
        else:
            self._rf_pulse_animation.stop()
            self._rf_pulse_effect.setOpacity(1.0)

    def _animate_widget_fade(self, widget: Optional[QWidget]) -> None:
        if widget is None:
            return
        effect = QGraphicsOpacityEffect(widget)
        widget.setGraphicsEffect(effect)
        anim = QPropertyAnimation(effect, b"opacity", self.window)
        anim.setDuration(260)
        anim.setStartValue(0.0)
        anim.setEndValue(1.0)
        anim.setEasingCurve(QEasingCurve.OutCubic)
        anim.finished.connect(lambda: widget.setGraphicsEffect(None))
        anim.start()

    def _shake_widget(self, widget: Optional[QWidget]) -> None:
        if widget is None:
            return
        base = widget.pos()
        anim = QPropertyAnimation(widget, b"pos", self.window)
        anim.setDuration(260)
        anim.setKeyValueAt(0.0, base)
        anim.setKeyValueAt(0.20, base + QPoint(-8, 0))
        anim.setKeyValueAt(0.40, base + QPoint(8, 0))
        anim.setKeyValueAt(0.60, base + QPoint(-6, 0))
        anim.setKeyValueAt(0.80, base + QPoint(6, 0))
        anim.setKeyValueAt(1.0, base)
        self._shake_animations.append(anim)
        anim.finished.connect(lambda: self._shake_animations.remove(anim) if anim in self._shake_animations else None)
        anim.start()

    def _set_progress_visual_mode(self, mode: str) -> None:
        self._progress_visual_mode = mode
        if mode == "active":
            if not self._progress_shimmer_timer.isActive():
                self._progress_shimmer_phase = 0.0
                self._progress_shimmer_timer.start()
        else:
            self._progress_shimmer_timer.stop()
        self._apply_progress_style()

    def _tick_progress_shimmer(self) -> None:
        self._progress_shimmer_phase += 0.06
        if self._progress_shimmer_phase > 1.0:
            self._progress_shimmer_phase = 0.0
        self._apply_progress_style()

    def _apply_progress_style(self) -> None:
        if self._progress_visual_mode == "success":
            chunk = "#238636"
        elif self._progress_visual_mode == "error":
            chunk = "#d73a49"
        elif self._progress_visual_mode == "active":
            c = self._progress_shimmer_phase
            left = max(0.0, c - 0.18)
            right = min(1.0, c + 0.18)
            chunk = (
                "qlineargradient(x1:0, y1:0, x2:1, y2:0, "
                f"stop:0 #1f6feb, stop:{left:.2f} #2f81f7, "
                f"stop:{c:.2f} #a8d1ff, stop:{right:.2f} #2f81f7, stop:1 #1f6feb)"
            )
        else:
            chunk = "#58A6FF"

        self.progress_bar.setStyleSheet(
            "QProgressBar {"
            " background: rgba(13, 17, 23, 245);"
            " border: 1px solid #30363d;"
            " border-radius: 12px;"
            " min-height: 22px;"
            " text-align: center;"
            " color: #dbeafe;"
            "}"
            f"QProgressBar::chunk {{ border-radius: 10px; background: {chunk}; }}"
        )

    def _apply_firmware_badges(self, item: Optional[Dict[str, Any]]) -> None:
        if not item:
            self.version_badge.setText("Versiyon: -")
            self.size_badge.setText("Boyut: -")
            self.type_badge.setText("Tip: -")
            for badge in (self.version_badge, self.size_badge, self.type_badge):
                badge.setProperty("badgeState", "idle")
                self._refresh_dynamic_style(badge)
            return

        version = item.get("version")
        version_text = f"v{version}" if version is not None else "?"
        size_raw = item.get("size", "?")
        try:
            size_text = f"{int(size_raw):,} byte"
        except Exception:
            size_text = "Bilinmiyor"

        self.version_badge.setText(f"Versiyon: {version_text}")
        self.size_badge.setText(f"Boyut: {size_text}")
        self.type_badge.setText(f"Tip: {item.get('type', '?')}")
        for badge in (self.version_badge, self.size_badge, self.type_badge):
            badge.setProperty("badgeState", "active")
            self._refresh_dynamic_style(badge)

    def _configure_admin_styles(self) -> None:
        if self.admin_window is None:
            return

        default_buttons = [
            self.close_admin_button,
            self.add_device_button,
            self.edit_device_button,
            self.save_all_button,
            self.change_password_button,
            self.update_stm32_key_button,
            self.toggle_aes_button,
            self.proxy_health_button,
            self.proxy_stop_button,
        ]
        danger_buttons = [self.delete_device_button, self.reset_config_button]
        primary_buttons = [self.add_device_button, self.edit_device_button, self.save_all_button, self.proxy_start_button]

        for btn in default_buttons:
            if btn is None:
                continue
            btn.setProperty("role", "default")
            btn.setCursor(Qt.PointingHandCursor)

        for btn in primary_buttons:
            if btn is not None:
                btn.setProperty("role", "primary")

        for btn in danger_buttons:
            if btn is not None:
                btn.setProperty("role", "danger")
                btn.setCursor(Qt.PointingHandCursor)

        self.admin_window.setStyleSheet(
            self._shared_theme_stylesheet()
            + """
            QLabel#adminHeaderLabel {
                font-size: 23px;
                font-weight: 700;
                color: #f5fbff;
            }
            QLabel#adminInfoLabel, QLabel#securityHintLabel, QLabel#deviceCountLabel, QLabel#proxyRuntimeHintLabel {
                color: #9bb0c9;
            }
            QTabWidget::pane {
                border: 1px solid #30363d;
                border-radius: 13px;
                background: rgba(22, 27, 34, 220);
                padding: 8px;
            }
            QTabBar::tab {
                background: rgba(22, 27, 34, 240);
                border: 1px solid #30363d;
                border-bottom: none;
                color: #88a2c1;
                border-top-left-radius: 9px;
                border-top-right-radius: 9px;
                padding: 9px 16px;
                margin-right: 5px;
                min-width: 120px;
            }
            QTabBar::tab:selected {
                color: #f3f9ff;
                border: 1px solid #58A6FF;
                background: qlineargradient(
                    x1:0, y1:0, x2:1, y2:0,
                    stop:0 #2f81f7, stop:1 #58A6FF
                );
            }
            QTabBar::tab:hover:!selected {
                color: #b9d1eb;
                border: 1px solid #4a596d;
            }
            QListWidget::item {
                padding: 7px;
                border-radius: 7px;
                margin: 2px 0;
            }
            QListWidget::item:selected {
                color: #f8fcff;
                background: qlineargradient(
                    x1:0, y1:0, x2:1, y2:0,
                    stop:0 rgba(47, 129, 247, 210),
                    stop:1 rgba(88, 166, 255, 190)
                );
            }
            QLabel#metaNameTitle, QLabel#metaFwTitle, QLabel#metaLastTitle, QLabel#metaDriveTitle, QLabel#metaAesTitle {
                color: #8fa6c2;
                font-weight: 600;
            }
            QLabel#metaNameValue, QLabel#metaFwValue, QLabel#metaLastValue, QLabel#metaDriveValue, QLabel#metaAesValue {
                color: #f0f6fc;
                background: rgba(13, 17, 23, 170);
                border: 1px solid #30363d;
                border-radius: 8px;
                padding: 4px 8px;
            }
            QLabel#proxyRuntimeStatusLabel {
                color: #f0f6fc;
                background: rgba(13, 17, 23, 170);
                border: 1px solid #30363d;
                border-radius: 8px;
                padding: 8px 10px;
            }
            QPushButton#toggleAesButton {
                min-width: 54px;
                max-width: 54px;
                padding: 4px;
            }
            """
        )

    def _set_admin_status(self, text: str) -> None:
        if self.admin_status_label is not None:
            self.admin_status_label.setText(text)

    def show(self) -> None:
        self.window.show()
        if self._success_overlay is not None:
            central = self.window.centralWidget()
            if central is not None:
                self._success_overlay.setGeometry(central.rect())
        self._play_window_intro_animation()

    def _set_status(self, text: str) -> None:
        self.status_label.setText(text)

    def _update_clock(self) -> None:
        now = QDateTime.currentDateTime()
        self.clock_label.setText(now.toString("dd.MM.yyyy  HH:mm:ss"))

    def _play_window_intro_animation(self) -> None:
        central = self.window.centralWidget()
        if central is None:
            return

        opacity = QGraphicsOpacityEffect(central)
        central.setGraphicsEffect(opacity)

        animation = QPropertyAnimation(opacity, b"opacity", self.window)
        animation.setDuration(340)
        animation.setStartValue(0.0)
        animation.setEndValue(1.0)
        animation.setEasingCurve(QEasingCurve.OutCubic)
        animation.finished.connect(lambda: central.setGraphicsEffect(None))
        animation.start()
        self._window_fade_animation = animation

    def _play_admin_intro_animation(self) -> None:
        if self.admin_window is None:
            return
        central = self.admin_window.centralWidget()
        if central is None:
            return

        opacity = QGraphicsOpacityEffect(central)
        central.setGraphicsEffect(opacity)

        animation = QPropertyAnimation(opacity, b"opacity", self.admin_window)
        animation.setDuration(260)
        animation.setStartValue(0.0)
        animation.setEndValue(1.0)
        animation.setEasingCurve(QEasingCurve.OutCubic)
        animation.finished.connect(lambda: central.setGraphicsEffect(None))
        animation.start()
        self._admin_window_fade_animation = animation

    def _set_progress_busy(self, enabled: bool) -> None:
        if enabled:
            self._progress_busy_refs += 1
        else:
            self._progress_busy_refs = max(0, self._progress_busy_refs - 1)

        if self._progress_busy_refs > 0:
            self.progress_bar.setRange(0, 0)
            self.progress_label.setText("Bekleniyor...")
            self._set_progress_visual_mode("active")
        else:
            self.progress_bar.setRange(0, 100)
            if self.progress_bar.value() < 100:
                self._set_progress_visual_mode("idle")
            if self.progress_bar.value() <= 0:
                self.progress_label.setText("0%")

    def _set_upload_button_pulse(self, enabled: bool) -> None:
        if enabled:
            self._upload_button_pulse.start()
        else:
            self._upload_button_pulse.stop()
            self._upload_button_opacity.setOpacity(1.0)

    def _start_status_animation(self, base_text: str) -> None:
        self._status_anim_base = base_text
        self._status_anim_step = 0
        self._tick_status_animation()
        if not self._status_anim_timer.isActive():
            self._status_anim_timer.start()

    def _stop_status_animation(self, final_text: Optional[str] = None) -> None:
        self._status_anim_timer.stop()
        if final_text is not None:
            self.status_label.setText(final_text)

    def _tick_status_animation(self) -> None:
        suffix = self._loading_frames[self._status_anim_step % len(self._loading_frames)]
        self._status_anim_step += 1
        self.status_label.setText(f"{self._status_anim_base}{suffix}")

    def _start_update_animation(self, base_text: str) -> None:
        self._update_anim_base = base_text
        self._update_anim_step = 0
        self._tick_update_animation()
        if not self._update_anim_timer.isActive():
            self._update_anim_timer.start()

    def _stop_update_animation(self, final_text: Optional[str] = None) -> None:
        self._update_anim_timer.stop()
        if final_text is not None:
            self.update_status_label.setText(final_text)

    def _tick_update_animation(self) -> None:
        suffix = self._loading_frames[self._update_anim_step % len(self._loading_frames)]
        self._update_anim_step += 1
        self.update_status_label.setText(f"{self._update_anim_base}{suffix}")

    # ─────────────────────────────────────────────────────────────
    # Cihaz Logu (Device Monitor)
    # ─────────────────────────────────────────────────────────────

    def _open_device_monitor_dialog(self) -> None:
        if self._monitor_dialog is None:
            self._build_monitor_dialog()
        self._monitor_dialog.show()
        self._monitor_dialog.raise_()
        self._monitor_dialog.activateWindow()

    def _build_monitor_dialog(self) -> None:
        dlg = QDialog(self.window)
        dlg.setWindowTitle("Cihaz Logu — Gonderici STM32")
        dlg.resize(700, 440)
        dlg.setStyleSheet(self._shared_theme_stylesheet())

        layout = QVBoxLayout(dlg)
        layout.setSpacing(8)
        layout.setContentsMargins(12, 12, 12, 12)

        # Üst satır: durum + butonlar
        top_row = QHBoxLayout()
        status_label = QLabel("Bagli degil")
        status_label.setStyleSheet("color: #8b949e; font-size: 9pt;")
        top_row.addWidget(status_label)
        top_row.addStretch()

        connect_btn = QPushButton("Baglat")
        connect_btn.setObjectName("monitorConnectBtn")
        connect_btn.setFixedWidth(110)
        clear_btn = QPushButton("Temizle")
        clear_btn.setFixedWidth(80)
        top_row.addWidget(connect_btn)
        top_row.addWidget(clear_btn)
        layout.addLayout(top_row)

        # Log alanı
        log_widget = QPlainTextEdit()
        log_widget.setReadOnly(True)
        log_widget.setPlaceholderText(
            "Cihaz logu burada gorunecek...\n\n"
            "[E:XX]  Hata mesajlari  (kirmizi)\n"
            "[W:XX]  Uyari mesajlari (sari)\n"
            "[I:XX]  Bilgi mesajlari (mavi)\n"
            "Diger   Normal log      (gri)"
        )
        log_widget.setLineWrapMode(QPlainTextEdit.WidgetWidth)
        log_widget.setStyleSheet(
            "QPlainTextEdit { font-family: 'Consolas', 'Courier New', monospace;"
            " font-size: 9pt; }"
        )
        layout.addWidget(log_widget)

        self._monitor_dialog = dlg
        self._monitor_log_widget = log_widget
        self._monitor_status_label = status_label
        self._monitor_connect_btn = connect_btn

        def _toggle_connect() -> None:
            port = self.port_combo.currentText()
            if not port or "bulunamadi" in port.lower():
                QMessageBox.warning(dlg, "Uyari", "Gecerli bir COM port secin.")
                return
            if self.device_monitor and self.device_monitor.is_running:
                self.device_monitor.stop()
                connect_btn.setText("Baglat")
                status_label.setText("Baglanti kesildi.")
                status_label.setStyleSheet("color: #8b949e; font-size: 9pt;")
            else:
                baud = self.config.get("baud_rate", 115200)
                self.device_monitor = DeviceMonitor(
                    port=port,
                    baud=baud,
                    on_message=lambda level, code, msg, ts: self.signals.device_log.emit(
                        level, code, msg, ts
                    ),
                )
                self.device_monitor.start()
                connect_btn.setText("Baglantiyi Kes")
                status_label.setText(f"Baglaniyor: {port}...")
                status_label.setStyleSheet("color: #d29922; font-size: 9pt;")

        connect_btn.clicked.connect(_toggle_connect)
        clear_btn.clicked.connect(log_widget.clear)

        def _on_dialog_close(_result: int) -> None:
            if self.device_monitor and self.device_monitor.is_running:
                self.device_monitor.stop()

        dlg.finished.connect(_on_dialog_close)

    def _on_device_log_message(
        self, level: str, code: int, msg: str, timestamp: str
    ) -> None:
        if self._monitor_log_widget is None:
            return

        # Seviyeye göre renk + etiket
        if level == "E":
            color = "#f85149"
            label = "HATA"
        elif level == "W":
            color = "#d29922"
            label = "UYARI"
        elif level == "I":
            color = "#58a6ff"
            label = "BİLGİ"
        else:
            color = "#8b949e"
            label = "LOG"

        code_str = f" [0x{code:02X}]" if code != 0 else ""
        safe_msg = msg.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        html = (
            f'<span style="color:#6e7681;">[{timestamp}]</span> '
            f'<span style="color:{color};font-weight:bold;">{label}{code_str}</span> '
            f'<span style="color:#e6edf3;">{safe_msg}</span>'
        )
        self._monitor_log_widget.appendHtml(html)
        self._monitor_log_widget.verticalScrollBar().setValue(
            self._monitor_log_widget.verticalScrollBar().maximum()
        )

        # Status label ve connect butonu güncelle
        if self._monitor_status_label:
            if level == "E":
                self._monitor_status_label.setText(f"Son hata: {msg[:55]}")
                self._monitor_status_label.setStyleSheet("color: #f85149; font-size: 9pt;")
            elif level == "I" and "baglandi" in msg.lower():
                self._monitor_status_label.setText("Bagli — mesajlar izleniyor")
                self._monitor_status_label.setStyleSheet("color: #3fb950; font-size: 9pt;")
                if self._monitor_connect_btn:
                    self._monitor_connect_btn.setText("Baglantiyi Kes")
            elif "durduruldu" in msg.lower() or "kesildi" in msg.lower():
                self._monitor_status_label.setText("Bagli degil")
                self._monitor_status_label.setStyleSheet("color: #8b949e; font-size: 9pt;")
                if self._monitor_connect_btn:
                    self._monitor_connect_btn.setText("Baglat")

    def _append_log(self, message: str) -> None:
        stamp = QDateTime.currentDateTime().toString("HH:mm:ss")
        self.log_text.appendPlainText(f"[{stamp}] {message}")
        self.log_text.verticalScrollBar().setValue(self.log_text.verticalScrollBar().maximum())

    def _reload_download_clients(self) -> None:
        backend = str(self.config.get("proxy_backend", "")).strip()
        self.proxy_client = FirmwareProxyClient.from_backend_spec(backend)

        service_json = str(self.config.get("service_account_json", "")).strip()
        self.drive_manager = DriveManager(service_account_json=service_json) if service_json else None

    def _try_load_config(self) -> None:
        if config_exists():
            try:
                self.config = load_config("admin")
                self._append_log("Config yuklendi (varsayilan sifre ile).")
            except ValueError:
                self._append_log("Config sifreli. Admin girisi yapmadan admin paneli acilmaz.")
        else:
            self._append_log("Config dosyasi yok. Admin panelinden cihaz ekleyin.")
        self._reload_download_clients()
        self._refresh_device_list()

    def _refresh_device_list(self) -> None:
        devices = self.config.get("devices", [])
        self.device_combo.blockSignals(True)
        self.device_combo.clear()
        if devices:
            self.device_combo.addItems([d.get("name", "Isimsiz") for d in devices])
            self.device_combo.setCurrentIndex(0)
        else:
            self.device_combo.addItem("Cihaz yok - Admin panelinden ekleyin")
            self.device_combo.setCurrentIndex(0)
        self.device_combo.blockSignals(False)
        self._on_device_selected()

    def _on_device_selected(self) -> None:
        self._stop_update_animation("Henuz kontrol edilmedi")
        self.firmware_combo.clear()
        self.firmware_info_label.setText("")
        self.available_files = []
        self._apply_firmware_badges(None)

    def _get_selected_device(self) -> Optional[Dict[str, Any]]:
        name = self.device_combo.currentText()
        for device in self.config.get("devices", []):
            if device.get("name") == name:
                return device
        return None

    def _device_channel(self, device: Optional[Dict[str, Any]]) -> str:
        if not device:
            return ""
        return str(device.get("channel", "") or device.get("drive_file_id", "")).strip()

    def _proxy_backend_value(self) -> str:
        return str(self.config.get("proxy_backend", "")).strip()

    def _proxy_service_account_value(self) -> str:
        return str(self.config.get("service_account_json", "")).strip()

    def _default_proxy_channel_map_path(self) -> str:
        return resource_path("proxy_channels.json")

    def _proxy_channel_map_value(self) -> str:
        value = str(self.config.get("proxy_channel_map_file", "")).strip()
        return value or self._default_proxy_channel_map_path()

    def _proxy_backend_form_value(self) -> str:
        if self.service_json_edit is not None:
            return self.service_json_edit.text().strip()
        return self._proxy_backend_value()

    def _proxy_service_account_form_value(self) -> str:
        if self.proxy_service_account_edit is not None:
            return self.proxy_service_account_edit.text().strip()
        return self._proxy_service_account_value()

    def _proxy_channel_map_form_value(self) -> str:
        if self.proxy_channel_map_edit is not None:
            value = self.proxy_channel_map_edit.text().strip()
            if value:
                return value
        return self._proxy_channel_map_value()

    def _set_proxy_runtime_status(self, text: str) -> None:
        if self.proxy_runtime_status_label is not None:
            self.proxy_runtime_status_label.setText(text)

    def _sync_proxy_runtime_refs(self) -> None:
        if self.proxy_server_thread is not None and not self.proxy_server_thread.is_alive():
            self.proxy_server = None
            self.proxy_server_thread = None
            self._proxy_runtime_config = None

    def _is_embedded_proxy_running(self) -> bool:
        self._sync_proxy_runtime_refs()
        return self.proxy_server is not None and self.proxy_server_thread is not None

    def _is_local_proxy_host(self, host: str) -> bool:
        return str(host or "").strip().lower() in {"127.0.0.1", "localhost", "::1"}

    def _apply_proxy_form_values_to_runtime(self) -> None:
        self.config["proxy_backend"] = self._proxy_backend_form_value()
        self.config["service_account_json"] = self._proxy_service_account_form_value()
        raw_channel_map = ""
        if self.proxy_channel_map_edit is not None:
            raw_channel_map = self.proxy_channel_map_edit.text().strip()
        self.config["proxy_channel_map_file"] = raw_channel_map
        self._reload_download_clients()

    def _collect_local_proxy_settings(self, use_form_values: bool) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
        backend_value = self._proxy_backend_form_value() if use_form_values else self._proxy_backend_value()
        client = FirmwareProxyClient.from_backend_spec(backend_value, timeout=2)
        if client.config_error:
            return None, client.config_error

        parsed = urllib.parse.urlparse(client.base_url)
        scheme = str(parsed.scheme or "").lower()
        host = str(parsed.hostname or "").strip()
        is_local = self._is_local_proxy_host(host)
        port = parsed.port or (8787 if is_local else 80)
        path = str(parsed.path or "").strip()

        if scheme != "http":
            return None, "Yerel proxy icin backend adresi http:// ile baslamali."
        if not host:
            return None, "Proxy host okunamadi."
        if not is_local:
            return None, "GUI sadece localhost veya 127.0.0.1 adresindeki yerel proxyyi baslatabilir."
        if path not in ("", "/"):
            return None, "Yerel proxy backend adresi ek yol icermemeli."

        service_json = self._proxy_service_account_form_value() if use_form_values else self._proxy_service_account_value()
        if not service_json:
            return None, "Service Account JSON yolu bos."
        if not os.path.exists(service_json):
            return None, f"Service Account JSON bulunamadi: {service_json}"

        channel_map_file = self._proxy_channel_map_form_value() if use_form_values else self._proxy_channel_map_value()
        if not channel_map_file:
            return None, "Channel map JSON yolu bos."
        if not os.path.exists(channel_map_file):
            return None, f"Channel map JSON bulunamadi: {channel_map_file}"

        return {
            "base_url": client.base_url,
            "api_key": client.api_key,
            "host": host,
            "port": int(port),
            "service_json": service_json,
            "channel_map_file": channel_map_file,
            "token_ttl": 120,
        }, None

    def _start_local_proxy_server(self, settings: Dict[str, Any], announce: bool = True) -> Optional[str]:
        self._sync_proxy_runtime_refs()
        if self._is_embedded_proxy_running():
            return None

        try:
            server = create_proxy_server(
                host=str(settings.get("host", "")).strip(),
                port=int(settings.get("port", 8787)),
                api_key=str(settings.get("api_key", "")).strip(),
                service_json=str(settings.get("service_json", "")).strip(),
                channel_map_file=str(settings.get("channel_map_file", "")).strip(),
                token_ttl=int(settings.get("token_ttl", 120)),
            )
        except Exception as exc:
            return f"Yerel proxy baslatilamadi: {exc}"

        def worker() -> None:
            try:
                server.serve_forever()
            except Exception as exc:
                self.signals.log.emit(f"Yerel proxy durdu: {exc}")
            finally:
                try:
                    server.server_close()
                except Exception:
                    pass

        self.proxy_server = server
        self.proxy_server_thread = threading.Thread(target=worker, name="local-proxy-server", daemon=True)
        self._proxy_runtime_config = dict(settings)
        self.proxy_server_thread.start()
        self._reload_download_clients()

        if announce:
            self._append_log(f"Yerel proxy baslatildi: {settings.get('base_url', '')}")
            self._set_admin_status("Yerel proxy baslatildi.")

        self._refresh_proxy_runtime_status()
        return None

    def _stop_local_proxy_server(self, announce: bool = True) -> Optional[str]:
        self._sync_proxy_runtime_refs()
        if self.proxy_server is None:
            return "GUI icinden baslatilmis yerel proxy bulunamadi."

        try:
            self.proxy_server.shutdown()
            if self.proxy_server_thread is not None:
                self.proxy_server_thread.join(timeout=2.5)
                if self.proxy_server_thread.is_alive():
                    return "Yerel proxy durdurulurken zaman asimi olustu."
        except Exception as exc:
            return f"Yerel proxy durdurulamadi: {exc}"

        self.proxy_server = None
        self.proxy_server_thread = None
        self._proxy_runtime_config = None

        self._reload_download_clients()
        if announce:
            self._append_log("Yerel proxy durduruldu.")
            self._set_admin_status("Yerel proxy durduruldu.")
        self._refresh_proxy_runtime_status()
        return None

    def _refresh_proxy_runtime_status(self) -> None:
        self._sync_proxy_runtime_refs()
        backend_value = self._proxy_backend_value()
        client = FirmwareProxyClient.from_backend_spec(backend_value, timeout=1)

        start_enabled = False
        stop_enabled = self._is_embedded_proxy_running()
        test_enabled = bool(backend_value)
        status_text = "Proxy backend tanimli degil."

        if backend_value and not client.config_error:
            parsed = urllib.parse.urlparse(client.base_url)
            host = str(parsed.hostname or "").strip()
            is_local = self._is_local_proxy_host(host)
            start_enabled = is_local and not stop_enabled
            payload, error = client.health()
            _ = payload
            if stop_enabled:
                runtime_url = client.base_url
                if isinstance(self._proxy_runtime_config, dict):
                    runtime_url = str(self._proxy_runtime_config.get("base_url", client.base_url))
                status_text = f"Yerel proxy GUI icinde calisiyor: {runtime_url}"
            elif error is None:
                prefix = "Yerel proxy erisilebilir" if is_local else "Harici proxy erisilebilir"
                status_text = f"{prefix}: {client.base_url}"
            else:
                prefix = "Yerel proxy ulasilamiyor" if is_local else "Harici proxy ulasilamiyor"
                status_text = f"{prefix}: {client.base_url}"
        elif backend_value and client.config_error:
            status_text = client.config_error

        if self.proxy_start_button is not None:
            self.proxy_start_button.setEnabled(start_enabled)
        if self.proxy_stop_button is not None:
            self.proxy_stop_button.setEnabled(stop_enabled)
        if self.proxy_health_button is not None:
            self.proxy_health_button.setEnabled(test_enabled)
        self._set_proxy_runtime_status(status_text)

    def _start_local_proxy_from_admin(self) -> None:
        self._apply_proxy_form_values_to_runtime()
        client = FirmwareProxyClient.from_backend_spec(self._proxy_backend_value(), timeout=1)
        if client.is_ready():
            _, health_error = client.health()
            if health_error is None and not self._is_embedded_proxy_running():
                self._refresh_proxy_runtime_status()
                self._set_admin_status("Proxy zaten erisilebilir.")
                self._append_log(f"Proxy zaten erisilebilir: {client.base_url}")
                return

        settings, error = self._collect_local_proxy_settings(use_form_values=False)
        if error:
            self._refresh_proxy_runtime_status()
            QMessageBox.warning(self._dialog_parent(), "Uyari", error)
            return

        start_error = self._start_local_proxy_server(settings, announce=True)
        if start_error:
            self._refresh_proxy_runtime_status()
            QMessageBox.critical(self._dialog_parent(), "Hata", start_error)
            return

    def _stop_local_proxy_from_admin(self) -> None:
        stop_error = self._stop_local_proxy_server(announce=True)
        if stop_error:
            self._refresh_proxy_runtime_status()
            QMessageBox.warning(self._dialog_parent(), "Uyari", stop_error)

    def _test_proxy_connection(self) -> None:
        self._apply_proxy_form_values_to_runtime()
        client = FirmwareProxyClient.from_backend_spec(self._proxy_backend_value(), timeout=2)
        if client.config_error:
            self._refresh_proxy_runtime_status()
            QMessageBox.warning(self._dialog_parent(), "Uyari", client.config_error)
            return

        payload, error = client.health()
        if error:
            self._refresh_proxy_runtime_status()
            self._append_log(error)
            QMessageBox.warning(self._dialog_parent(), "Proxy Testi", error)
            return

        ts = "-"
        if isinstance(payload, dict):
            ts = str(payload.get("ts", "-"))
        self._refresh_proxy_runtime_status()
        self._append_log(f"Proxy testi basarili: {client.base_url} (ts={ts})")
        self._set_admin_status("Proxy baglanti testi basarili.")
        QMessageBox.information(self._dialog_parent(), "Proxy Testi", f"Proxy erisilebilir:\n{client.base_url}")

    def _ensure_local_proxy_available(self) -> Optional[str]:
        backend_value = self._proxy_backend_value()
        if not backend_value:
            return None

        client = FirmwareProxyClient.from_backend_spec(backend_value, timeout=1)
        if client.config_error:
            return client.config_error

        parsed = urllib.parse.urlparse(client.base_url)
        if not self._is_local_proxy_host(parsed.hostname or ""):
            return None

        _, health_error = client.health()
        if health_error is None:
            return None

        settings, settings_error = self._collect_local_proxy_settings(use_form_values=False)
        if settings_error:
            return f"{health_error} Yerel proxy otomatik baslatilamadi: {settings_error}"

        start_error = self._start_local_proxy_server(settings, announce=False)
        if start_error:
            return f"{health_error} Yerel proxy otomatik baslatilamadi: {start_error}"

        self._append_log(f"Yerel proxy otomatik baslatildi: {settings.get('base_url', '')}")
        self._set_admin_status("Yerel proxy otomatik baslatildi.")
        return None

    def _scan_ports(self) -> None:
        if self._upload_running:
            return
        if serial_list_ports is None:
            self.port_combo.clear()
            self.port_combo.addItem("pyserial kurulu degil")
            self._append_log("COM port taramasi atlandi: pyserial bulunamadi.")
            return

        ports = [port.device for port in serial_list_ports.comports()]
        self.port_combo.clear()
        if ports:
            self.port_combo.addItems(ports)
            cfg_port = self.config.get("serial_port", "")
            if cfg_port in ports:
                self.port_combo.setCurrentText(cfg_port)
        else:
            self.port_combo.addItem("Port bulunamadi")
        self.baud_value_label.setText(str(self.config.get("baud_rate", 115200)))
        self._append_log("COM portlar tarandi: " + (", ".join(ports) if ports else "yok"))

    def _scan_folder(self) -> None:
        if self._upload_running:
            return
        if not self.connection_mode:
            self._set_step(0)
            self._set_mode_validation("Baglanti modu secmeden tarama yapamazsiniz.")
            self._shake_widget(self.mode_cards_container)
            return

        if self.current_step != 2:
            self._set_step(2)

        device = self._get_selected_device()
        if not device:
            QMessageBox.warning(self.window, "Uyari", "Once bir cihaz secin.")
            self._shake_widget(self.device_combo)
            return

        channel = self._device_channel(device)
        use_proxy = bool(self._proxy_backend_value())
        if not channel:
            self._stop_update_animation("Kanal bilgisi tanimli degil")
            self._shake_widget(self.device_combo)
            return

        self._start_update_animation("Klasor taraniyor")
        self._start_status_animation("Sunucu yaniti bekleniyor")
        self._set_progress_busy(True)
        self.scan_folder_button.setEnabled(False)
        self.available_files = []

        if use_proxy:
            proxy_error = self._ensure_local_proxy_available()
            if proxy_error:
                self._set_progress_busy(False)
                self._stop_status_animation("Hazir")
                self.scan_folder_button.setEnabled(True)
                self._stop_update_animation(proxy_error)
                self._append_log(proxy_error)
                return

        def worker() -> None:
            try:
                if use_proxy:
                    if self.proxy_client is None:
                        self._reload_download_clients()
                    if self.proxy_client is None:
                        self.signals.scan_finished.emit(None, "Proxy istemcisi baslatilamadi.")
                        return
                    files, error = self.proxy_client.list_channel_files(channel)
                else:
                    folder_id = str(device.get("drive_file_id", "")).strip()
                    if not folder_id:
                        self.signals.scan_finished.emit(None, "Drive klasor ID tanimli degil")
                        return
                    if self.drive_manager is None:
                        self._reload_download_clients()
                    if self.drive_manager is None:
                        self.signals.scan_finished.emit(None, "Drive Manager baslatilamadi.")
                        return
                    files, error = self.drive_manager.list_all_files_in_folder(folder_id)
                self.signals.scan_finished.emit(files, error)
            except Exception as exc:
                self.signals.scan_finished.emit(None, f"Beklenmeyen hata: {exc}")

        threading.Thread(target=worker, daemon=True).start()

    def _on_scan_finished(self, files: Any, error: Any) -> None:
        self._set_progress_busy(False)
        self._stop_status_animation("Hazir")
        self.scan_folder_button.setEnabled(True)
        if not files:
            self.available_files = []
            self.firmware_combo.clear()
            self.firmware_info_label.setText("")
            self._apply_firmware_badges(None)
            self._stop_update_animation(str(error or "Dosya bulunamadi"))
            if error:
                self._append_log(str(error))
            return

        self.available_files = list(files)
        display_names: List[str] = []
        for item in self.available_files:
            ver = item.get("version")
            ver_str = f"v{ver}" if ver is not None else "v?"
            display_names.append(f"{item.get('name', 'dosya')}  [{ver_str} | {item.get('type', '?')}]")

        self.firmware_combo.blockSignals(True)
        self.firmware_combo.clear()
        self.firmware_combo.addItems(display_names)
        self.firmware_combo.setCurrentIndex(0)
        self.firmware_combo.blockSignals(False)
        self._on_firmware_selected()

        msg = f"{len(self.available_files)} dosya bulundu"
        if error:
            msg += f" ({error})"
        self._stop_update_animation(msg)
        self._append_log("Klasor tarandi: " + msg)

    def _on_firmware_selected(self, _row: int = -1) -> None:
        idx = self.firmware_combo.currentIndex()
        if idx < 0 or idx >= len(self.available_files):
            self.firmware_info_label.setText("")
            self._apply_firmware_badges(None)
            return

        item = self.available_files[idx]
        self._apply_firmware_badges(item)
        ver = item.get("version")
        ver_str = f"v{ver}" if ver is not None else "bilinmiyor"
        size_raw = item.get("size", "?")
        try:
            size_str = f"{int(size_raw):,} byte"
        except Exception:
            size_str = "boyut bilinmiyor"

        device = self._get_selected_device()
        installed = device.get("last_installed_version", 0) if device else 0
        info = f"Versiyon: {ver_str} | Boyut: {size_str} | Tur: {item.get('type', '?')} | Yuklu: v{installed}"
        self.firmware_info_label.setText(info)

    def _start_or_stop_upload(self) -> None:
        if self.upload_thread and self.upload_thread.is_alive():
            self.stop_requested = True
            self._start_status_animation("Durdurma istegi gonderildi")
            self._append_log("Durdurma istegi gonderildi.")
            return
        self._start_upload()

    def _start_upload(self) -> None:
        if not self.connection_mode:
            self._set_step(0)
            self._set_mode_validation("Baglanti modu secmeden guncelleme baslatilamaz.")
            self._shake_widget(self.mode_cards_container)
            return

        if self.current_step != 2:
            self._set_step(2)

        device = self._get_selected_device()
        if not device:
            QMessageBox.warning(self.window, "Uyari", "Once bir cihaz secin.")
            self._shake_widget(self.device_combo)
            return

        aes_key = device.get("aes_key_hex", "")
        if not aes_key:
            QMessageBox.warning(self.window, "Uyari", "Secili cihazin AES key alani bos.")
            self._shake_widget(self.device_combo)
            return

        idx = self.firmware_combo.currentIndex()
        if idx < 0 or idx >= len(self.available_files):
            QMessageBox.warning(self.window, "Uyari", "Once firmware listesi taranip bir dosya secilmeli.")
            self._shake_widget(self.firmware_combo)
            return

        file_item = self.available_files[idx]
        target_download_token = str(file_item.get("token", "")).strip()
        target_file_id = file_item.get("id", "")
        file_type = file_item.get("type", "BIN")
        self.pending_firmware_version = file_item.get("version")

        if not target_download_token and not target_file_id:
            QMessageBox.warning(self.window, "Uyari", "Secilen dosyanin indirme bilgisi yok.")
            self._shake_widget(self.firmware_combo)
            return

        serial_port = self.port_combo.currentText()
        if not serial_port or "bulunamadi" in serial_port.lower():
            QMessageBox.warning(self.window, "Uyari", "Gecerli bir COM port secin.")
            self._shake_widget(self.port_combo)
            return

        upload_cfg = {
            "serial_port": serial_port,
            "baud_rate": self.config.get("baud_rate", 115200),
            "download_token": target_download_token,
            "drive_file_id": target_file_id,
            "aes_key_hex": aes_key,
            "packet_size": self.config.get("packet_size", 128),
            "max_retries": self.config.get("max_retries", 7),
            "firmware_version": device.get("firmware_version", 1),
            "file_type": file_type,
            "filename": file_item.get("name", ""),
            "is_rf": self.rf_mode_check.isChecked(),
        }

        if target_download_token:
            proxy_error = self._ensure_local_proxy_available()
            if proxy_error:
                self._append_log(proxy_error)
                QMessageBox.warning(self._dialog_parent(), "Uyari", proxy_error)
                return

        self._set_completion_actions_visible(False)
        self._hide_success_overlay(animate=False)
        self._upload_running = True
        self._set_selection_controls_enabled(False, animated=False)
        self.stop_requested = False
        self._set_progress_busy(False)
        self.progress_bar.setValue(0)
        self.progress_label.setText("0%")
        self._set_progress_visual_mode("active")
        self.start_upload_button.setText("Durdur")
        self._set_upload_button_pulse(True)
        self._start_status_animation("Guncelleme devam ediyor")
        self._append_log(f"Secilen firmware: {file_item.get('name', '')} [{file_type}]")

        def worker() -> None:
            if target_download_token:
                if self.proxy_client is None:
                    self._reload_download_clients()
                download_client = self.proxy_client
            else:
                if self.drive_manager is None:
                    self._reload_download_clients()
                download_client = self.drive_manager
            success = upload_firmware(
                config=upload_cfg,
                log=lambda msg: self.signals.log.emit(str(msg)),
                on_progress=lambda cur, total: self.signals.progress.emit(int(cur), int(total)),
                stop_flag=lambda: self.stop_requested,
                download_client=download_client,
            )
            self.signals.upload_finished.emit(bool(success))

        # Upload sirasinda monitor'u durdur — port paylasim cakismasi onle
        if self.device_monitor and self.device_monitor.is_running:
            self._monitor_was_running = True
            self._monitor_port = self.port_combo.currentText()
            self.device_monitor.stop()
        else:
            self._monitor_was_running = False

        self.upload_thread = threading.Thread(target=worker, daemon=True)
        self.upload_thread.start()

    def _on_worker_progress(self, current: int, total: int) -> None:
        if total <= 0:
            return
        self._set_progress_visual_mode("active")
        pct = int((current * 100) / total)
        self.progress_bar.setValue(max(0, min(100, pct)))
        self.progress_label.setText(f"{pct}% ({current}/{total})")

    def _on_upload_finished(self, success: bool) -> None:
        self._upload_running = False
        self._set_selection_controls_enabled(True, animated=True)
        self.start_upload_button.setText("Guncellemeyi Baslat")
        self._set_upload_button_pulse(False)

        if success:
            self._stop_status_animation("Guncelleme basarili.")
            self.progress_bar.setValue(100)
            self.progress_label.setText("100%")
            self._set_progress_visual_mode("success")
            self._set_completion_actions_visible(
                True,
                "Guncelleme Basarili. Yeni bir islem baslatabilir veya ayarlari duzenleyebilirsiniz.",
            )
            self._show_success_overlay()
            device = self._get_selected_device()
            if device and self.pending_firmware_version is not None:
                device["last_installed_version"] = self.pending_firmware_version
                self.pending_firmware_version = None
                if self.admin_password:
                    try:
                        save_config(self.config, self.admin_password)
                        self._append_log(f"Yuklu versiyon kaydedildi: v{device['last_installed_version']}")
                    except Exception:
                        self._append_log("Versiyon kaydedilemedi. Admin panelinden tekrar kaydedin.")
        elif self.stop_requested:
            self._stop_status_animation("Islem kullanici tarafindan durduruldu.")
            self._set_progress_visual_mode("error")
            self._set_completion_actions_visible(
                True,
                "Islem durduruldu. Ayarlari degistirip yeniden baslatabilirsiniz.",
            )
        else:
            self._stop_status_animation("Guncelleme basarisiz.")
            self._set_progress_visual_mode("error")
            self._set_completion_actions_visible(
                True,
                "Guncelleme basarisiz. Yeni deneme icin ayarlari kontrol edin.",
            )
        self.pending_firmware_version = None

        # Upload bitti — monitor bagliysa yeniden basla
        if self._monitor_was_running and self._monitor_port:
            baud = self.config.get("baud_rate", 115200)
            self.device_monitor = DeviceMonitor(
                port=self._monitor_port,
                baud=baud,
                on_message=lambda level, code, msg, ts: self.signals.device_log.emit(
                    level, code, msg, ts
                ),
            )
            self.device_monitor.start()
            self._monitor_was_running = False

    def _show_admin_login_dialog(self) -> None:
        if self.admin_password:
            self._open_admin_window()
            return

        dialog = QDialog(self._dialog_parent())
        dialog.setWindowTitle("Admin Girisi")
        form = QFormLayout(dialog)

        user_edit = QLineEdit(dialog)
        pwd_edit = QLineEdit(dialog)
        pwd_edit.setEchoMode(QLineEdit.Password)

        creds = load_credentials()
        user_edit.setText(creds.get("username", "admin"))

        form.addRow("Kullanici Adi:", user_edit)
        form.addRow("Sifre:", pwd_edit)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel, parent=dialog)
        form.addRow(buttons)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)

        if dialog.exec() != QDialog.Accepted:
            return

        username = user_edit.text().strip()
        password = pwd_edit.text().strip()
        if not verify_admin(username, password):
            QMessageBox.critical(self._dialog_parent(), "Hata", "Admin bilgileri hatali.")
            return

        try:
            self.config = load_config(password)
        except ValueError as exc:
            QMessageBox.critical(self._dialog_parent(), "Hata", str(exc))
            return

        self.admin_password = password
        self._reload_download_clients()
        self._refresh_device_list()
        self._open_admin_window()
        self.admin_login_button.setText("Admin Paneli")
        self._append_log("Admin girisi basarili.")

    def _refresh_admin_panel(self) -> None:
        if self.baud_edit is None:
            return
        self._show_admin_aes = False
        if self.toggle_aes_button is not None:
            self.toggle_aes_button.setText("Goz")
        self.baud_edit.setText(str(self.config.get("baud_rate", 115200)))
        if self.retry_edit is not None:
            self.retry_edit.setText(str(self.config.get("max_retries", 7)))
        if self.packet_edit is not None:
            self.packet_edit.setText(str(self.config.get("packet_size", 128)))
        if self.default_port_edit is not None:
            self.default_port_edit.setText(str(self.config.get("serial_port", "COM7")))
        if self.service_json_edit is not None:
            self.service_json_edit.setText(str(self.config.get("proxy_backend", "")))
        if self.proxy_service_account_edit is not None:
            self.proxy_service_account_edit.setText(self._proxy_service_account_value())
        if self.proxy_channel_map_edit is not None:
            self.proxy_channel_map_edit.setText(self._proxy_channel_map_value())
        self._refresh_admin_device_list()
        self._refresh_proxy_runtime_status()

    def _refresh_admin_device_list(self) -> None:
        if self.device_list_widget is None:
            return
        self.device_list_widget.blockSignals(True)
        self.device_list_widget.clear()
        for idx, dev in enumerate(self.config.get("devices", [])):
            name = dev.get("name", "Isimsiz")
            ver = dev.get("firmware_version", "?")
            last_ver = dev.get("last_installed_version", "?")
            item = QListWidgetItem(f"{name}  |  fw v{ver}  |  yuklu v{last_ver}")
            item.setData(Qt.UserRole, idx)
            item.setToolTip(f"Kanal: {self._device_channel(dev)}")
            self.device_list_widget.addItem(item)
        self.device_list_widget.blockSignals(False)
        self._apply_admin_device_filter()

    def _selected_admin_index(self) -> int:
        if self.device_list_widget is None:
            return -1
        item = self.device_list_widget.currentItem()
        if item is None:
            return -1
        idx = item.data(Qt.UserRole)
        if isinstance(idx, int):
            return idx
        return -1

    def _apply_admin_device_filter(self) -> None:
        if self.device_list_widget is None:
            return
        query = ""
        if self.device_search_edit is not None:
            query = self.device_search_edit.text().strip().lower()

        visible_count = 0
        selected_visible = False
        for row in range(self.device_list_widget.count()):
            item = self.device_list_widget.item(row)
            idx = item.data(Qt.UserRole)
            device = None
            if isinstance(idx, int):
                devices = self.config.get("devices", [])
                if 0 <= idx < len(devices):
                    device = devices[idx]

            haystack = item.text().lower()
            if device:
                haystack += " " + self._device_channel(device).lower()

            is_visible = query in haystack if query else True
            item.setHidden(not is_visible)
            if is_visible:
                visible_count += 1
                if item.isSelected():
                    selected_visible = True

        if self.device_count_label is not None:
            total = len(self.config.get("devices", []))
            self.device_count_label.setText(f"{visible_count}/{total} cihaz")

        if not selected_visible and visible_count > 0:
            for row in range(self.device_list_widget.count()):
                item = self.device_list_widget.item(row)
                if not item.isHidden():
                    self.device_list_widget.setCurrentItem(item)
                    break

        self._update_admin_device_meta()

    def _on_admin_device_selection_changed(self, current: Any, previous: Any) -> None:
        _ = previous
        self._show_admin_aes = False
        if self.toggle_aes_button is not None:
            self.toggle_aes_button.setText("Goz")
        if current is None:
            self._update_admin_device_meta()
            return
        self._update_admin_device_meta()

    def _toggle_admin_aes_visibility(self) -> None:
        self._show_admin_aes = not self._show_admin_aes
        if self.toggle_aes_button is not None:
            self.toggle_aes_button.setText("Gizle" if self._show_admin_aes else "Goz")
        self._update_admin_device_meta()

    def _mask_aes_value(self, value: str) -> str:
        clean = value.strip()
        if not clean:
            return "-"
        if len(clean) <= 8:
            return "*" * len(clean)
        return f"{clean[:4]}{'*' * (len(clean) - 8)}{clean[-4:]}"

    def _update_admin_device_meta(self) -> None:
        if None in (self.meta_name_value, self.meta_fw_value, self.meta_last_value, self.meta_drive_value, self.meta_aes_value):
            return
        idx = self._selected_admin_index()
        devices = self.config.get("devices", [])
        if idx < 0 or idx >= len(devices):
            self.meta_name_value.setText("-")
            self.meta_fw_value.setText("-")
            self.meta_last_value.setText("-")
            self.meta_drive_value.setText("-")
            self.meta_aes_value.setText("********")
            return

        dev = devices[idx]
        key_hex = str(dev.get("aes_key_hex", "")).strip()
        aes_text = key_hex if self._show_admin_aes else self._mask_aes_value(key_hex)
        self.meta_name_value.setText(str(dev.get("name", "Isimsiz")))
        self.meta_fw_value.setText(f"v{dev.get('firmware_version', '?')}")
        self.meta_last_value.setText(f"v{dev.get('last_installed_version', '?')}")
        self.meta_drive_value.setText(self._device_channel(dev))
        self.meta_aes_value.setText(aes_text)

    def _device_dialog(self, title: str, existing: Optional[Dict[str, Any]] = None) -> Optional[Dict[str, Any]]:
        existing = existing or {}
        dialog = QDialog(self._dialog_parent())
        dialog.setWindowTitle(title)
        form = QFormLayout(dialog)

        name_edit = QLineEdit(dialog)
        drive_edit = QLineEdit(dialog)
        key_edit = QLineEdit(dialog)
        ver_edit = QLineEdit(dialog)
        last_ver_edit = QLineEdit(dialog)

        name_edit.setText(str(existing.get("name", "")))
        drive_edit.setText(self._device_channel(existing))
        key_edit.setText(str(existing.get("aes_key_hex", "")))
        ver_edit.setText(str(existing.get("firmware_version", 1)))
        last_ver_edit.setText(str(existing.get("last_installed_version", 0)))

        form.addRow("Cihaz Adi:", name_edit)
        form.addRow("Kanal Adi:", drive_edit)
        form.addRow("AES Key (64 hex):", key_edit)
        form.addRow("Firmware Versiyon:", ver_edit)
        form.addRow("Yuklu Versiyon:", last_ver_edit)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel, parent=dialog)
        form.addRow(buttons)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)

        if dialog.exec() != QDialog.Accepted:
            return None

        name = name_edit.text().strip()
        channel = drive_edit.text().strip()
        key_hex = key_edit.text().strip()

        if not name:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Cihaz adi bos olamaz.")
            return None
        if not channel:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Kanal adi bos olamaz.")
            return None
        if len(key_hex) != 64:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "AES key 64 hex karakter olmali.")
            return None
        try:
            bytes.fromhex(key_hex)
        except ValueError:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "AES key gecerli bir hex degeri degil.")
            return None

        try:
            fw_ver = int(ver_edit.text().strip())
            last_ver = int(last_ver_edit.text().strip())
        except ValueError:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Versiyon alanlari sayi olmali.")
            return None

        return {
            "name": name,
            "channel": channel,
            "aes_key_hex": key_hex,
            "firmware_version": fw_ver,
            "last_installed_version": last_ver,
        }

    def _add_device(self) -> None:
        device = self._device_dialog("Yeni Cihaz Ekle")
        if not device:
            return
        devices = self.config.setdefault("devices", [])
        devices.append(device)
        self._refresh_admin_device_list()
        self._refresh_device_list()
        self._set_admin_status("Yeni cihaz eklendi (kaydetmeyi unutmayin).")

    def _edit_selected_device(self) -> None:
        idx = self._selected_admin_index()
        devices = self.config.get("devices", [])
        if idx < 0 or idx >= len(devices):
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Duzenlemek icin bir cihaz secin.")
            return
        updated = self._device_dialog("Cihaz Duzenle", existing=devices[idx])
        if not updated:
            return
        devices[idx] = updated
        self._refresh_admin_device_list()
        self._refresh_device_list()
        self._set_admin_status("Cihaz bilgisi guncellendi (kaydetmeyi unutmayin).")

    def _delete_selected_device(self) -> None:
        idx = self._selected_admin_index()
        devices = self.config.get("devices", [])
        if idx < 0 or idx >= len(devices):
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Silmek icin bir cihaz secin.")
            return
        confirm = QMessageBox.question(
            self._dialog_parent(),
            "Onay",
            "Secili cihazi silmek istiyor musunuz?",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if confirm != QMessageBox.Yes:
            return
        del devices[idx]
        self._refresh_admin_device_list()
        self._refresh_device_list()
        self._set_admin_status("Cihaz silindi (kaydetmeyi unutmayin).")

    def _save_all_config(self) -> None:
        if not self.admin_password:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Admin girisi gerekli.")
            return
        if None in (
            self.baud_edit,
            self.retry_edit,
            self.packet_edit,
            self.default_port_edit,
            self.service_json_edit,
            self.proxy_service_account_edit,
            self.proxy_channel_map_edit,
        ):
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Admin arayuzu tam yuklenemedi.")
            return
        try:
            self.config["baud_rate"] = int(self.baud_edit.text().strip())  # type: ignore[union-attr]
            self.config["max_retries"] = int(self.retry_edit.text().strip())  # type: ignore[union-attr]
            self.config["packet_size"] = int(self.packet_edit.text().strip())  # type: ignore[union-attr]
            self.config["serial_port"] = self.default_port_edit.text().strip()  # type: ignore[union-attr]
            self.config["proxy_backend"] = self.service_json_edit.text().strip()  # type: ignore[union-attr]
            self.config["service_account_json"] = self.proxy_service_account_edit.text().strip()  # type: ignore[union-attr]
            self.config["proxy_channel_map_file"] = self.proxy_channel_map_edit.text().strip()  # type: ignore[union-attr]
        except ValueError:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Sayi alanlari gecerli olmali.")
            return

        try:
            save_config(self.config, self.admin_password)
            self._reload_download_clients()
            self._refresh_device_list()
            self._refresh_proxy_runtime_status()
            if self._is_embedded_proxy_running():
                self._append_log("Proxy ayarlari kaydedildi. Etkinlesmesi icin yerel proxyyi yeniden baslatin.")
                self._set_admin_status("Ayarlar kaydedildi. Proxy icin yeniden baslatma gerekebilir.")
            else:
                self._set_admin_status("Tum ayarlar kaydedildi.")
            QMessageBox.information(self._dialog_parent(), "Basarili", "Ayarlar kaydedildi.")
        except Exception as exc:
            QMessageBox.critical(self._dialog_parent(), "Hata", f"Kayit hatasi: {exc}")

    def _change_password_dialog(self) -> None:
        dialog = QDialog(self._dialog_parent())
        dialog.setWindowTitle("Sifre Degistir")
        form = QFormLayout(dialog)

        creds = load_credentials()
        user_edit = QLineEdit(dialog)
        old_edit = QLineEdit(dialog)
        new_edit = QLineEdit(dialog)
        new2_edit = QLineEdit(dialog)

        user_edit.setText(creds.get("username", "admin"))
        old_edit.setEchoMode(QLineEdit.Password)
        new_edit.setEchoMode(QLineEdit.Password)
        new2_edit.setEchoMode(QLineEdit.Password)

        form.addRow("Yeni Kullanici Adi:", user_edit)
        form.addRow("Mevcut Sifre:", old_edit)
        form.addRow("Yeni Sifre:", new_edit)
        form.addRow("Yeni Sifre (tekrar):", new2_edit)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel, parent=dialog)
        form.addRow(buttons)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)

        if dialog.exec() != QDialog.Accepted:
            return

        old_pwd = old_edit.text().strip()
        new_pwd = new_edit.text().strip()
        new_pwd2 = new2_edit.text().strip()
        new_user = user_edit.text().strip()

        if not verify_admin(creds.get("username", ""), old_pwd):
            QMessageBox.critical(self._dialog_parent(), "Hata", "Mevcut sifre hatali.")
            return
        if not new_user:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Kullanici adi bos olamaz.")
            return
        if not new_pwd:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Yeni sifre bos olamaz.")
            return
        if new_pwd != new_pwd2:
            QMessageBox.critical(self._dialog_parent(), "Hata", "Yeni sifreler eslesmiyor.")
            return

        try:
            change_admin_credentials(new_user, new_pwd)
            self.admin_password = new_pwd
            save_config(self.config, new_pwd)
            self._set_admin_status("Admin bilgileri guncellendi.")
            QMessageBox.information(self._dialog_parent(), "Basarili", "Admin bilgileri guncellendi.")
        except Exception as exc:
            QMessageBox.critical(self._dialog_parent(), "Hata", f"Sifre guncelleme hatasi: {exc}")

    def _reset_config_dialog(self) -> None:
        confirm = QMessageBox.question(
            self._dialog_parent(),
            "Config Sifirla",
            "Bu islem cihaz profilleri ve ayarlari siler.\nAdmin bilgileri korunur.\nDevam edilsin mi?",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if confirm != QMessageBox.Yes:
            return

        try:
            reset_config()
            self.config = DEFAULT_CONFIG.copy()
            self._reload_download_clients()
            self._refresh_device_list()
            self._refresh_admin_panel()
            self._set_admin_status("Config sifirlandi.")
            QMessageBox.information(self._dialog_parent(), "Basarili", "Config sifirlandi.")
        except Exception as exc:
            QMessageBox.critical(self._dialog_parent(), "Hata", f"Sifirlama hatasi: {exc}")

    def _update_stm32_key_dialog(self) -> None:
        devices = self.config.get("devices", [])
        if not devices:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Once bir cihaz ekleyin.")
            return

        dialog = QDialog(self._dialog_parent())
        dialog.setWindowTitle("STM32 AES Key Guncelle")
        form = QFormLayout(dialog)

        device_combo = QComboBox(dialog)
        device_combo.addItems([d.get("name", "Isimsiz") for d in devices])
        key_edit = QLineEdit(dialog)
        key2_edit = QLineEdit(dialog)
        key_edit.setEchoMode(QLineEdit.Password)
        key2_edit.setEchoMode(QLineEdit.Password)

        form.addRow("Cihaz:", device_combo)
        form.addRow("Yeni AES Key (64 hex):", key_edit)
        form.addRow("Yeni AES Key (tekrar):", key2_edit)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel, parent=dialog)
        form.addRow(buttons)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)

        if dialog.exec() != QDialog.Accepted:
            return

        new_key = key_edit.text().strip()
        new_key2 = key2_edit.text().strip()
        if new_key != new_key2 or not new_key:
            QMessageBox.critical(self._dialog_parent(), "Hata", "Key alanlari bos veya eslesmiyor.")
            return
        if len(new_key) != 64:
            QMessageBox.critical(self._dialog_parent(), "Hata", "Key 64 hex karakter olmali.")
            return
        try:
            bytes.fromhex(new_key)
        except ValueError:
            QMessageBox.critical(self._dialog_parent(), "Hata", "Key hex formatinda degil.")
            return

        idx = device_combo.currentIndex()
        if idx < 0 or idx >= len(devices):
            return
        selected_device = devices[idx]

        confirm = QMessageBox.question(
            self._dialog_parent(),
            "Onay",
            "STM32 uzerindeki AES key degistirilecek. Isleme devam edilsin mi?",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if confirm != QMessageBox.Yes:
            return

        serial_port = self.port_combo.currentText().strip()
        if not serial_port or "bulunamadi" in serial_port.lower():
            serial_port = self.config.get("serial_port", "COM7")

        update_cfg = {
            "serial_port": serial_port,
            "baud_rate": self.config.get("baud_rate", 115200),
            "aes_key_hex": selected_device.get("aes_key_hex", ""),
        }
        context = {"device_name": selected_device.get("name", ""), "new_key": new_key}
        self._set_progress_busy(True)
        self._start_status_animation("STM32 key guncelleniyor")

        def worker() -> None:
            success = update_stm32_key(update_cfg, new_key, log=lambda msg: self.signals.log.emit(str(msg)))
            self.signals.key_update_finished.emit(success, context)

        threading.Thread(target=worker, daemon=True).start()

    def _on_key_update_finished(self, success: bool, context: Any) -> None:
        self._set_progress_busy(False)
        if not isinstance(context, dict):
            self._stop_status_animation("Hazir")
            return
        device_name = context.get("device_name", "")
        new_key = context.get("new_key", "")

        if success:
            for dev in self.config.get("devices", []):
                if dev.get("name") == device_name:
                    dev["aes_key_hex"] = new_key
                    break
            self._append_log("Cihaz AES key guncellendi. Kaydetmek icin 'Tum Ayarlari Kaydet' tusunu kullanin.")
            self._refresh_admin_device_list()
            self._set_admin_status("STM32 key guncellendi (kaydetmeyi unutmayin).")
            self._stop_status_animation("STM32 key guncelleme basarili.")
            QMessageBox.information(self._dialog_parent(), "Basarili", "STM32 key guncelleme basarili.")
        else:
            self._set_admin_status("STM32 key guncelleme basarisiz.")
            self._stop_status_animation("STM32 key guncelleme basarisiz.")
            QMessageBox.critical(self._dialog_parent(), "Hata", "STM32 key guncelleme basarisiz.")

    def shutdown(self) -> None:
        stop_error = self._stop_local_proxy_server(announce=False)
        if stop_error and self._is_embedded_proxy_running():
            self._append_log(stop_error)


def main() -> None:
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    controller = FirmwareUpdaterQtApp()
    app.aboutToQuit.connect(controller.shutdown)
    controller.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
