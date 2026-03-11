import os
import sys
import threading
from typing import Any, Dict, List, Optional

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
from drive_manager import DriveManager
from firmware_proxy_client import FirmwareProxyManager
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


class FirmwareUpdaterQtApp:
    def __init__(self) -> None:
        self.config: Dict[str, Any] = DEFAULT_CONFIG.copy()
        self.admin_password: Optional[str] = None
        self.drive_manager: Optional[Any] = None
        self.available_files: List[Dict[str, Any]] = []
        self.pending_drive_version: Optional[int] = None
        self.stop_requested = False
        self.upload_thread: Optional[threading.Thread] = None

        self.signals = UiSignals()
        self.signals.log.connect(self._append_log)
        self.signals.progress.connect(self._on_worker_progress)
        self.signals.scan_finished.connect(self._on_scan_finished)
        self.signals.upload_finished.connect(self._on_upload_finished)
        self.signals.key_update_finished.connect(self._on_key_update_finished)

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
        self.add_device_button: Optional[QPushButton] = None
        self.edit_device_button: Optional[QPushButton] = None
        self.delete_device_button: Optional[QPushButton] = None
        self.save_all_button: Optional[QPushButton] = None
        self.change_password_button: Optional[QPushButton] = None
        self.reset_config_button: Optional[QPushButton] = None
        self.update_stm32_key_button: Optional[QPushButton] = None
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
        self.save_all_button = self._aw(QPushButton, "saveAllButton")
        self.change_password_button = self._aw(QPushButton, "changePasswordButton")
        self.reset_config_button = self._aw(QPushButton, "resetConfigButton")
        self.update_stm32_key_button = self._aw(QPushButton, "updateStm32KeyButton")
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

    def _open_admin_window(self) -> None:
        self._ensure_admin_window()
        if self.admin_window is None:
            return
        self._refresh_admin_panel()
        self.admin_window.show()
        self.admin_window.raise_()
        self.admin_window.activateWindow()
        self._play_admin_intro_animation()
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
        ]
        danger_buttons = [self.delete_device_button, self.reset_config_button]
        primary_buttons = [self.add_device_button, self.edit_device_button, self.save_all_button]

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
            QLabel#adminInfoLabel, QLabel#securityHintLabel, QLabel#deviceCountLabel {
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

    def _append_log(self, message: str) -> None:
        stamp = QDateTime.currentDateTime().toString("HH:mm:ss")
        self.log_text.appendPlainText(f"[{stamp}] {message}")
        self.log_text.verticalScrollBar().setValue(self.log_text.verticalScrollBar().maximum())

    def _firmware_source(self) -> str:
        raw = str(self.config.get("firmware_source", "proxy")).strip().lower()
        if raw == "drive":
            return "drive"
        return "proxy"

    def _migrate_device_schema(self) -> None:
        devices = self.config.get("devices", [])
        if not isinstance(devices, list):
            return
        source = self._firmware_source()
        for device in devices:
            if not isinstance(device, dict):
                continue
            channel = str(device.get("firmware_channel", "")).strip()
            legacy_drive_id = str(device.get("drive_file_id", "")).strip()
            if not channel and legacy_drive_id:
                device["firmware_channel"] = legacy_drive_id
            if source == "proxy":
                device.pop("drive_file_id", None)

    def _reload_drive_manager(self) -> None:
        source = self._firmware_source()
        if source == "proxy":
            proxy_key = str(self.config.get("proxy_api_key", "")).strip()
            if not proxy_key:
                proxy_key = os.environ.get("FIRMWARE_PROXY_API_KEY", "").strip()
            self.drive_manager = FirmwareProxyManager(
                base_url=str(self.config.get("proxy_base_url", "")),
                api_key=proxy_key,
            )
            return
        service_json = self.config.get("service_account_json", "")
        self.drive_manager = DriveManager(service_account_json=service_json)

    def _try_load_config(self) -> None:
        if config_exists():
            try:
                self.config = load_config("admin")
                self._migrate_device_schema()
                self._append_log("Config yuklendi (varsayilan sifre ile).")
            except ValueError:
                self._append_log("Config sifreli. Admin girisi yapmadan admin paneli acilmaz.")
        else:
            self._append_log("Config dosyasi yok. Admin panelinden cihaz ekleyin.")
        self._reload_drive_manager()
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

        source = self._firmware_source()
        proxy_key = str(self.config.get("proxy_api_key", "")).strip()
        if not proxy_key:
            proxy_key = os.environ.get("FIRMWARE_PROXY_API_KEY", "").strip()

        if source == "proxy" and not proxy_key:
            self._stop_update_animation("Proxy API key tanimli degil")
            self._append_log("Proxy API key eksik. Admin panelde backend alanina 'url|api_key' girip kaydedin.")
            self._shake_widget(self.device_combo)
            return

        if source == "proxy":
            file_selector = str(device.get("firmware_channel", "")).strip()
            missing_msg = "Firmware kanali tanimli degil"
        else:
            file_selector = str(device.get("drive_file_id", device.get("firmware_channel", ""))).strip()
            missing_msg = "Drive klasor ID tanimli degil"

        if not file_selector:
            self._stop_update_animation(missing_msg)
            self._shake_widget(self.device_combo)
            return

        self._start_update_animation("Firmware listesi aliniyor")
        self._start_status_animation("Sunucu yaniti bekleniyor")
        self._set_progress_busy(True)
        self.scan_folder_button.setEnabled(False)
        self.available_files = []

        if self.drive_manager is None:
            self._reload_drive_manager()

        def worker() -> None:
            try:
                files, error = self.drive_manager.list_all_files_in_folder(file_selector)  # type: ignore[union-attr]
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
        self._append_log("Firmware listesi yenilendi: " + msg)

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
        target_file_ref = str(file_item.get("id", "")).strip()
        file_type = file_item.get("type", "BIN")
        self.pending_drive_version = file_item.get("version")

        if not target_file_ref:
            QMessageBox.warning(self.window, "Uyari", "Secilen dosya icin gecerli indirme referansi yok.")
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
            "firmware_source": self._firmware_source(),
            "file_ref": target_file_ref,
            "aes_key_hex": aes_key,
            "packet_size": self.config.get("packet_size", 128),
            "max_retries": self.config.get("max_retries", 7),
            "firmware_version": device.get("firmware_version", 1),
            "file_type": file_type,
            "filename": file_item.get("name", ""),
            "is_rf": self.rf_mode_check.isChecked(),
            # RF güvenlik parametreleri
            "auth_key_hex": self.config.get("auth_key_hex", "A1B2C3D4E5F60718293A4B5C6D7E8F900112233445566778899AABBCCDDEEFF0"),
            "auth_password_hex": self.config.get("auth_password_hex", "DEADBEEFCAFEBABE123456789ABCDEF0"),
            "private_key_path": self.config.get("private_key_path", "private_key.pem"),
        }

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
            if self.drive_manager is None:
                self._reload_drive_manager()
            success = upload_firmware(
                config=upload_cfg,
                log=lambda msg: self.signals.log.emit(str(msg)),
                on_progress=lambda cur, total: self.signals.progress.emit(int(cur), int(total)),
                stop_flag=lambda: self.stop_requested,
                drive_manager=self.drive_manager,
            )
            self.signals.upload_finished.emit(bool(success))

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
            if device and self.pending_drive_version is not None:
                device["last_installed_version"] = self.pending_drive_version
                self.pending_drive_version = None
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
        self.pending_drive_version = None

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
            self._migrate_device_schema()
        except ValueError as exc:
            QMessageBox.critical(self._dialog_parent(), "Hata", str(exc))
            return

        self.admin_password = password
        self._reload_drive_manager()
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
            if self._firmware_source() == "proxy":
                self.service_json_edit.setPlaceholderText(
                    "Proxy Base URL (ornek: https://fw-proxy.example.com | opsiyonel: url|api_key)"
                )
                self.service_json_edit.setText(str(self.config.get("proxy_base_url", "")))
            else:
                self.service_json_edit.setPlaceholderText("Google Service Account JSON yolu")
                self.service_json_edit.setText(str(self.config.get("service_account_json", "")))
        self._refresh_admin_device_list()

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
            item.setToolTip("Firmware referansi: gizli")
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

    def _mask_drive_id_value(self, value: str) -> str:
        clean = value.strip()
        if not clean:
            return "-"
        if len(clean) <= 6:
            return "*" * len(clean)
        return f"{clean[:3]}{'*' * (len(clean) - 6)}{clean[-3:]}"

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
        channel_value = str(dev.get("firmware_channel", dev.get("drive_file_id", "")))
        drive_text = self._mask_drive_id_value(channel_value)
        self.meta_name_value.setText(str(dev.get("name", "Isimsiz")))
        self.meta_fw_value.setText(f"v{dev.get('firmware_version', '?')}")
        self.meta_last_value.setText(f"v{dev.get('last_installed_version', '?')}")
        self.meta_drive_value.setText(drive_text)
        self.meta_aes_value.setText(aes_text)

    def _device_dialog(self, title: str, existing: Optional[Dict[str, Any]] = None) -> Optional[Dict[str, Any]]:
        existing = existing or {}
        source = self._firmware_source()
        dialog = QDialog(self._dialog_parent())
        dialog.setWindowTitle(title)
        form = QFormLayout(dialog)

        name_edit = QLineEdit(dialog)
        ref_edit = QLineEdit(dialog)
        key_edit = QLineEdit(dialog)
        ver_edit = QLineEdit(dialog)
        last_ver_edit = QLineEdit(dialog)

        if source == "proxy":
            existing_ref = str(existing.get("firmware_channel", existing.get("drive_file_id", ""))).strip()
            ref_label = "Firmware Kanali:"
            missing_ref_message = "Firmware kanali bos olamaz."
            existing_hint = "Yeni kanal girin (bos ise mevcut korunur)"
            empty_hint = "Ornek: urun-a-seri-1"
        else:
            existing_ref = str(existing.get("drive_file_id", existing.get("firmware_channel", ""))).strip()
            ref_label = "Drive Klasor ID (gizli):"
            missing_ref_message = "Drive klasor ID bos olamaz."
            existing_hint = "Yeni Drive ID girin (bos ise mevcut korunur)"
            empty_hint = "Drive Klasor ID"

        name_edit.setText(str(existing.get("name", "")))
        ref_edit.setEchoMode(QLineEdit.Password)
        if existing_ref:
            ref_edit.setPlaceholderText(existing_hint)
        else:
            ref_edit.setPlaceholderText(empty_hint)
        key_edit.setText(str(existing.get("aes_key_hex", "")))
        ver_edit.setText(str(existing.get("firmware_version", 1)))
        last_ver_edit.setText(str(existing.get("last_installed_version", 0)))

        form.addRow("Cihaz Adi:", name_edit)
        form.addRow(ref_label, ref_edit)
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
        ref_input = ref_edit.text().strip()
        firmware_ref = ref_input if ref_input else existing_ref
        key_hex = key_edit.text().strip()

        if not name:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Cihaz adi bos olamaz.")
            return None
        if not firmware_ref:
            QMessageBox.warning(self._dialog_parent(), "Uyari", missing_ref_message)
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

        result = {
            "name": name,
            "aes_key_hex": key_hex,
            "firmware_version": fw_ver,
            "last_installed_version": last_ver,
        }
        if source == "proxy":
            result["firmware_channel"] = firmware_ref
            result.pop("drive_file_id", None)
        else:
            result["drive_file_id"] = firmware_ref
        return result

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
        if None in (self.baud_edit, self.retry_edit, self.packet_edit, self.default_port_edit, self.service_json_edit):
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Admin arayuzu tam yuklenemedi.")
            return
        try:
            self.config["baud_rate"] = int(self.baud_edit.text().strip())  # type: ignore[union-attr]
            self.config["max_retries"] = int(self.retry_edit.text().strip())  # type: ignore[union-attr]
            self.config["packet_size"] = int(self.packet_edit.text().strip())  # type: ignore[union-attr]
            self.config["serial_port"] = self.default_port_edit.text().strip()  # type: ignore[union-attr]
            backend_value = self.service_json_edit.text().strip()  # type: ignore[union-attr]
            proxy_url = backend_value
            proxy_key = str(self.config.get("proxy_api_key", "")).strip()
            if not proxy_key:
                proxy_key = os.environ.get("FIRMWARE_PROXY_API_KEY", "").strip()
            if backend_value.lower().startswith("http://") or backend_value.lower().startswith("https://"):
                if "|" in backend_value:
                    maybe_url, maybe_key = backend_value.split("|", 1)
                    proxy_url = maybe_url.strip()
                    maybe_key = maybe_key.strip()
                    if maybe_key:
                        proxy_key = maybe_key
            if backend_value.lower().startswith("http://") or backend_value.lower().startswith("https://"):
                self.config["firmware_source"] = "proxy"
                self.config["proxy_base_url"] = proxy_url
                self.config["proxy_api_key"] = proxy_key
                self.config["service_account_json"] = ""
            elif self._firmware_source() == "proxy":
                self.config["proxy_base_url"] = backend_value
                self.config["service_account_json"] = ""
            else:
                self.config["firmware_source"] = "drive"
                self.config["service_account_json"] = backend_value
            self._migrate_device_schema()
        except ValueError:
            QMessageBox.warning(self._dialog_parent(), "Uyari", "Sayi alanlari gecerli olmali.")
            return

        try:
            save_config(self.config, self.admin_password)
            self._reload_drive_manager()
            self._refresh_device_list()
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
            self._reload_drive_manager()
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


def main() -> None:
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    controller = FirmwareUpdaterQtApp()
    controller.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
