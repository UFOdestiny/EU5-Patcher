<div align="center">

# 🏆 EU5 Patcher

### Enable Achievements Unconditionally

[中文](README_CN.md) | [English](README.md)

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)]()
[![Game](https://img.shields.io/badge/game-Europa%20Universalis%20V-orange.svg)]()

</div>

---

## 📖 About

The debate over whether **unmodified ironman mode** should be required to unlock achievements has been going on for years. While games like *Crusader Kings III* and *Stellaris* have adopted a more player-friendly approach, *Europa Universalis V* has unfortunately taken a step back.

This patcher allows you to:
- Enable **all achievements** in non-ironman mode.
- Enable **all game rules** in non-ironman mode.

| Mode        | Mod   | Setting | Console | Save & Load | Achievements |
| ----------- | ----- | ------- | ------- | ----------- | ------------ |
| Non-Ironman | ✅ Any | ✅ Any   | ✅ Yes   | ✅ Yes       | ✅ Yes        |


<div align="center">
<img src="Effect_Achi.png" alt="Achievement Effect" width="auto"/>
</div>

---
## 🚀 How to Use
> [!TIP]
> You will need to patch `eu5.exe` again after every game update.

### 🐍 Option 1: Python Script

You can run the script from any location.

```bash
python patch.py
```

### ⚙️ Option 2: Compile from Source (C++)

```bash
# 1. Compile the source code
cl /std:c++17 /O2 /EHsc patch.cpp

# 2. Run the generated patch.exe
patch.exe
```

### ⚠️ Option 3: Pre-built Executable

> [!WARNING]
> Running unknown executables carries risks. Only proceed if you trust the source.

1. Download `patch.exe` from the [📦 Releases page](https://github.com/UFOdestiny/EU5-Patcher/releases/).
2. Run `patch.exe`.

---

## ✅ Post-Patching

If successful, you will see the message:
```
Path: "E:\\\\SteamLibrary\\steamapps\\common\\Europa Universalis V\\binaries\\eu5.exe"
Backup created: "E:\\\\SteamLibrary\\steamapps\\common\\Europa Universalis V\\binaries\\eu5.exe.backup"

Patch #1 (CanGetAchievements Branch-1) found at offset: 0x1a1360 (RVA 0x1a1f60)

Patch #2/#3 (registered manager eligibility) found at offset: 0x3086230 (RVA 0x3086e30)

Patch #4 (IsGameRuleEnabled predicate) found at offset: 0x4d72f20 (RVA 0x4d73b20)

Patch #5 (Native achievement update gate) found at offset: 0x604ba1f (RVA 0x604c61f)

Patch #6 (Native eligibility boolean gate) found at offset: 0x604becd (RVA 0x604cacd)

Patch #7 (Native eligibility boolean gate) found at offset: 0x9c10e96 (RVA 0x9c11a96)

Patch #8 (Native eligibility boolean gate) found at offset: 0x9c11d43 (RVA 0x9c12943)

EU5 is successfully patched.
Press Enter to exit...
```

---

## 📚 How it works？

```text
"CanGetAchievements"
    -> Registration #1 -> callback -> predicate -> Patch #1
    -> Registration #2 -> callback -> predicate -> Patch #2
                                     |
                                     +-> manager eligibility -> Patch #3
                                         (can be the predicate itself)

"IsGameRuleEnabled"
    -> registration -> callback -> boolean callee -> Patch #4
                       (preserve output marshalling)

Native eligibility predicate / update / list initialization / list refresh
    -> validated allowed branches -> Patches #5-#8

B8 01 00 00 00 C3     mov eax, 1; ret  (#1-#4)
75 -> EB             take existing true branch (#6-#8)
0F 84 ... -> NOPs    enter normal update evaluation (#5)
```

The GUI registration shares an entry with a copy of the manager eligibility predicate. The shared native predicate and three inlined checks also need coverage. The historical checksum label referred to six eligibility fields. Result marshalling and individual achievement possible/happened triggers still execute normally. Every site is validated before backup and only its replacement bytes are written; incomplete resolution aborts.

---

## 🧪 Regression checks

```powershell
# Synthetic PE tests covering old and new registration layouts
python -m unittest discover -s tests -v

# Also check C++ and the real game image, writing only temporary copies
$env:EU5_CPP_PATCHER = (Resolve-Path .\patch.exe).Path
$env:EU5_TEST_EXE = 'E:\SteamLibrary\steamapps\common\Europa Universalis V\binaries\eu5.exe.backup'
python -m unittest discover -s tests -v
```

---

## 🙌 Credits

This project was created for educational purposes. Inspired by:
- [Enabling Achievements in Stellaris With Mods (All game versions) [SRE]](https://steamcommunity.com/sharedfiles/filedetails/?id=2460079052)