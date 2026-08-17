<div align="center">

# 🏆 EU5 Patcher

### 无条件开启成就

[中文](README_CN.md) | [English](README.md)

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)]()
[![Game](https://img.shields.io/badge/game-Europa%20Universalis%20V-orange.svg)]()

</div>

---

## 📖 简介

关于是否应该强制要求使用**未修改的铁人模式**才能解锁成就的争论已经持续多年。虽然《十字军之王 III》和《群星》等游戏已经采取了对玩家更加友好的方式，但《欧陆风云 V》遗憾地在这一点上倒退了一步。

这个补丁可以让你：

* 在非铁人模式下开启**所有成就**。
* 在非铁人模式下启用**所有游戏规则**。

| 模式       | Mod    | 设置   | 控制台 | 存档 & 读档 | 成就   |
| ---------- | ------ | ------ | ------ | ----------- | ------ |
| 非铁人模式 | ✅ 任意 | ✅ 任意 | ✅ 开启 | ✅ 开启      | ✅ 开启 |

<div align="center">
<img src="Effect_Achi.png" alt="效果图" width="auto"/>
</div>

---

## 🚀 如何使用

> [!TIP]
> 每次游戏更新后，都需要重新对 `eu5.exe` 打补丁。

### 🐍 方法 1：Python 脚本

你可以在任意位置运行该脚本。

```bash
python patch.py
```

### ⚙️ 方法 2：从源码编译（C++）

```bash
# 1. 编译源代码
cl /std:c++17 /O2 /EHsc patch.cpp

# 2. 运行生成的 patch.exe
patch.exe
```

### ⚠️ 方法 3：预编译可执行文件

> [!WARNING]
> 运行未知来源的可执行文件存在风险。请仅在你信任该来源的情况下继续。

1. 从 [📦 Releases 页面](https://github.com/UFOdestiny/EU5-Patcher/releases/) 下载 `patch.exe`。
2. 运行 `patch.exe`。

---

## ✅ 打补丁后

如果补丁应用成功，你将看到类似以下信息：

```text
Path: "E:\\\\SteamLibrary\\steamapps\\common\\Europa Universalis V\\binaries\\eu5.exe"
Backup created: "E:\\\\SteamLibrary\\steamapps\\common\\Europa Universalis V\\binaries\\eu5.exe.backup"

Patch #1 (CanGetAchievements Branch-1) found at offset: 0x729bb0 (RVA 0x72a7b0)

Patch #3 (checksum via Branch-2) found at offset: 0x1f6ae70 (RVA 0x1f6ba70)

Patch #2 (CanGetAchievements Branch-2) found at offset: 0x3fa7480 (RVA 0x3fa8080)

Patch #4 (IsGameRuleEnabled via string xref) found at offset: 0x786810 (RVA 0x787410)

EU5 is successfully patched.
Press Enter to exit...
```

---

## 📚 工作原理

```text
"CanGetAchievements"
        │
        └─ RIP-relative XREF
            │
            ├─ Registration #1
            │   └─ Callback
            │       └─ Predicate
            │           └─ Patch #1 → return true
            │
            └─ Registration #2
                └─ Callback
                    └─ Predicate
                        │
                        ├─ 识别为 Branch-2
                        │
                        ├─ Patch #2 → return true
                        │
                        └─ call/jmp
                            └─ checksum()
                                └─ Patch #3 → return true


"IsGameRuleEnabled"
        │
        └─ RIP-relative XREF
            └─ Registration
                └─ Callback
                    └─ Predicate
                        └─ Patch #4 → return true


补丁结果：
    B8 01 00 00 00 C3
    mov eax, 1
    ret
```

---

## 🙌 致谢

本项目主要用于学习和教育目的。灵感来源于：

* [Enabling Achievements in Stellaris With Mods (All game versions) [SRE]](https://steamcommunity.com/sharedfiles/filedetails/?id=2460079052)
