@echo off
echo Enter the full path to your gmod install (where gmod.exe is) (no trailing backslash)
echo Make sure you've built the debug build atleast once before running this
set /p gmodpath="Game Path: "

if exist "%gmodpath%\garrysmod\addons\remixbinary" rmdir /s /q "%gmodpath%\garrysmod\addons\remixbinary"
mklink /j "%gmodpath%\garrysmod\addons\remixbinary" ".\garrysmod\garrysmod\addons\remixbinary"
echo Addon Linked.

if exist "%gmodpath%\bin\win64\stdshader_dx6.dll" (
    :: If it's a symlink, just delete it. If it's a real file, back it up.
    fsutil reparsepoint query "%gmodpath%\bin\win64\stdshader_dx6.dll" >nul 2>&1
    if errorlevel 1 (
        echo Backing up original stdshader_dx6.dll...
        move /y "%gmodpath%\bin\win64\stdshader_dx6.dll" "%gmodpath%\bin\win64\stdshader_dx6.dll.bak"
    ) else (
        del /f /q "%gmodpath%\bin\win64\stdshader_dx6.dll"
    )
)
mklink "%gmodpath%\bin\win64\stdshader_dx6.dll" "%cd%\garrysmod\bin\win64\stdshader_dx6.dll"
echo Shader linked.

if exist "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.dll" del /f /q "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.dll"
if exist "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.exp" del /f /q "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.exp"
if exist "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.pdb" del /f /q "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.pdb"
if exist "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.dll" del /f /q "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.dll"
if exist "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.exp" del /f /q "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.exp"
if exist "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.pdb" del /f /q "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.pdb"

mklink "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.dll" "%cd%\x86_64\Debug\gmcl_RTXFixesBinary_win64.dll"
mklink "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.exp" "%cd%\x86_64\Debug\gmcl_RTXFixesBinary_win64.exp"
mklink "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win64.pdb" "%cd%\x86_64\Debug\gmcl_RTXFixesBinary_win64.pdb"
mklink "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.dll" "%cd%\x86\Debug\gmcl_RTXFixesBinary_win32.dll"
mklink "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.exp" "%cd%\x86\Debug\gmcl_RTXFixesBinary_win32.exp"
mklink "%gmodpath%\garrysmod\lua\bin\gmcl_RTXFixesBinary_win32.pdb" "%cd%\x86\Debug\gmcl_RTXFixesBinary_win32.pdb"
echo Binary Linked.
pause
