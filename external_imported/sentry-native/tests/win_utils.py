import pathlib
import win32api

from tests import SENTRY_VERSION


def check_binary_version(binary_path: pathlib.Path):
    if not binary_path.exists():
        return

    info = win32api.GetFileVersionInfo(str(binary_path), "\\")
    ms = info["FileVersionMS"]
    ls = info["FileVersionLS"]
    file_version = (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    file_version = f"{file_version[0]}.{file_version[1]}.{file_version[2]}"
    if SENTRY_VERSION != file_version:
        raise RuntimeError(
            f"Binary {binary_path.parts[-1]} has a different version ({file_version}) than expected ({SENTRY_VERSION})."
        )
