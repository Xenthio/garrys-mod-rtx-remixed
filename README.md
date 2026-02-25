 > This project uses AI-generated code in various areas of the codebase.

<img src="https://github.com/user-attachments/assets/fad469d4-b7b2-428c-a093-5b497f02d820" alt="drawing" width="500"/>

## Features
- Material fixes
    - Fixes some broken UI/game materials and removes detail textures
    - Change all water textures to a single one to simplify replacements in Remix
    - **Runtime ToPBR Conversion**: Automatically converts Source Engine materials to PBR at runtime:
        - Reads VTF texture files directly from Source Engine filesystem
        - Writes PBR textures (normal, roughness, metallic) to `rtx-remix/mods/~gmod_topbr/textures/`
        - Generates roughness textures from `$envmapmask` or `$phongexponent`
        - Estimates metallic from `$phongboost` values
        - Auto-processes materials on map load
- Model fixes
    - Hardware skinning
    - Fixes some props having unstable hashes in RTX Remix so they can be replaced in the Remix Toolkit
    - Allows most HL2 RTX mesh replacements to load correctly
- Remix API Support (x64 only)
    - Lights
    - Lua bindings for addon creation
    - Map-specific Remix settings
      

## Installation
- Switch to the `x86-64` branch of Garry's Mod on Steam (x32 works but not recommended)
- Ensure you've ran the vanilla game at least once, especially if you switched branches.
- Download [RTXLauncher](https://github.com/Xenthio/RTXLauncher/releases/latest).
- **Windows**
   - Put `RTXLauncher.Avalonia.Windows.exe` in an empty folder, run it as an <ins>**Administrator**</ins>
   - Do not place in the same place as your vanilla game
   - Do not place it in a OneDrive synced folder (Documents, Desktop, etc), the game will not work correctly if you do so
- **Linux**
   - Ensure Steam Play is forced for Garry's Mod with either Proton Experimental or another recent Proton build.
   - Run [GModPatchTool](https://github.com/solsticegamestudios/GModPatchTool)
   - Put `RTXLauncher.Avalonia.Linux` in an empty folder, run it via the terminal or double click it (make sure it's marked as executable `chmod +x ./RTXLauncher.Avalonia.Linux`)

- Select the Fixes package type:
  - `garrys-mod-rtx-remixed` (this repo, normal version)
  - [`garrys-mod-rtx-remixed-perf`](https://github.com/sambow23/garrys-mod-rtx-remixed-perf) (performance focused version)
- Select `Start Quick Install` on the main screen and follow the prompts when asked.
- Once it's finished, press `Launch Game` at the bottom of the launcher.

## Multiplayer
Multiplayer works best when the server/host has this addon and the cvar `sv_allowcslua 1` set.

You can join servers without the addon but you ***will*** experience visual issues.

## Support
### [Problematic Addons](https://github.com/Xenthio/garrys-mod-rtx-remixed/wiki/Problematic-Addons)
### [Known issues](https://github.com/Xenthio/garrys-mod-rtx-remixed/wiki/Known-Issues)

## Recommended Resources
[HDRI Editor](https://github.com/sambow23/hdri_cube/blob/main/README.md)

## Credits
* [vlazed](https://github.com/vlazed/) for [toggle-cursor](https://github.com/vlazed/toggle-cursor)
* Yosuke Nathan on the RTX Remix Showcase server for making the initial `Garry's Mod Remixed` logo
* Everyone on the RTX Remix Showcase server
* NVIDIA for RTX Remix
* [Nak2](https://github.com/Nak2) for [NikNaks](https://github.com/Nak2/NikNaks)
* [BlueAmulet](https://github.com/BlueAmulet) for [SourceRTXTweaks](https://github.com/BlueAmulet/SourceRTXTweaks)
* [0xNULLderef](https://github.com/0xNULLderef) and [Wolƒe Strider Shoσter](https://github.com/wolfestridershooter) for additional x64 patches (culling and HDR map lighting)
* [King David](https://github.com/KingDavidW) for all of their guidance for getting hardware skinning working.
