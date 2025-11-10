# 🏆 EU5 Patcher — Enable Achievements Unconditionally

The debate over whether **Ironman mode** should be required to unlock achievements has been going on for years. While *Crusader Kings III* and *Stellaris* took a player-friendly approach, *Europa Universalis V* unfortunately stepped backward. Therefore,  this patch removes those restrictions — enabling the achievement system in:
- **Non-Ironman mode**
- **Modded games**
- **Any game settings**

⚠️ Some bugs may still prevent achievements from unlocking in non-Ironman mode. If you use ironman mode, everything works as normal.


![alt text](figures/effect.png)
## 🚀 How to Use
> [!TIP]
> You’ll need to patch `eu5.exe` after every game update.

### 🐍 Option 1: Python
0. Install Python and ensure the path is configured correctly.
1. Put this `patch.py` in the `.../Europa Universalis IV/binaries/`, where `eu5.exe` exists.
2. Open `cmd` in this folder, and use the command `python patch.py`.

### ⚙️ Option 2: C++
1. Download `patch.cpp`, and compile it with command `cl /std:c++17 /O2 /EHsc patch.cpp`.
2. Put the exe in the `.../Europa Universalis IV/binaries/`, where `eu5.exe` exists. Run it.

### ⚠️ Option 3: EXE (NOT RECOMMENDED, AT YOUR OWN RISK)
> [!WARNING]
> Running unknown executables is risky. Only proceed if you fully trust the source.

1. Download `patch.exe` from [Releases page](https://github.com/UFOdestiny/EU5-Patcher/releases/tag/exe).
2. Put the exe in the `.../Europa Universalis IV/binaries/`, where `eu5.exe` exists. Run it.  

### ✅ After Patching
If everything goes well, you’ll see `EU5 is successfully patched`.
You might notice the trophy icon appears red in the settings menu at first; simply start the game, and it will turn green as shown in the figure.



## 📘 Tutorial
WIP
![alt text](figures/position.png)
## 🙌 Credits
This project was created primarily for learning and skill development. It was inspired by:
- [Enabling Achievements in Stellaris With Mods (All game versions) [SRE]](https://steamcommunity.com/sharedfiles/filedetails/?id=2460079052)
