# ahhd-vita

A native PS Vita port of Alien Hominid HD, built on the engine from [cc-vita](https://github.com/unclejahjah/cc-vita) (the Castle Crashers port).

No game data is included, and none may ever be committed (see `.gitignore`). The app reads the data files from your own copy of the game at runtime.

## Why cc-vita's engine fits

Alien Hominid HD's game data is Flash content in a thin wrapper:

- `data/game/*.pak` and `data/levels/*.pak` are ZIP archives, each holding one uncompressed record, `NAME.COK6.BREC` (or `PDAn.PDAG.BREC` for the PDA minigames).
- A BREC record is a 160-byte header followed by a plain uncompressed SWF (version 6). The header holds the SWF's offset (u64 at 0x20, 0xa0 in every file seen) and size (u64 at 0x28).
- The SWF holds the vector shapes, sprites, timelines and the game's ActionScript 1 scripts, with the same custom opcodes (0x70, 0xA0 to 0xA4) cc-vita's interpreter already handles.

`swf_load()` unwraps the ZIP and BREC layers itself (`brec_unwrap()` in `src/swf.c`), so every tool and the app can be pointed straight at a `.pak` file.

Other data, not used yet:

| Path | Format |
| --- | --- |
| `data/baf/*.baf` | Behemoth "BAF!" vector animation files, one per movie. What they add over the shapes in the SWF is still unknown. |
| `data/music`, `data/sounds` `*.adpcm` | RIFF WAV, Microsoft ADPCM |
| `data/loc/*.dct` | String tables (key, text), 12 languages |
| `data/fonts` | BMFont binary `.fnt` plus PNG pages |
| `data/shaders` | Direct3D 9 shader bytecode (to be rewritten for the Vita) |

## Desktop harness

`tools/hosttest.c` builds the parser and renderer for Linux or WSL and writes frames as PPM images:

```
gcc -O2 -Ithird_party/libtess2/Include -o hosttest tools/hosttest.c \
  src/swf.c src/render.c src/as_core.c src/as_mc.c src/as_lib.c src/as_render.c \
  src/cc_host.c src/cc_native.c src/hostfont.c src/strtab.c src/cc_buttons.c \
  src/level.c src/player.c src/enemy.c third_party/libtess2/Source/*.c -lm -lz

./hosttest "<install>/data/game/alien.pak" walk out/walk 8
```

The second argument is a root frame label. ALIEN has 72, including `stand`, `walk`, `jump`, `roll`, `shoot_stand`, `shoot_up`, `slash`, `dig`, `flip`, `beam` and the `die_*` set.

## Vita build

```
mkdir build && cd build
cmake ..
make
```

The Vita app is still cc-vita's Castle Crashers front end (paths, host natives, menus). Adapting it is the next step; see `HANDOFF.md`.
