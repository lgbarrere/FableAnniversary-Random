# FableAnniversary-Random
A randomizer project for **Fable 1** handled by Archipelago. This is a work in progress.
**Fable 1** refers to both **Fable - The Lost Chapters** and **Fable Anniversary** in this project.

## Disclaimer
This project provides an open‑source modding tool and original mod scripts.
**You must own a legitimate copy of Fable 1**.
This repository does **not** include any game files, textures, audio, executables, or other copyrighted material from the game.
The tool works by modifying files on your local installation that you supply.
Use at your own risk.

## Recommendations
> [!TIP]
> For a better understanding of how to setup this project, some paths in this documentation are replaced by keywords as follows and should be known:
* **\<ProjectFolder\>** is the path to this project once it is downloaded, for instance **D:/Modding/FableAnniversary-Random/**
* **\<GameFolder\>** is the path to folder containing all the files necessary to launch the game, for instance **D:/Program Files (x86)/Steam/steamapps/common/Fable The Lost Chapters/**

## Mod Installation for playing
### Mod Download
**This section is a work in progress and is not functional, the following steps cannot be applied yet (TODO: remove this line once the mod works).**

Make sure the [last Archipelago release](https://github.com/ArchipelagoMW/Archipelago/releases) is installed to have the best randomization logic. 
All the required files of the Mod can be found in the [Releases](https://github.com/lgbarrere/FableAnniversary-Random/releases).
To use the Mod, download and unzip **Fable1_Random.zip** somewhere safe, then follow the instructions in the next sections of this guide. This archive contains:
* The **mods/** folder containing the Randomizer Mod
* The **dinput8.dll** file loading the Mod itself, it runs the code handling all the randomized elements
* A default **Fable1.yaml** file to define the player options, but the recommended way is the **Player Options page** (TODO: link to provide once ready) instead
* The **Fable1.apworld** file containing the randomization logic, it is already installed by [Archipelago 0.6.5 at least](https://github.com/ArchipelagoMW/Archipelago/releases) (TODO: change this version once the game is truly deployed)

### Game Setup
**This section is a work in progress and is not functional, the following steps cannot be applied yet (TODO: remove this line once the mod works).**

The Mod can be installed and played by following these steps (see the [Mod Download](https://github.com/lgbarrere/FableAnniversary-Random#mod-download) section to get **Fable1_Random.zip**):
1. Copy the **mods/** folder and **dinput8.dll** from **Fable1_Random.zip** to **\<GameFolder\>** (where the **Fable.exe** is placed)
2. Launch the game, if "OFFLINE" is visible in the upper-right corner of the screen, the Mod is working

### Create a Config (.yaml) File
> [!NOTE]
> The purpose of a YAML file is described at the [Basic Multiworld Setup Guide](https://archipelago.gg/tutorial/Archipelago/setup/en#generating-a-game).

No option is currently taken into account by the Mod, this is a work in progress.

The YAML file can be generated using the **Player Options page** (TODO: link to provide once the apworld is deployed on Archipelago), which is the recommended way.

However, a default YAML can be used while the options are not implemented, see the [Mod Download](https://github.com/lgbarrere/FableAnniversary-Random#mod-download) section to get **Fable1.yaml**.

The player name configured by this default YAML is "Fable1", it can be changed by renaming both the YAML and the "name" section inside of this YAML, **both must be identical**.

## Joining a MultiWorld Game
**TODO: fill out this section when possible**

> [!TIP]
> For a local single-player session, the \<IP\> must be ``localhost:38281``

## Hosting a MultiWorld game
See the [Mod Download](https://github.com/lgbarrere/FableAnniversary-Random#mod-download) section to get a YAML file for **Fable 1**.

### Hosting official games only
**This section is a work in progress and is not functional, the following steps cannot be applied yet (TODO: remove this line once the mod works).**

Follow these steps to host a remote session:
1. Go to the [Archipelago GENERATE GAME page](https://archipelago.gg/generate)
2. Configure the game rules, click "Upload File" and select the YAML of each player to host
3. Go to the [Archipelago HOST GAME page](https://archipelago.gg/uploads)
4. Click "Upload File" and select the generated **AP_\<seed\>.zip** by the previous step
5. Send the link of the generated room page to each player

### Hosting some unofficial games or using single-player localhost
**This section is a work in progress and is not functional, the following steps cannot be applied yet (TODO: remove this line once the mod works).**

In this section, **Archipelago/** refers to the path where [Archipelago](https://github.com/ArchipelagoMW/Archipelago/releases) is installed locally.

Follow these steps to host a remote session:
1. Double-click the unofficial **.apworld** files to automatically install their randomization logic
2. Put the YAML of each player to host in **Archipelago/Players/**
3. Launch the Archipelago launcher and click "Generate" to configure a game in **Archipelago/output/**
4. For a multiplayer session, go to the [Archipelago HOST GAME page](https://archipelago.gg/uploads)
5. Click "Upload File" and select the generated **AP_\<seed\>.zip** in **Archipelago/output/**
6. Send the link of the generated room page to each player
> [!TIP]
> For a local single-player session, click "Host" in the Archipelago launcher by using the generated **AP_\<seed\>.zip** in step 3

## Mod installation for development
### Project Architecture
> [!TIP]
> Before setting this project up for development, it is highly recommended to know how this project is structured.
1. **dinput8/dinput8/** contains the C++ code to load a mod, used to build dinput8.dll which loads the mod at game start (the entry point is dllmain.cpp)
2. **dinput8/Release/** which is the folder containing the resulting dinput8 files, dinput8.dll is the dll to use as mod loader

### Requirements
1. A computer running Windows
2. [Microsoft Visual Studio Community](https://visualstudio.microsoft.com/fr/vs/community/) 2019 (prefer recent versions) or 2022
3. A legal copy of **Fable 1** original game (can be found on [Steam](https://store.steampowered.com/app/288470/Fable_Anniversary/))
4. Clone this project with ``git clone https://github.com/lgbarrere/FableAnniversary-Random.git``

### How to use and build the project
#### Build the DLL mod loader
There are 2 ways to build **dinput8.dll** that loads the mods from ``<GameFolder>/mods/``, the first one is recommended:
1. Run **deploy.bat** to build and copy **dinput8.dll** in ``<GameFolder>/`` automatically
2. Generate **dinput8.dll** manually with **Microsoft Visual Studio Community** using ``Build > Build Solution`` (ctrl + shift + B)

> [!NOTE]
> The second option requires to copy **dinput8.dll** in ``<GameFolder>/`` by hand

#### Injection of the Mod in the game
The **deploy.bat** script has been made to facilitate the injection of the mods.
> [!IMPORTANT]
> This script should be updated with your own paths corresponding to your project and game setups.
> Follow the comments in this script to fill it properly.

### Contributions to the randomization logic
> [!NOTE]
> In this section, ``<apworld>`` refers to the ``fable1`` folder, this is the general name to describe a world randomization logic.

The randomization can be found in the **forked Archipelago project** (TODO: link to provide once ready), the apworld folder is named "fable1".
It is recommended to read the [AP documentations](https://github.com/ArchipelagoMW/Archipelago/tree/main/docs) before contributing to the logic.

After implementing and testing the ``<apworld>``, package it and name it "fable1.apworld" (the archipelago logo should be displayed) to prepare the apworld for the releases.
Double-clicking this "fable1.apworld" should install the ``<apworld>`` automatically, ready to be played.

## Planned updates
**TODO: fill out this section when possible**

## Known Issues
### Major issues
No major issue found.

### Minor issues
No minor issue found.


