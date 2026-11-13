MP3SkipCutscenes - cutscene skipper for Max Payne 3 (PC)
=========================================================

Works with the Steam and the Rockstar Games Launcher versions: the mod has no Steam-specific code, it finds
what it needs in MaxPayne3.exe by byte signatures. Made for and tested on MaxPayne3.exe 1.0.0.276 (Steam).
The Rockstar Games Launcher executable has NOT been tested by the author - check the log after the first start.

What it does
  - Space skips cutscenes, including the ones the game locks ("STILL LOADING"): the level is loaded first,
    then the scene is ended, so the game does not crash.
  - One press skips the whole chain of cutscenes up to gameplay.
  - Space ends kill cameras (except the scripted ones where the shot has to land).
  - Game files on disk are not modified.

Install
  1. Close the game.
  2. Open the folder that contains MaxPayne3.exe:
       Steam:     ...\steamapps\common\Max Payne 3\Max Payne 3
       Rockstar:  the install folder shown in Rockstar Games Launcher -> Settings -> My installed games
                  -> Max Payne 3 -> Open (usually C:\Program Files\Rockstar Games\Max Payne 3)
  3. Copy there:
       MP3SkipCutscenes.asi
       MP3SkipCutscenes.ini
       dinput8.dll            <- ASI loader. SKIP this file if you already have a dinput8.dll there
                                 (Ultimate ASI Loader or another ASI loader) - keep yours.
  4. Start the game the usual way (Steam or Rockstar Games Launcher).
  5. Open MP3SkipCutscenes.log next to MaxPayne3.exe. A working install ends the startup part with
       hooks installed (... patches ...)
       ready
     If it says "NOT FOUND" / "unsupported executable", your MaxPayne3.exe differs and the mod left the game
     untouched - send the log to the author.

Use
  Space (or the game's own skip button) during a cutscene. Skipping starts after the first second of a scene.
  Settings: MP3SkipCutscenes.ini (key, tail length, chain window, kill cams).

Uninstall
  Delete MP3SkipCutscenes.asi, MP3SkipCutscenes.ini, MP3SkipCutscenes.log.
  If you copied dinput8.dll from this package and use no other .asi mods: delete dinput8.dll and MP3AsiLoader.log.
  Nothing else is changed anywhere.

Notes
  - Do not use together with MaxPayne3.FusionFix's CutsceneSkip or the old ScriptHook "SkipCutscenes.asi".
  - Transition movies between chapters still take their ~6 seconds (the game's scripts hold them; being studied).
  - Single player only.
  - Needs Windows 10 or 11.

License: GPL-3.0-or-later (see LICENSE). Based on the cutscene skip of MaxPayne3.FusionFix by ThirteenAG
(https://github.com/ThirteenAG/MaxPayne3.FusionFix). Full source code is in source.zip.
