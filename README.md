 > This project uses AI-generated code.

<p align="center">
  <img src="https://github.com/user-attachments/assets/fad469d4-b7b2-428c-a093-5b497f02d820" width="500" />
</p>

## Features
- ToPBR: Automatically converts Source Engine materials to PBR at runtime
- Hardware Skinning (For Half-Life 2 RTX Assets)
- Remix API Support (including Lua bindings for addon creation)
      
## Installation
- Switch to the `x86-64` branch of Garry's Mod on Steam
- Ensure you've ran the vanilla game at least once, especially if you switched branches.
- Download [RTXLauncher](https://github.com/Xenthio/RTXLauncher/releases) (Use the newer nightly if available).
- **Windows**
   - Run the launcher (**as an Administrator**)
- **Linux**
   - Ensure Steam Play is forced for Garry's Mod with either Proton Experimental or another recent Proton build.
   - Run [GModPatchTool](https://github.com/solsticegamestudios/GModPatchTool)
   - Make sure it's marked as executable `chmod +x ./RTXLauncher.Avalonia.Linux`, and run it.

- Select the Release Channel (Stable or Nightly)
- Select `Start Quick Install` on the main screen and follow the prompts when asked.
- Once it's finished, press `Launch Game` at the bottom of the launcher.

## Multiplayer
Multiplayer works best when the server/host has this addon and the cvar `sv_allowcslua 1` set.

You can join servers without the addon but you ***will*** experience visual issues.

## Support
### [Problematic Addons](https://github.com/Xenthio/garrys-mod-rtx-remixed/wiki/Problematic-Addons)
### [Common issues](https://github.com/Xenthio/garrys-mod-rtx-remixed/wiki/Common-Issues)

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
