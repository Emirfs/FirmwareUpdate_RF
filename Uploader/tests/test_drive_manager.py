import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from unittest.mock import MagicMock, patch, call
from drive_manager import DriveManager


def _make_service(pages):
    """pages: list of lists — her eleman bir sayfa dosya listesi."""
    service = MagicMock()
    files_resource = service.files.return_value
    list_resource = files_resource.list.return_value

    responses = []
    for i, page_files in enumerate(pages):
        resp = {"files": page_files}
        if i < len(pages) - 1:
            resp["nextPageToken"] = f"token_{i+1}"
        responses.append(resp)

    list_resource.execute.side_effect = responses
    return service


def test_list_all_files_single_page():
    """Tek sayfa — nextPageToken yok."""
    page1 = [{"id": "a", "name": "update_1.bin", "size": "1024"}]
    dm = DriveManager.__new__(DriveManager)
    dm.service = _make_service([page1])

    files, err = dm.list_all_files_in_folder("folder_xyz")

    assert err is None
    assert len(files) == 1
    assert files[0]["id"] == "a"


def test_list_all_files_multiple_pages():
    """İki sayfa — ikinci sayfadaki dosyalar da gelir."""
    page1 = [{"id": "a", "name": "update_1.bin", "size": "100"}]
    page2 = [{"id": "b", "name": "update_2.bin", "size": "200"}]
    dm = DriveManager.__new__(DriveManager)
    dm.service = _make_service([page1, page2])

    files, err = dm.list_all_files_in_folder("folder_xyz")

    assert err is None
    assert len(files) == 2
    ids = {f["id"] for f in files}
    assert ids == {"a", "b"}


def test_list_all_files_skips_non_firmware():
    """Sadece .bin ve .hex dosyaları döner."""
    page1 = [
        {"id": "a", "name": "update_1.bin", "size": "100"},
        {"id": "b", "name": "readme.txt", "size": "50"},
        {"id": "c", "name": "update_2.hex", "size": "200"},
    ]
    dm = DriveManager.__new__(DriveManager)
    dm.service = _make_service([page1])

    files, err = dm.list_all_files_in_folder("folder_xyz")

    assert len(files) == 2
    names = {f["name"] for f in files}
    assert names == {"update_1.bin", "update_2.hex"}
