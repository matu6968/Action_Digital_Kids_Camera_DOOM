# Vendored apxtool

`apxtool/` is a copy of the host tool from the companion repository (`apxtool`, `src/apxtool/`), commit **ae9d505**
("support claiming low RAM window reservations"), MIT (`apxtool/LICENSE`). It is used through `PYTHONPATH=tools/python`
(the shell scripts and the Python tools in `tools/` set that up themselves), so no `pip install` is needed beyond `pyusb`.

It includes the Silvergear support (`silvergear*.py`), the thermal-printer debug commands (`debug printer`) and the SCSI backend
(`scsiio/`, now inside the package).

Local changes (only the default file locations, so the tool works inside this repository instead of next to the SDK checkout):

| File | Change |
|---|---|
| `flasher.py`, `cli.py`, `rom_rescue.py` | the flasher payload defaults to `tools/flasher/build/flasher.bin` (`make -C tools/flasher`; override with `APXTOOL_FLASHER` or `--payload`) |
| `debug_cli.py` | `debug start` defaults to `firmware/build/doom.manifest.json` |
| `silvergear.py` | the forced-BootROM stage defaults to `tools/flasher/build/silvergear-stage.bin` (not built here; pass `--stage` or build it from the SDK's `boot/`) |

`silvergear_bringup.py` still points at the SDK's `build/flasher/silvergear-bringup.bin`; it is a bring-up tool this repository does not use.

`tools/flasher/` is the SDK's `boot/` flasher (`flasher.c`, `flasher.S`, `flasher.ld`), built to a byte-identical `flasher.bin`.
