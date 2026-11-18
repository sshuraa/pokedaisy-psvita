# PokeDaisy for the PS Vita (early test build)

A homebrew port of PokeDaisy to the PS Vita: GBA, Game Boy and Game Boy Color games on mGBA, with the companion on
the Vita's one screen the way the Android app has it on a one-screen device: the library is one page, a game fills the
screen, and the companion opens over it - a WINDOW over the dimmed game (the default) or a SIDE PANEL beside it
(SETTINGS > SCREEN > COMPANION VIEW). Behind everything the theme's animated background (the app's DaisyBackdrop;
SETTINGS > SCREEN > BACKGROUND turns it off).

It reuses the 3DS port's portable C (the companion in `3ds/core/`, the running game in `3ds/host/`, the field features in
`linux/` and the app's `pk_*.c`), with a one-screen mode added to it (`pd_library_screen`, `pd_session.oneScreen`, the
`oneScreen` settings rows) that leaves the 3DS's two screens as they were. This folder is only the Vita's side: the
screen (`video.c`, and `backdrop.c` with its colours and motifs in `themes_gen.c`, made by `tools/gen_themes.py` from
the app's Kotlin), the sound (`audio.c`), the buttons, touch screen and files (`main.c`), the log (`host.c`).

**Status: v0.10, a test build.** FireRed runs at full speed with clean sound on real hardware; the one-screen layout,
the OVERLAY frames and EXPAND are confirmed there. v0.10 adds transitions (library pages slide, menus fade, the
companion's tabs slide and its pages zoom, as the app's AnimatedContent), the game's own menu click (CLICK SOUND: its
SE_SELECT rendered from the ROM the first time a game is played, as the app does), wider frames, a MILES theme (the
Vita's own: white with orange accents), and a CPU meter per core (game / sound + EXPAND / renderer). Its whole flow also
runs in the desktop harness below.

## Installing

1. A Vita (or PS TV) with HENkaku / Ensō and VitaShell.
2. Copy `pokedaisy.vpk` to the Vita (VitaShell's USB or FTP) and install it from VitaShell.
3. Put your games in `ux0:data/pokedaisy/roms/` (`.gba`, `.gb`, `.gbc`; your own dumps, none are included). The folder
   is made on the first start. A save sits beside its game as `<game>.sav`, in mGBA's format, so saves move between this
   and mGBA / RetroArch's mGBA core.

The companion supports the same games as the 3DS port (retail English FireRed / LeafGreen / Emerald / Ruby / Sapphire,
the other languages and hacks it lists, the Game Boy games); any other game plays without one.

## Controls

The companion and the library were written with the 3DS's names for the buttons (A, B, X, Y); the library's strip at the
bottom draws them as the Vita's.

| 3DS name | Vita | In a game |
| --- | --- | --- |
| A / B | Cross / Circle | the GBA's A / B (`positional=1` in vita.ini swaps them) |
| X | Triangle | opens the companion; while it's open its buttons are the companion's (D-pad and Cross its cursor, L / R its tabs, Circle back). The window closes on Triangle, START, or Circle with nothing left to go back from; the side panel gives the buttons back on those (Triangle takes them again) and closes on START |
| Y | Square | QUICK MENU (the left stick aims its ring; it has COMPANION too) |
| L / R, START, SELECT, D-pad | the same | the GBA's; the left stick is the D-pad too |
| ZL | right stick left | the companion's tab before |
| ZR | right stick right (held) | fast-forward |

Touch: the companion where it's drawn; its X (and a tap on the dim around the window) closes it; on the game, the HUD's
parts (PC, BAG, WILD, COMPANION, the party, the minimap) and the QUICK MENU's tiles. In the library, Triangle shows INFO
(a game's save and party, a setting's words) over the page; on the save tools the right stick does. START in the
library closes the app.

## Files (`ux0:data/pokedaisy/`)

`roms/` (games and saves), `roms/pokedaisy-backups/` (each save copied before every start and state load, newest 10
per game), `states/`, `settings.ini` (the companion's settings), `vita.ini` (the button order,
`threaded_video`, `native_mixer`, `sound_capture`), `log.txt` (and `log-prev.txt`: the run before). When
something goes wrong, `log.txt` is what to send.

## Building

With [VitaSDK](https://vitasdk.org) installed (`VITASDK` set):

```
git submodule update --init third_party/mgba third_party/libvita2d
cmake -S vita -B build-vita -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita          # -> build-vita/pokedaisy.vpk
```

Without VitaSDK's prebuilt toolchain (only git reaching GitHub, no ARM GCC), `tools/clang-vitasdk/setup.sh <dir>`
builds an equivalent SDK from source around the machine's clang and lld; v0.1's VPK was built with it.

## The desktop harness

`desktop/` builds the port's own `main.c`, `video.c`, `audio.c` and `host.c` for a desktop over fakes of the Vita's
calls (`fake_vita.c`: vita2d drawn in software, the buttons and touch screen from a script, `shot` writes the 960x544
screen as a PNG). See the top of `fake_vita.c` for the script's commands. With a fixture ROM (the 3DS port's
`3ds/test/fixture_rom.c`, built with clang: `desktop/gba_crt0.s`, `desktop/gba.ld`, `desktop/gbafix.py`), the companion
shows the fixture's party.

## Not there yet

- One GBA core: mGBA (the 3DS's armGBA and gpSP recompilers are ARM11 code). The library's X: CORE pick does nothing here.
- No DS games (the Android app's melonDS isn't in the 3DS port this builds on).
- The layouts are fixed: the side-by-side one puts both screens at 4/3 (a GBA game at exactly 2x).
- RetroAchievements, co-op and the app's online features aren't in the 3DS port either.
