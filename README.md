 > This project uses AI-generated code.

> [!CAUTION]
> # This project is now deprecated and is no longer being updated, please use [SourcePT](https://github.com/sambow23/SourcePT) instead.

<p align="center">
  <img src="https://github.com/user-attachments/assets/fad469d4-b7b2-428c-a093-5b497f02d820" width="500" />
</p>

## Features
- ToPBR: Automatically converts Source Engine materials to PBR at runtime
- Hardware Skinning (For Half-Life 2 RTX Assets)
- Remix API Support (including Lua bindings for addon creation)
- [Prepared PBR assets in GMA addons](docs/PBR_GMA_LOADER.md): prepare DDS replacements before renderer startup and reuse them on later launches (Windows x64).
- Win64 legacy texture-hash replacement provider for Advanced Material Editor
  ([protocol and safety notes](source/advmat_rtx_bridge/README.md))

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
