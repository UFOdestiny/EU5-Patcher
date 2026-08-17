#!/usr/bin/env python3
"""EU5 Patcher - Enable Achievements Unconditionally"""

import os
import platform
import re
import shutil
import struct
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
# String anchors used for resilient location of the registered callbacks.
CAN_GET_ACHIEVEMENTS_ANCHOR: Final[bytes] = b"CanGetAchievements\x00"
IS_GAME_RULE_ENABLED_ANCHOR: Final[bytes] = b"IsGameRuleEnabled\x00"
RETURN_TRUE: Final[bytes] = b"\xb8\x01\x00\x00\x00\xc3"

# Patch #3 remains a function-level patch because checksum has other callers.
# It is now located from CanGetAchievements Branch-2 and forced to return true.
# These field offsets are used only to validate that the resolved callee has the
# expected checksum-like structure; they are not used as a global signature.
CHECKSUM_STATE_OFFSETS: Final[frozenset[int]] = frozenset(
    {0x130, 0x131, 0x132, 0x133, 0x134, 0x139}
)
CHECKSUM_MIN_STATE_FIELDS: Final[int] = 4


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
class PESection:
    name: str
    rva: int
    virtual_size: int
    raw_offset: int
    raw_size: int

    @property
    def raw_rva_end(self) -> int:
        return self.rva + self.raw_size

    def contains_rva(self, rva: int) -> bool:
        return self.rva <= rva < self.rva + max(self.virtual_size, self.raw_size)

    def contains_raw_offset(self, offset: int) -> bool:
        return self.raw_offset <= offset < self.raw_offset + self.raw_size


@dataclass(frozen=True)
class PatchJob:
    label: str
    offset: int
    replacement: tuple[int | None, ...]


class PatchError(Exception):
    """Custom exception for patch-related errors."""


def create_backup(source: Path, dest: Path) -> None:
    """Create a backup of the source file."""
    try:
        shutil.copy2(source, dest)
        print(f"Backup created: {dest}")
    except OSError as e:
        raise PatchError(f"Failed to create backup: {e}") from e


def find_pattern_optional(data: bytes, pattern: re.Pattern[bytes]) -> list[int]:
    """Find a pattern without raising when it is absent."""
    return [m.start() for m in pattern.finditer(data)]


def apply_patch(
    data: bytearray, offset: int, replacement: tuple[int | None, ...]
) -> None:
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


