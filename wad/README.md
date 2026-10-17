# WAD

`doom1_e1m1.wad` is what the camera plays: the shareware DOOM IWAD in **GBADoom's processed format**, with E1M1 as the only map,
trimmed to 1.48 MB so it fits the 1.5 MiB RAM region the firmware reserves for it. `seed_doom1_e1m1_sfx.wad` (3.2 MB) is its
source: the reference console port's E1M1 WAD with the DOOM sound lumps merged back in.

```sh
python3 tools/wad_trim.py --in wad/seed_doom1_e1m1_sfx.wad --engine firmware/GBADoom --out wad/doom1_e1m1.wad --report
```

`wad_trim.py` keeps the directory of the WAD exactly as it is (every lump keeps its name and number, so sprite, flat and
texture ranges and name lookups behave as before) and **empties the lumps the map cannot reach**. What it keeps:

| | |
|---|---|
| Everything outside sprites / patches / flats | except the help, credit, intermission and demo lumps and the text screen |
| Sprites | those of the things placed on E1M1 and their state chains (parsed from `info.c`), the player's weapons (fist, pistol, shotgun, chaingun; `--all-weapons` adds the rest), puffs, blood, teleport fog, imp fireballs |
| Textures and patches | the textures used by the map's sidedefs, their animation and switch partners, and the patches they are built from |
| Flats | those the sectors use plus the rest of their animation group |
| Sounds | the sound effects of the kept monsters and a fixed set of weapon, player and world sounds, **resampled to 8 kHz** (`--sound-rate`) |
| Palettes | `PLAYPAL` and `PLAYPAL1` (two gamma levels; the engine clamps the menu to that) |

The result is checked three ways: on the host (the directory still parses, the data is 4-byte aligned), on the camera (builds with
`DOOM_STATS=1` print `EMPTY LUMP requested: NAME` the first time the engine asks for a lump that was emptied; `tools/bot.py` walks
around for a while) and by playing it. The first two found nothing in a two-minute run; the whole map has not been played through.

The firmware wants the processed format: lump data 4-byte aligned, level geometry and graphics converted by GBADoom's
`GbaWadUtil`. A raw `doom1.wad` will not work.

## Putting it on the camera

* **SD card:** copy `doom1_e1m1.wad` to the root of a FAT/exFAT card and name it `DOOM.WAD`.
* **In the firmware image:** `tools/spi_pack.py --wad wad/doom1_e1m1.wad` embeds a deflate-compressed copy (681 KB) in the free flash.
  It is used when there is no card, or no `DOOM.WAD` on it.
* **Over USB, while developing:** `tools/load_wad.py wad/doom1_e1m1.wad`.

## Licence

The DOOM shareware IWAD is freely redistributable under id Software's shareware terms; it is not GPL. The GPL applies to the engine
source, not to the game data. Retail WADs are not redistributable. The trimmed WAD here removes content, adds none.
