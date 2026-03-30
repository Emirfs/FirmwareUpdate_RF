import sys
import os
import time
import threading
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from unittest.mock import MagicMock, patch


def _make_proxy_state(api_key="testkey", channel_map=None, token_ttl=120):
    """ProxyState oluşturur; Drive ve dosya sistemi mock'lanır."""
    from firmware_proxy_server import ProxyState

    if channel_map is None:
        channel_map = {"test-channel": "folder_abc"}

    state = ProxyState.__new__(ProxyState)
    state.api_key = api_key
    state.token_ttl = token_ttl
    state.channel_map = channel_map
    state.channel_map_file = "fake_channels.json"
    state._catalog_cache = {}
    state._catalog_lock = threading.Lock()
    state._CATALOG_TTL = 60.0
    state._auth_fails = {}
    state._auth_lock = threading.Lock()
    # Mock drive
    state.drive = MagicMock()
    return state


def test_catalog_cache_hit_skips_drive():
    """İkinci çağrı Drive API'ye gitmez."""
    state = _make_proxy_state()
    fake_files = [{"id": "f1", "name": "update_1.bin", "version": 1, "type": "BIN", "size": "100"}]
    state.drive.list_all_files_in_folder.return_value = (fake_files, None)

    # İlk çağrı — Drive çağrılır
    files1, err1 = state.get_catalog("test-channel")
    assert err1 is None
    assert files1 == fake_files
    assert state.drive.list_all_files_in_folder.call_count == 1

    # İkinci çağrı — cache'den gelmeli, Drive tekrar çağrılmamalı
    files2, err2 = state.get_catalog("test-channel")
    assert err2 is None
    assert files2 == fake_files
    assert state.drive.list_all_files_in_folder.call_count == 1  # hâlâ 1


def test_catalog_cache_miss_after_ttl():
    """TTL geçince Drive tekrar çağrılır."""
    state = _make_proxy_state()
    fake_files = [{"id": "f1", "name": "update_1.bin", "version": 1, "type": "BIN", "size": "100"}]
    state.drive.list_all_files_in_folder.return_value = (fake_files, None)

    files1, _ = state.get_catalog("test-channel")
    assert state.drive.list_all_files_in_folder.call_count == 1

    # Cache zamanını manuel olarak geçmişe al
    state._catalog_cache["test-channel"] = (time.time() - 61, fake_files)

    files2, _ = state.get_catalog("test-channel")
    assert state.drive.list_all_files_in_folder.call_count == 2


def test_catalog_cache_cleared_on_reload():
    """reload() sonrası cache boşalır."""
    state = _make_proxy_state()
    fake_files = [{"id": "f1", "name": "update_1.bin", "version": 1, "type": "BIN", "size": "100"}]
    state.drive.list_all_files_in_folder.return_value = (fake_files, None)

    state.get_catalog("test-channel")
    assert "test-channel" in state._catalog_cache

    with patch.object(state, "_load_channel_map", return_value={"test-channel": "folder_abc"}):
        state.reload()

    assert state._catalog_cache == {}
