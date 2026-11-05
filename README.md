# ahhd-vita

A native PS Vita port of Alien Hominid HD. It runs the game's own Flash content and ActionScript from your copy of the game on a PS Vita, at 60 fps.

**No game data is included, and none may ever be committed (see `.gitignore`).** You need your own copy of Alien Hominid HD. The app reads the data files from it at runtime.

## Install

You need a PS Vita with HENkaku or Ensō and VitaShell, plus your own copy of Alien Hominid HD on a PC. The Steam release is the one this was tested with.

### 1. Install the app

1. Get `ahhd_vita.vpk`. It comes from this repository's releases, from the "Vita build" workflow's `ahhd-vita-vpk` artifact on GitHub Actions, or from building it yourself (see [Building](#building)).
2. Copy the VPK to the Vita, for example to `ux0:downloads/`.
3. In VitaShell, open the VPK and press Cross to install it. The "Alien Hominid" bubble appears on the home screen.

### 2. Find the game's data folder on your PC

In Steam, right-click Alien Hominid HD and choose Manage, then Browse local files. Open the `data` folder. You'll use these folders from it:

| Folder | Holds | Needed for | Size |
| --- | --- | --- | --- |
| `game` | `.pak` files (`alien.pak`, `menu.pak`, ...) | The game itself | About 25 MB with `levels` |
| `levels` | `.pak` files (`level11.pak`, ...) | The levels | |
| `fonts` | `.fnt` and `.png` files | All text | Small |
| `loc` | `.dct` files | Menu text | Small |
| `sounds` | 224 `.adpcm` files | Sound effects | About 5 MB |
| `music` | 36 `.adpcm` files | Music | About 134 MB |

The other folders, `baf` and `shaders`, aren't used.

### 3. Copy the data to the Vita

1. Connect the Vita to the PC. You can use VitaShell's FTP server (press Select in VitaShell, then connect an FTP client to the address it shows), or its USB mode.
2. On the Vita, create the folder `ux0:data/AlienHominidHD/`.
3. Copy the six folders from the table into it, as they are. Don't convert, rename or unpack any files.

When you're done, it should look like this:

```
ux0:data/AlienHominidHD/
├── game/alien.pak       (and the other .pak files)
├── levels/level11.pak   (and the other .pak files)
├── fonts/synjunicode_20.fnt
├── loc/en.dct
├── sounds/*.adpcm
└── music/*.adpcm
```

Check that each folder holds its files directly. A common mistake is ending up with `ux0:data/AlienHominidHD/data/game/` or `sounds/sounds/`.

### Sound and music

Sound effects come from `sounds`, and music from `music`. Both are the game's own `.adpcm` files, read as they are, so you only need to copy them. Without either folder, the game still runs, just without that kind of sound. To save space, you can copy `sounds` and leave out `music`, which is the 134 MB folder.

If there's no sound:
- Check that the files are in `ux0:data/AlienHominidHD/sounds/` and `ux0:data/AlienHominidHD/music/`, not in a subfolder.
- Check the Music and SFX volumes in the game's options.
- Check the Vita's own volume.

### 4. Start the game

Tap the bubble. If a data file is missing, the app shows which one and where it expected it, instead of starting.

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

## License

The source code is under the MIT License, see `LICENSE`. That does not cover Alien Hominid HD or any of its art, audio, data or names, which belong to The Behemoth.
