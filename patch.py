import os
import re
import shutil

PATTERN = "80 ?? ?? ?? ?? ?? 00 75 ?? 80 ?? ?? ?? ?? ?? 00 74 ?? 80 ?? ?? ?? ?? ?? 00 74 ?? 80 ?? ?? ?? ?? ?? 00"
PATTERN_REPLACE = "80 ?? ?? ?? ?? ?? 00 eb"

EU5_PATH = "eu5.exe"
EU5_BACKUP_PATH = "eu5.exe.backup"


def pattern_to_regex(pattern_str):
    parts = pattern_str.split()
    regex_bytes = b""
    count_any = 0

    for part in parts:
        if part == "??":
            count_any += 1
        else:
            if count_any > 0:
                regex_bytes += b".{" + str(count_any).encode() + b"}"
                count_any = 0
            regex_bytes += bytes.fromhex(part)

    if count_any > 0:
        regex_bytes += b".{" + str(count_any).encode() + b"}"

    return re.compile(regex_bytes, re.DOTALL)


def pattern_to_list(pattern_str):
    res = []
    for i in pattern_str.split():
        if i == "??":
            res.append(i)
        else:
            res.append(hex(int(i, 16)))
    return res


def make_patch(filename):

    # Make Backup
    shutil.copy2(EU5_PATH, EU5_BACKUP_PATH)
    print(f"Backup is made: {EU5_BACKUP_PATH}")

    # Read
    with open(filename, "rb") as f:
        data = bytearray(f.read())

    # Match
    regex = pattern_to_regex(PATTERN)
    matches = [m.start() for m in regex.finditer(data)]
    if len(matches) != 1:
        print(
            "Fail: The pattern is not founded. Have you patched it before? "
            "If no, the pattern needs to update."
        )
        exit()
    index = matches[0]

    # Replace
    replace_parts = pattern_to_list(PATTERN_REPLACE)
    for i in range(len(replace_parts)):
        if replace_parts[i] == "??":
            print(f"{index+i}: {hex(data[index + i])} \t-> {hex(data[index + i])}")
        else:
            print(f"{index+i}: {hex(data[index + i])} \t-> {replace_parts[i]}")
            data[index + i] = int(replace_parts[i], 16)

    # Write Back
    with open(filename, "wb") as f:
        f.write(data)
        print("EU5 is successfully patched.")


def main():

    # Check Path
    if not os.path.exists(EU5_PATH):
        print(
            "eu5.exe is not found. "
            "Put this file in .../Europa Universalis IV/binaries/"
        )
        exit(0)

    # Patch
    make_patch(EU5_PATH)

    print("Press any key to exit...")
    input()


if __name__ == "__main__":
    main()
