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
    state._auth_fails = {}
    state._auth_lock = threading.Lock()
    state._AUTH_FAIL_MAX = 10
    state._AUTH_FAIL_WINDOW = 60.0
    state._AUTH_BLOCK_TTL = 60.0
    state._channel_map_stat = (0.0, 0)
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


def test_catalog_unknown_channel_returns_none():
    """Bilinmeyen kanal için None ve hata mesajı döner."""
    state = _make_proxy_state(channel_map={"other-channel": "folder_xyz"})

    files, error = state.get_catalog("nonexistent-channel")

    assert files is None
    assert error is not None
    state.drive.list_all_files_in_folder.assert_not_called()


def test_channel_map_reload_on_file_change():
    """_poll_channel_map_once mtime değişince reload çağırır."""
    state = _make_proxy_state()
    state._channel_map_stat = (1000.0, 100)

    reload_called = []

    def mock_reload():
        reload_called.append(True)
        state._catalog_cache.clear()
        state.channel_map = {"test-channel": "folder_abc"}

    state.reload = mock_reload

    # Farklı mtime/size simüle et
    new_stat = MagicMock()
    new_stat.st_mtime = 2000.0
    new_stat.st_size = 200

    with patch("firmware_proxy_server.os.stat", return_value=new_stat):
        state._poll_channel_map_once()

    assert len(reload_called) == 1
    assert state._channel_map_stat == (2000.0, 200)


def test_channel_map_no_reload_if_unchanged():
    """mtime ve size değişmediyse reload çağrılmaz."""
    state = _make_proxy_state()
    state._channel_map_stat = (1000.0, 100)

    reload_called = []
    state.reload = lambda: reload_called.append(True)

    unchanged_stat = MagicMock()
    unchanged_stat.st_mtime = 1000.0
    unchanged_stat.st_size = 100

    with patch("firmware_proxy_server.os.stat", return_value=unchanged_stat):
        state._poll_channel_map_once()

    assert len(reload_called) == 0


def test_stat_channel_map_returns_zero_on_oserror():
    """OSError olursa (0.0, 0) döner."""
    state = _make_proxy_state()
    with patch("firmware_proxy_server.os.stat", side_effect=OSError):
        result = state._stat_channel_map()
    assert result == (0.0, 0)


def test_send_binary_chunks():
    """_send_binary BytesIO'yu 8KB chunk'larla gönderir."""
    import io
    from firmware_proxy_server import _send_binary

    # 20KB veri — en az 3 chunk gerekir
    data = io.BytesIO(b"x" * (8192 * 2 + 512))

    written_chunks = []
    handler = MagicMock()
    handler.wfile.write.side_effect = lambda chunk: written_chunks.append(len(chunk))

    _send_binary(handler, "test.bin", data)

    assert len(written_chunks) == 3
    assert written_chunks[0] == 8192
    assert written_chunks[1] == 8192
    assert written_chunks[2] == 512
    # Content-Length doğru gönderildi
    content_length_calls = [
        c for c in handler.send_header.call_args_list
        if c[0][0] == "Content-Length"
    ]
    assert content_length_calls[0][0][1] == str(8192 * 2 + 512)


def test_rate_limit_blocks_after_10_fails():
    """10 hatalı denemeden sonra IP bloklanır."""
    state = _make_proxy_state()

    for _ in range(10):
        blocked = state.record_auth_fail("1.2.3.4")

    assert blocked is True
    assert state.is_auth_blocked("1.2.3.4") is True


def test_rate_limit_resets_after_window():
    """60 saniyelik pencere geçince blok kalkar."""
    state = _make_proxy_state()

    for _ in range(10):
        state.record_auth_fail("1.2.3.4")

    # Zamanı geçmişe al
    state._auth_fails["1.2.3.4"] = (10, time.time() - 61)

    assert state.is_auth_blocked("1.2.3.4") is False


def test_rate_limit_clears_on_success():
    """Başarılı auth sonrası hata sayacı sıfırlanır."""
    state = _make_proxy_state()

    for _ in range(5):
        state.record_auth_fail("1.2.3.4")

    state.clear_auth_fail("1.2.3.4")

    assert state.is_auth_blocked("1.2.3.4") is False
    assert "1.2.3.4" not in state._auth_fails


def test_rate_limit_10th_request_gets_429():
    """10. hatalı deneme anında 429 döner (blocked=True)."""
    state = _make_proxy_state()

    # İlk 9 deneme — bloklanmaz
    for _ in range(9):
        blocked = state.record_auth_fail("5.6.7.8")
    assert not blocked

    # 10. deneme — bloklanır
    blocked = state.record_auth_fail("5.6.7.8")
    assert blocked is True
