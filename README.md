# ahhd-vita

A native PS Vita port of Alien Hominid HD. It runs the game's own Flash content and ActionScript from your copy of the game on a PS Vita, at 60 fps.

**No game data is included, and none may ever be committed (see `.gitignore`).** You need your own copy of Alien Hominid HD. The app reads the data files from it at runtime.

## Install

1. Get `ahhd_vita.vpk`: from this repository's releases, from the "Vita build" workflow's `ahhd-vita-vpk` artifact on GitHub Actions, or by building it yourself (see below).
2. Install the VPK with VitaShell.
3. Copy these folders from your install's `data` folder to `ux0:data/AlienHominidHD/` on the Vita, so that `ux0:data/AlienHominidHD/game/alien.pak` exists:

| Folder | Needed for | Size |
| --- | --- | --- |
| `game`, `levels` | The game itself | About 25 MB |
| `fonts`, `loc` | All text | Small |
| `sounds` | Sound effects | 5 MB |
| `music` | Music | 134 MB |

The game runs silent without `sounds` and `music`.

### Sound and music

1. Find your install's `data` folder. In Steam, right-click Alien Hominid HD, pick Manage, then Browse local files, and open `data`.
2. Copy the `sounds` and `music` folders as they are, without converting or renaming anything, to `ux0:data/AlienHominidHD/`. VitaShell's FTP server or a USB connection both work.
3. Check that `ux0:data/AlienHominidHD/sounds/` and `ux0:data/AlienHominidHD/music/` hold the game's `.adpcm` files directly, not inside another folder.

You can copy only `sounds` for effects without music, which saves 134 MB. Changes take effect the next time the app starts.

## Controls

| Button | Action |
| --- | --- |
| D-pad or left stick | Move, menu selection |
| Cross | Jump, accept |
| Square | Shoot |
| Circle | Grenade, back |
| Triangle | Hold with Down on the ground to dig; get in or out of a vehicle |
| L, R | Roll |
| Start | Pause in a level |
| Select | Use a coin to continue |
| Cross or Start | Skip the intro screens |
| Select + Start | Quit |
| Select + Up | Debug overlay |

## Saves

Progress, unlocks, options and volumes are kept in `ux0:data/AlienHominidHD/save.dat`. Back this file up before deleting the app's bubble.

## Not working

- Online play and leaderboards.
- All You Can Eat multiplayer.

## Building

With [VitaSDK](https://vitasdk.org) installed and `VITASDK` set:

```
mkdir build && cd build
cmake ..
make
```

This builds the full game, `ahhd_vita.vpk`.

### LiveArea art

The bubble and LiveArea art are made from the game's art, so they stay out of the repository. A build without them gets the default LiveArea. To use your own, put these PNG files in the gitignored `sce_sys` folder before running `cmake`:

| File | Size |
| --- | --- |
| `sce_sys/icon0.png` | 128x128, 8-bit indexed |
| `sce_sys/livearea/contents/bg.png` | 840x500, 8-bit indexed |
| `sce_sys/livearea/contents/startup.png` | 420x143, transparent |

`sce_sys/livearea/contents/template.xml` is in the repository and places the launch button.

### Animation viewer

`cmake -DAH_VIEWER=ON ..` builds `src/ah_viewer.c`, a viewer for the game's movies, in place of the game. It needs only `game` and `levels`.

Controls: Up/Down picks the movie, Left/Right the animation (or the root frame when a movie has no labels), L/R zooms, the left stick pans, Cross resets the view, Select+Up toggles the 60 fps cap, and Start quits.

Levels only show what their timeline places in the viewer, because most of a level is built by its scripts.

## How it works

The engine comes from [cc-vita](https://github.com/unclejahjah/cc-vita). Alien Hominid HD's game data is Flash content in a thin wrapper:

- `data/game/*.pak` and `data/levels/*.pak` are ZIP archives, each holding one uncompressed record, `NAME.COK6.BREC` (or `PDAn.PDAG.BREC` for the PDA minigames).
- A BREC record is a 160-byte header followed by a plain uncompressed SWF (version 6). The header holds the SWF's offset (u64 at 0x20, 0xa0 in every file seen) and size (u64 at 0x28).
- The SWF holds the vector shapes, sprites, timelines and the game's ActionScript 1 scripts, with the same custom opcodes (0x70, 0xA0 to 0xA4) cc-vita's interpreter already handles.

`swf_load()` unwraps the ZIP and BREC layers itself (`brec_unwrap()` in `src/swf.c`), so every tool and the app can be pointed straight at a `.pak` file.

Other data:

| Path | Format |
| --- | --- |
| `data/baf/*.baf` | Behemoth "BAF!" vector animation files, one per movie. Not used; what they add over the shapes in the SWF is unknown. |
| `data/music`, `data/sounds` `*.adpcm` | RIFF WAV, Microsoft ADPCM |
| `data/loc/*.dct` | String tables (key, text), 12 languages |
| `data/fonts` | BMFont binary `.fnt` plus PNG pages |
| `data/shaders` | Direct3D 9 shader bytecode. Not used. |

## Desktop harness

`tools/hosttest.c` builds the parser and renderer for Linux or WSL and writes frames as PPM images:

```
gcc -O2 -Ithird_party/libtess2/Include -o hosttest tools/hosttest.c \
  src/ah_host.c src/ah_view.c src/swf.c src/render.c src/as_core.c src/as_mc.c src/as_lib.c src/as_render.c \
  src/cc_host.c src/cc_native.c src/hostfont.c src/strtab.c src/cc_buttons.c \
  src/level.c src/player.c src/enemy.c third_party/libtess2/Source/*.c -lm -lz

./hosttest "<install>/data/game/alien.pak" walk out/walk 8
```

`--root file.pak frame out [zoom] [pan_x] [pan_y]` draws one root frame on a 960x544 screen, framed as the Vita viewer frames it.

The second argument of the default mode is a root frame label. ALIEN has 72, including `stand`, `walk`, `jump`, `roll`, `shoot_stand`, `shoot_up`, `slash`, `dig`, `flip`, `beam` and the `die_*` set.
