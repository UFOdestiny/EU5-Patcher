#!/usr/bin/env python3
"""EU5 Patcher - Enable Achievements Unconditionally"""

import os
import platform
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Final, Optional


def get_steam_install_path() -> Optional[str]:
    """
    Get the Steam installation path.
    On Windows queries the registry. On Linux checks standard paths.
    Returns None if Steam is not found.
    """
    system = platform.system()

    if system == "Windows":
        try:
            import winreg
        except ImportError:
            return None
        try:
            key = winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\\WOW6432Node\\Valve\\Steam"
            )
            path, _ = winreg.QueryValueEx(key, "InstallPath")
            winreg.CloseKey(key)
            return path
        except FileNotFoundError:
            try:
                key = winreg.OpenKey(
                    winreg.HKEY_CURRENT_USER, r"Software\\Valve\\Steam"
                )
                path, _ = winreg.QueryValueEx(key, "SteamPath")
                winreg.CloseKey(key)
                return path
            except Exception:
                return None

    if system == "Linux":
        candidates = [
            os.path.expanduser("~/.local/share/Steam"),
            os.path.expanduser("~/.steam/steam"),
            os.path.expanduser("~/.var/app/com.valvesoftware.Steam/.steam/steam"),
        ]
        for candidate in candidates:
            if os.path.isdir(candidate):
                return candidate
        return None

    return None