def parse_pe_sections(data: bytes) -> list[PESection]:
    """Parse enough of a PE image to map file offsets and RVAs."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise PatchError("Target is not a valid PE image (missing MZ header).")

    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\x00\x00":
        raise PatchError("Target is not a valid PE image (missing PE header).")

    number_of_sections = struct.unpack_from("<H", data, pe_offset + 6)[0]
    optional_header_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    section_table = pe_offset + 24 + optional_header_size

    sections: list[PESection] = []
    for i in range(number_of_sections):
        off = section_table + i * 40
        if off + 40 > len(data):
            raise PatchError("PE section table is truncated.")

        name = (
            data[off : off + 8].split(b"\x00", 1)[0].decode("ascii", errors="replace")
        )
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, off + 8
        )
        sections.append(PESection(name, rva, virtual_size, raw_offset, raw_size))

    return sections


def get_section(sections: list[PESection], name: str) -> PESection:
    for section in sections:
        if section.name == name:
            return section
    raise PatchError(f"PE section {name!r} was not found.")


def raw_offset_to_rva(sections: list[PESection], offset: int) -> int:
    for section in sections:
        if section.contains_raw_offset(offset):
            return section.rva + (offset - section.raw_offset)
    raise PatchError(f"File offset {offset:#x} does not belong to a PE section.")


def rva_to_raw_offset(sections: list[PESection], rva: int) -> int:
    for section in sections:
        delta = rva - section.rva
        if 0 <= delta < section.raw_size:
            return section.raw_offset + delta
    raise PatchError(f"RVA {rva:#x} does not map to raw file data.")


def find_string_rvas(
    data: bytes, sections: list[PESection], anchor: bytes
) -> list[int]:
    """Return every mapped RVA containing the exact NUL-terminated ASCII anchor."""
    rvas: list[int] = []
    start = 0
    while True:
        offset = data.find(anchor, start)
        if offset < 0:
            break
        try:
            rvas.append(raw_offset_to_rva(sections, offset))
        except PatchError:
            pass
        start = offset + 1

    if not rvas:
        raise PatchError(
            f"String anchor {anchor[:-1].decode(errors='replace')!r} was not found."
        )
    return rvas


def iter_rip_lea_targets(
    data: bytes,
    text: PESection,
    opcode: bytes,
    start_rva: int,
    end_rva: int,
):
    """Yield (instruction_rva, target_rva) for a specific 7-byte RIP-relative LEA."""
    start_rva = max(start_rva, text.rva)
    end_rva = min(end_rva, text.raw_rva_end)
    if start_rva >= end_rva:
        return

    start_off = text.raw_offset + (start_rva - text.rva)
    end_off = text.raw_offset + (end_rva - text.rva)

    pos = start_off
    while pos + 7 <= end_off:
        if data[pos : pos + 3] == opcode:
            disp = struct.unpack_from("<i", data, pos + 3)[0]
            insn_rva = text.rva + (pos - text.raw_offset)
            target_rva = insn_rva + 7 + disp
            yield insn_rva, target_rva
        pos += 1


def parse_runtime_function_ranges(
    data: bytes, sections: list[PESection]
) -> list[tuple[int, int]]:
    """Read x64 RUNTIME_FUNCTION ranges from .pdata when available."""
    try:
        pdata = get_section(sections, ".pdata")
    except PatchError:
        return []

    ranges: list[tuple[int, int]] = []
    end = min(pdata.raw_offset + pdata.raw_size, len(data))
    for off in range(pdata.raw_offset, end - 11, 12):
        begin_rva, end_rva, _unwind_rva = struct.unpack_from("<III", data, off)
        if begin_rva and end_rva > begin_rva:
            ranges.append((begin_rva, end_rva))
    return ranges


def function_range_for_rva(
    rva: int,
    text: PESection,
    runtime_ranges: list[tuple[int, int]],
    *,
    fallback_before: int = 0x20,
    fallback_after: int = 0x100,
) -> tuple[int, int]:
    """Use .pdata boundaries; fall back to a small local window for leaf thunks."""
    containing = [(begin, end) for begin, end in runtime_ranges if begin <= rva < end]
    if containing:
        return min(containing, key=lambda item: item[1] - item[0])

    return (
        max(text.rva, rva - fallback_before),
        min(text.raw_rva_end, rva + fallback_after),
    )


def find_anchor_xrefs(
    data: bytes,
    text: PESection,
    anchor_rvas: list[int],
) -> list[tuple[int, int]]:
    """Find LEA RDX,[RIP+disp32] references to an anchor string."""
    anchor_set = set(anchor_rvas)
    results: list[tuple[int, int]] = []
    # 48 8D 15 xx xx xx xx == lea rdx, [rip+disp32]
    for insn_rva, target_rva in iter_rip_lea_targets(
        data, text, b"\x48\x8d\x15", text.rva, text.raw_rva_end
    ):
        if target_rva in anchor_set:
            results.append((insn_rva, target_rva))
    return results


def find_registered_callback(
    data: bytes,
    text: PESection,
    runtime_ranges: list[tuple[int, int]],
    string_xref_rva: int,
    string_rva: int,
) -> int:
    """
    Starting from the string xref inside the registration function, find the
    later LEA RDX,[RIP+callback] whose target points back into .text.
    """
    _begin, end = function_range_for_rva(
        string_xref_rva,
        text,
        runtime_ranges,
        fallback_before=0x20,
        fallback_after=0x180,
    )

    candidates: list[tuple[int, int]] = []
    for insn_rva, target_rva in iter_rip_lea_targets(
        data, text, b"\x48\x8d\x15", string_xref_rva + 7, end
    ):
        if target_rva != string_rva and text.contains_rva(target_rva):
            candidates.append((insn_rva, target_rva))

    if not candidates:
        raise PatchError(
            f"Could not resolve registered callback after string xref at {string_xref_rva:#x}."
        )

    # The callback argument is normally the nearest later RIP-relative LEA into .text.
    candidates.sort(key=lambda item: item[0])
    if debug_info and len(candidates) > 1:
        print(
            f"Warning: multiple callback candidates after {string_xref_rva:#x}: "
            + ", ".join(f"{target:#x}" for _, target in candidates)
        )
    return candidates[0][1]


def resolve_can_get_achievements_predicate(
    data: bytes,
    text: PESection,
    runtime_ranges: list[tuple[int, int]],
    callback_rva: int,
) -> int:
    """
    The registered CanGetAchievements callback is a thin wrapper. Its first
    argument is the real predicate, normally loaded with:
        lea rcx, [rip + predicate]
    """
    _begin, end = function_range_for_rva(
        callback_rva, text, runtime_ranges, fallback_before=0, fallback_after=0x60
    )

    candidates: list[tuple[int, int]] = []
    # 48 8D 0D xx xx xx xx == lea rcx, [rip+disp32]
    for insn_rva, target_rva in iter_rip_lea_targets(
        data, text, b"\x48\x8d\x0d", callback_rva, end
    ):
        if text.contains_rva(target_rva):
            candidates.append((insn_rva, target_rva))

    if not candidates:
        raise PatchError(
            f"Could not resolve inner CanGetAchievements predicate from callback {callback_rva:#x}."
        )

    candidates.sort(key=lambda item: item[0])
    if debug_info and len(candidates) > 1:
        print(
            f"Warning: multiple inner predicate candidates in callback {callback_rva:#x}: "
            + ", ".join(f"{target:#x}" for _, target in candidates)
        )
    return candidates[0][1]


def iter_rel32_control_targets(
    data: bytes,
    text: PESection,
    start_rva: int,
    end_rva: int,
):
    """Yield direct x64 CALL/JMP rel32 targets found in a bounded .text range."""
    start_rva = max(start_rva, text.rva)
    end_rva = min(end_rva, text.raw_rva_end)
    if start_rva >= end_rva:
        return

    start_off = text.raw_offset + (start_rva - text.rva)
    end_off = text.raw_offset + (end_rva - text.rva)

    pos = start_off
    while pos + 5 <= end_off:
        opcode = data[pos]
        if opcode in (0xE8, 0xE9):  # call rel32 / jmp rel32
            disp = struct.unpack_from("<i", data, pos + 1)[0]
            insn_rva = text.rva + (pos - text.raw_offset)
            target_rva = insn_rva + 5 + disp
            if text.contains_rva(target_rva):
                yield insn_rva, target_rva, opcode
        pos += 1


def checksum_structure_score(
    data: bytes,
    sections: list[PESection],
    text: PESection,
    runtime_ranges: list[tuple[int, int]],
    candidate_rva: int,
) -> tuple[int, frozenset[int]]:
    """
    Score a candidate checksum function by looking for CMP byte ptr [reg+disp32],0
    accesses to the known state-field neighborhood (+0x130..+0x139).

    This is deliberately a local structural validation after resolving a callee
    from Branch-2, not a whole-file byte signature.
    """
    begin, end = function_range_for_rva(
        candidate_rva,
        text,
        runtime_ranges,
        fallback_before=0,
        fallback_after=0x180,
    )

    try:
        start_off = rva_to_raw_offset(sections, begin)
        end_off = rva_to_raw_offset(sections, end - 1) + 1
    except PatchError:
        return 0, frozenset()

    found: set[int] = set()
    pos = start_off
    while pos + 7 <= end_off:
        # 80 /7 ib == CMP r/m8, imm8.  mod=10 means [base + disp32].
        if data[pos] == 0x80:
            modrm = data[pos + 1]
            mod = (modrm >> 6) & 0x3
            reg = (modrm >> 3) & 0x7
            if mod == 0x2 and reg == 0x7 and data[pos + 6] == 0:
                disp = struct.unpack_from("<I", data, pos + 2)[0]
                if disp in CHECKSUM_STATE_OFFSETS:
                    found.add(disp)
        pos += 1

    return len(found), frozenset(found)


def resolve_branch2_and_checksum(
    data: bytes,
    sections: list[PESection],
    text: PESection,
    runtime_ranges: list[tuple[int, int]],
    predicates: list[int],
) -> tuple[int, int]:
    """
    Identify CanGetAchievements Branch-2 and its checksum callee.

    For each resolved predicate, inspect its direct CALL/JMP rel32 targets.  The
    checksum callee is selected by local structural validation of the target
    function.  This continues to work when Branch-2/checksum were already
    patched at their entries because the original function bodies remain in the
    executable behind the early `mov eax,1; ret`.
    """
    runtime_starts = {begin for begin, _end in runtime_ranges}
    matches: list[tuple[int, int, int, frozenset[int], int]] = []

    for predicate_rva in predicates:
        begin, end = function_range_for_rva(
            predicate_rva,
            text,
            runtime_ranges,
            fallback_before=0,
            fallback_after=0x220,
        )

        seen_targets: set[int] = set()
        for insn_rva, target_rva, opcode in iter_rel32_control_targets(
            data, text, begin, end
        ):
            if target_rva == predicate_rva or target_rva in seen_targets:
                continue
            seen_targets.add(target_rva)

            # Non-leaf checksum has unwind metadata in the current x64 binary.
            # Prefer real runtime-function starts to reject accidental E8/E9
            # bytes occurring inside another instruction's immediate data.
            if runtime_starts and target_rva not in runtime_starts:
                continue

            score, fields = checksum_structure_score(
                data, sections, text, runtime_ranges, target_rva
            )
            if score >= CHECKSUM_MIN_STATE_FIELDS:
                matches.append((predicate_rva, target_rva, score, fields, insn_rva))

    if debug_info:
        for predicate_rva, checksum_rva, score, fields, insn_rva in matches:
            op = "call/jmp"
            print(
                f"Checksum candidate: branch={predicate_rva:#x}, {op}@{insn_rva:#x} "
                f"-> {checksum_rva:#x}, score={score}, fields="
                + ",".join(f"{field:#x}" for field in sorted(fields))
            )

    unique_pairs: dict[tuple[int, int], tuple[int, frozenset[int], int]] = {}
    for predicate_rva, checksum_rva, score, fields, insn_rva in matches:
        key = (predicate_rva, checksum_rva)
        previous = unique_pairs.get(key)
        if previous is None or score > previous[0]:
            unique_pairs[key] = (score, fields, insn_rva)

    if len(unique_pairs) != 1:
        details = (
            ", ".join(
                f"branch {branch:#x} -> checksum {checksum:#x}"
                for branch, checksum in unique_pairs
            )
            or "none"
        )
        raise PatchError(
            "Could not uniquely resolve Branch-2 -> checksum from local call structure; "
            f"candidates: {details}."
        )

    (branch2_rva, checksum_rva), _info = next(iter(unique_pairs.items()))
    return branch2_rva, checksum_rva


def add_return_true_job(
    jobs: list[PatchJob],
    data: bytearray,
    sections: list[PESection],
    label: str,
    target_rva: int,
) -> None:
    offset = rva_to_raw_offset(sections, target_rva)
    if data[offset : offset + len(RETURN_TRUE)] == RETURN_TRUE:
        print(f"{label} already patched at RVA {target_rva:#x}.")
        return
    jobs.append(PatchJob(label, offset, tuple(RETURN_TRUE)))


def prepare_patch_jobs(data: bytearray, sections: list[PESection]) -> list[PatchJob]:
    """
    Resolve every patch from string anchors/call relationships before writing.

    Patch #1: CanGetAchievements Branch-1 -> return true
    Patch #2: CanGetAchievements Branch-2 -> return true
    Patch #3: checksum called by Branch-2 -> return true
    Patch #4: IsGameRuleEnabled callback -> return true
    """
    text = get_section(sections, ".text")
    runtime_ranges = parse_runtime_function_ranges(data, sections)
    jobs: list[PatchJob] = []

    # Resolve the two CanGetAchievements predicates from the stable string anchor.
    can_rvas = find_string_rvas(data, sections, CAN_GET_ACHIEVEMENTS_ANCHOR)
    can_xrefs = find_anchor_xrefs(data, text, can_rvas)
    if len(can_xrefs) < 2:
        raise PatchError(
            f"Expected at least 2 CanGetAchievements registrations, found {len(can_xrefs)}."
        )

    predicates: list[int] = []
    for xref_rva, string_rva in can_xrefs:
        callback_rva = find_registered_callback(
            data, text, runtime_ranges, xref_rva, string_rva
        )
        predicate_rva = resolve_can_get_achievements_predicate(
            data, text, runtime_ranges, callback_rva
        )
        if predicate_rva not in predicates:
            predicates.append(predicate_rva)

    if len(predicates) != 2:
        raise PatchError(
            f"Expected exactly 2 distinct CanGetAchievements predicates, found {len(predicates)}."
        )

    # Determine Branch-2 semantically from its checksum call, not from RVA order.
    branch2_rva, checksum_rva = resolve_branch2_and_checksum(
        data, sections, text, runtime_ranges, predicates
    )
    branch1_candidates = [rva for rva in predicates if rva != branch2_rva]
    if len(branch1_candidates) != 1:
        raise PatchError("Could not uniquely determine CanGetAchievements Branch-1.")
    branch1_rva = branch1_candidates[0]

    if debug_info:
        print(f"CanGetAchievements Branch-1: RVA {branch1_rva:#x}")
        print(f"CanGetAchievements Branch-2: RVA {branch2_rva:#x}")
        print(f"Branch-2 checksum: RVA {checksum_rva:#x}")

    # Important: every target above was resolved from the untouched input image.
    # Only now do we create write jobs.  Patch #3 remains a real function patch,
    # so every other caller of checksum also observes `true`.
    add_return_true_job(
        jobs,
        data,
        sections,
        "Patch #1 (CanGetAchievements Branch-1)",
        branch1_rva,
    )
    add_return_true_job(
        jobs,
        data,
        sections,
        "Patch #3 (checksum via Branch-2)",
        checksum_rva,
    )
    add_return_true_job(
        jobs,
        data,
        sections,
        "Patch #2 (CanGetAchievements Branch-2)",
        branch2_rva,
    )

    # IsGameRuleEnabled directly registers the function we want to force true.
    rule_rvas = find_string_rvas(data, sections, IS_GAME_RULE_ENABLED_ANCHOR)
    rule_xrefs = find_anchor_xrefs(data, text, rule_rvas)
    if not rule_xrefs:
        raise PatchError("No IsGameRuleEnabled registration xref was found.")

    callbacks: list[int] = []
    for xref_rva, string_rva in rule_xrefs:
        callback_rva = find_registered_callback(
            data, text, runtime_ranges, xref_rva, string_rva
        )
        if callback_rva not in callbacks:
            callbacks.append(callback_rva)

    if len(callbacks) != 1:
        raise PatchError(
            f"Expected 1 IsGameRuleEnabled callback, found {len(callbacks)}: "
            + ", ".join(f"{rva:#x}" for rva in callbacks)
        )

    add_return_true_job(
        jobs,
        data,
        sections,
        "Patch #4 (IsGameRuleEnabled via string xref)",
        callbacks[0],
    )

    return jobs


def make_patch(filepath: Path) -> None:
    """Patch the target executable."""
    try:
        data = bytearray(filepath.read_bytes())
    except OSError as e:
        raise PatchError(f"Failed to read file: {e}") from e

    sections = parse_pe_sections(data)

    # Resolve every target from the untouched image before any bytes are changed.
    # Patch #1/#2/#3 share the CanGetAchievements anchor/call chain; #4 uses its own anchor.
    patch_jobs = prepare_patch_jobs(data, sections)

    if not patch_jobs:
        print("\nAll patches are already applied.")
        return

    # Only create a backup after every required target has been resolved successfully.
    create_backup(filepath, filepath.with_name(filepath.name + EU5_BACKUP_SUFFIX))

    for job in patch_jobs:
        print(f"\n{job.label} found at offset: {job.offset:#x}")
        if debug_info:
            print(f"Applying {job.label}...\n")
        apply_patch(data, job.offset, job.replacement)

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
