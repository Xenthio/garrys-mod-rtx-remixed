@echo off
echo Enter the full path to your gmod install (where gmod.exe is) (no trailing backslash)
echo Make sure you've built the debug build atleast once before running this
set /p gmodpath="Game Path: "
mklink /j "%gmodpath%\garrysmod\addons\remixbinary" ".\garrysmod\garrysmod\addons\remixbinary"
echo Addon Linked.
mklink %gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.dll %cd%\x86_64\Debug\gmcl_RTXFixesBinary_win64.dll
mklink %gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.exp %cd%\x86_64\Debug\gmcl_RTXFixesBinary_win64.exp
mklink %gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.pdb %cd%\x86_64\Debug\gmcl_RTXFixesBinary_win64.pdb
mklink %gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.dll %cd%\x86\Debug\gmcl_RTXFixesBinary_win32.dll
mklink %gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.exp %cd%\x86\Debug\gmcl_RTXFixesBinary_win32.exp
mklink %gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.pdb %cd%\x86\Debug\gmcl_RTXFixesBinary_win32.pdb
echo Binary Linked.
pause