def find_all_steam_libraries_with_app(vdf_path: str, target_appid: str) -> list[str]:
    """Return all Steam library paths that contain the given AppID."""
    results: list[str] = []
    current_path = None
    in_apps_block = False

    with open(vdf_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()

            if line.startswith('"path"'):
                # "path"    "E:\SteamLibrary"
                current_path = line.split('"')[3]

            # 进入 apps 块
            elif line == '"apps"':
                in_apps_block = True

            elif in_apps_block:
                # apps 块结束
                if line == "}":
                    in_apps_block = False
                    continue

                # 检查 AppID
                if line.startswith(f'"{target_appid}"'):
                    if current_path:
                        results.append(current_path)

    return results


def get_game_folder(name: str) -> Optional[str]:
    """2
    Get the installation folder of a Steam game by its name.
    Checks all Steam libraries and returns the first path where the game exists.
    Returns None if the game is not found.
    """
    steam_path = get_steam_install_path()
    if not steam_path:
        return None

    library_db = os.path.join(steam_path, "steamapps", "libraryfolders.vdf")
    if not os.path.isfile(library_db):
        return None

    library_paths = find_all_steam_libraries_with_app(library_db, "3450310")  # EU5
    for library_path in library_paths:
        game_folder = os.path.join(library_path, "steamapps", "common", name)
        binaries_path = os.path.join(game_folder, "binaries", "eu5.exe")
        if os.path.isfile(binaries_path):
            return game_folder

    return None


# Constants
# Achievement Check
PATTERN: Final[str] = (
    "80 ?? ?? ?? ?? ?? 00 75 ?? "
    "80 ?? ?? ?? ?? ?? 00 74 ?? "
    "80 ?? ?? ?? ?? ?? 00 74 ?? "
    "80 ?? ?? ?? ?? ?? 00"
)

PATTERN_REPLACE: Final[str] = "80 ?? ?? ?? ?? ?? 00 eb"

# Ironman Save and Load: GameIsIronman
PATTERN2: Final[str] = (
    "74 ?? "
    "?? ?? ?? "
    "ff ?? ?? "
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "74 ?? "
    "32 c0"
)

PATTERN_REPLACE2: Final[str] = (
    "eb ?? "
    "?? ?? ?? "
    "ff ?? ?? "
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "74 ?? "
    "b0 00"
)

# Ironman Console: BLOCK_COMMAND_MULTIPLAYER_IRONMAN
PATTERN3: Final[str] = (
    "00 "
    "75 ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "32 c0 "
    "48 ?? ?? ?? "
    "c3 "
    "b0 01"
)

PATTERN_REPLACE3: Final[str] = (
    "00 "
    "75 ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "32 c0 "
    "48 ?? ?? ?? "
    "c3 "
    "b0 00"
)

# Save: SaveIngame
PATTERN4: Final[str] = (
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "48 ?? ?? "
    "e8"
)

PATTERN_REPLACE4: Final[str] = "e8 ?? ?? ?? ?? 80 ?? ?? ?? ?? ?? 09"

# Load: LoadIngame
PATTERN5: Final[str] = (
    "e8 ?? ?? ?? ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "80 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "83 ?? ?? ?? ?? ?? 00 "
    "75 ?? "
    "48 ?? ?? "
    "e8"
)

PATTERN_REPLACE5: Final[str] = "e8 ?? ?? ?? ?? 80 ?? ?? ?? ?? ?? 09"


SCRIPT_DIR: Final[Path] = Path(__file__).resolve().parent

EU5_PATH: Final[Path] = SCRIPT_DIR / "eu5.exe"
_game_folder = get_game_folder("Europa Universalis V")
STEAM_EU5_PATH: Final[Optional[Path]] = (
    Path(_game_folder) / "binaries" / "eu5.exe" if _game_folder else None
)
EU5_BACKUP_SUFFIX: Final[str] = ".backup"
EU5_BACKUP_PATH: Final[Path] = SCRIPT_DIR / "eu5.exe.backup"

debug_info = False


@dataclass(frozen=True)
class PatchDefinition:
    label: str
    pattern: str
    replacement: str


PATCHES: Final[list[PatchDefinition]] = [
    PatchDefinition("Patch #1", PATTERN, PATTERN_REPLACE),
    PatchDefinition("Patch #2", PATTERN2, PATTERN_REPLACE2),
    PatchDefinition("Patch #3", PATTERN3, PATTERN_REPLACE3),
    PatchDefinition("Patch #4", PATTERN4, PATTERN_REPLACE4),
    PatchDefinition("Patch #5", PATTERN5, PATTERN_REPLACE5),
]


class PatchError(Exception):
    """Custom exception for patch-related errors."""


def pattern_to_regex(pattern_str: str) -> re.Pattern[bytes]:
    """Convert a hex pattern string with wildcards to a compiled regex."""
    parts = pattern_str.split()
    regex_parts: list[bytes] = []
    wildcard_count = 0

    for part in parts:
        if part == "??":
            wildcard_count += 1
        else:
            if wildcard_count > 0:
                regex_parts.append(f".{{{wildcard_count}}}".encode())
                wildcard_count = 0
            # Escape the byte in case it's a regex special char
            regex_parts.append(re.escape(bytes.fromhex(part)))

    if wildcard_count > 0:
        regex_parts.append(f".{{{wildcard_count}}}".encode())

    return re.compile(b"".join(regex_parts), re.DOTALL)


def pattern_to_list(pattern_str: str) -> list[int | None]:
    """Convert a hex pattern string to a list of byte values (None for wildcards)."""
    return [None if part == "??" else int(part, 16) for part in pattern_str.split()]


def create_backup(source: Path, dest: Path) -> None:
    """Create a backup of the source file."""
    try:
        shutil.copy2(source, dest)
        print(f"Backup created: {dest}")
    except OSError as e:
        raise PatchError(f"Failed to create backup: {e}") from e


def find_pattern(data: bytes, pattern: re.Pattern[bytes]) -> list[int]:
    """Find the pattern in data and return its offsets."""
    matches = list(pattern.finditer(data))

    if len(matches) == 0:
        raise PatchError(
            "Pattern not found. Have you patched it before? "
            "If not, the pattern may need to be updated."
        )

    return [m.start() for m in matches]


def apply_patch(data: bytearray, offset: int, replacement: list[int | None]) -> None:
    """Apply the replacement pattern at the specified offset."""
    for i, value in enumerate(replacement):
        original = data[offset + i]
        if value is None:
            if debug_info:
                print(
                    f"{offset + i:#x}: {original:#04x} -> {original:#04x} (unchanged)"
                )
        else:
            if debug_info:
                print(f"{offset + i:#x}: {original:#04x} -> {value:#04x}")
            data[offset + i] = value


def make_patch(filepath: Path) -> None:
    """Patch the target executable."""
    # Read file
    try:
        data = bytearray(filepath.read_bytes())
    except OSError as e:
        raise PatchError(f"Failed to read file: {e}") from e

    # Find every pattern and prepare replacements before touching disk
    patch_jobs: list[tuple[PatchDefinition, int, list[int | None]]] = []
    for patch_def in PATCHES:
        regex = pattern_to_regex(patch_def.pattern)
        offsets = find_pattern(data, regex)
        replacement = pattern_to_list(patch_def.replacement)
        for offset in offsets:
            patch_jobs.append((patch_def, offset, replacement))

    # Create backup only after both patterns are confirmed
    create_backup(filepath, filepath.with_name(filepath.name + EU5_BACKUP_SUFFIX))

    for patch_def, offset, replacement in patch_jobs:
        print(f"\n{patch_def.label} found at offset: {offset:#x}")
        if debug_info:
            print(f"Applying {patch_def.label}...\n")
        apply_patch(data, offset, replacement)

    # Write back
    try:
        filepath.write_bytes(data)
        print("\nEU5 is successfully patched.")
    except OSError as e:
        raise PatchError(f"Failed to write file: {e}") from e


def main() -> int:
    """Main entry point."""
    if EU5_PATH.exists():
        path = EU5_PATH
        print(f"Path: {EU5_PATH}")
    elif STEAM_EU5_PATH and STEAM_EU5_PATH.exists():
        path = STEAM_EU5_PATH
        print(f"Path: {STEAM_EU5_PATH}")
    else:
        print(
            "eu5.exe not found. "
            "Place this script in .../Europa Universalis V/binaries/"
            "or install the game via Steam."
        )
        print(f"Expected paths:\n - {EU5_PATH}\n - {STEAM_EU5_PATH}")
        input("Press Enter to exit...")
        return 1
    try:
        make_patch(path)
    except PatchError as e:
        print(f"Error: {e}")
        input("Press Enter to exit...")
        return 1

    input("Press Enter to exit...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
