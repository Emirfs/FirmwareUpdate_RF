# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['gui_uploader_qt.py'],
    pathex=[],
    binaries=[],
    datas=[('ui/main_window.ui', 'ui'), ('ui/admin_window.ui', 'ui')],
    hiddenimports=[
        'config_manager',
        'drive_manager',
        'intelhex',
        'PySide6.QtUiTools',
        'PySide6.QtCore',
        'PySide6.QtWidgets',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='FirmwareUpdater',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
