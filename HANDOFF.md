# Handoff: state of the Alien Hominid HD Vita port (2026-11-03)

Goal: Alien Hominid HD running natively on the PS Vita by running the game's own SWF content
and ActionScript 1 from the user's PC copy on cc-vita's AVM1 engine.

## Done
- Engine imported from unclejahjah/cc-vita at 69cfbb9.
- `brec_unwrap()` in `src/swf.c`: `swf_load()` now opens `.pak` (ZIP, stored or deflate),
  `.BREC` and plain `.swf`.
- Desktop harness checked on the Steam build's files, unmodified engine otherwise:
  - ALIEN: 505 shapes tessellated, about 900 triangles a frame; stand, walk, roll, shoot_stand,
    shoot_up, slash, dig, flip, die_burn, die_ice and beam render correctly.
  - `jump` renders an empty frame, probably because the body is moved by script, not timeline.
  - LEVEL11: 1,195 shapes tessellate in 0.2 s. MENU: 5 shapes.

## Facts about the data
- BREC header: u64 at 0x00 = 0x80, 0x08 = 1, 0x10 = 2, 0x20 = SWF offset (0xa0), 0x28 = SWF
  size + 1, 0x80 = 0x20. The meaning of the other fields is unknown.
- SWFs are version 6. ALIEN's first frame calls `_root.f_AlienInit(this)`, so the shared game
  functions live on `_root` in another movie, likely MENU or a level, as MAIN.swf did for
  Castle Crashers.
- The Steam exe is wrapped in SteamStub. It is not needed.

## Next (in order)
1. Find which movie defines the `_root` game functions (`f_AlienInit` and so on) and run it
   with `hosttest --as`.
2. Replace cc_host.c's Castle Crashers natives with Alien Hominid's (list what the scripts call
   that nothing defines).
3. Audio: WAV MS-ADPCM decoder in place of mpg123 MP3.
4. Text: `.dct` reader in place of strings_en.txt.
5. Vita front end: data path `ux0:data/AlienHominidHD/`, then menus and input.
6. Work out what `data/baf/*.baf` adds over the SWF shapes (HD outlines? textures?).
