# EU5 Patcher to Enable Achievements Unconditionally
The debate over whether Ironman mode should be required to unlock achievements has been going on for a long time. 
Crusader Kings III and Stellaris set a good example, but Europa Universalis V took a step backward. 
So I decided to make this patch, which enables the achievement system even in **non-ironman mode**, **with mods**, and under **any game settings**.   

There are some bugs that may prevent achievements unlock in non-ironman mode and I'm working on that. If you use ironman mode, it is totally fine.

![alt text](figures/effect.png)
# How to use it?
> [!TIP]
> You have to patch eu5.exe after each game update.

## Python
0. Install Python and ensure the path is configured correctly.
1. Put this `patch.py` in the `.../Europa Universalis IV/binaries/`, where `eu5.exe` exists.
2. Open `cmd` in this folder, and use the command `python patch.py`.

## C++
1. Download `patch.cpp`, and compile it with command `cl /std:c++17 /O2 /EHsc patch.cpp`.
2. Put the exe in the `.../Europa Universalis IV/binaries/`, where `eu5.exe` exists. Run it.

## EXE (NOT RECOMMENDED, AT YOUR OWN RISK)
> [!WARNING]
> I do not recommend you to download and run an unknown exe if you are not 100% sure it is safe.

1. Download `patch.exe` from [Releases](https://github.com/UFOdestiny/EU5-Patcher/releases/tag/exe).
2. Put the exe in the `.../Europa Universalis IV/binaries/`, where `eu5.exe` exists. Run it.  


You'll see the message “EU5 is successfully patched” if the process completes successfully. After that, you can start a new game with any mods or settings in non-ironman mode. You might notice that the trophy icon appears red in the settings menu at first — just start the game, and it will turn green as shown in the figure.



# Tutorial
WIP
![alt text](figures/position.png)
# Credits
This patcher is mainly for learning purposes and honing skills, which is based on:
- [Enabling Achievements in Stellaris With Mods (All game versions) [SRE]](https://steamcommunity.com/sharedfiles/filedetails/?id=2460079052)
