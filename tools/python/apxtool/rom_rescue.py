"""On-chip SRAM recovery for a camera stuck in the forced BootROM loader.

The forced BootROM (USB 1908:3319, ``BLDR v1.00``) has no DDR and none of the
vendor application's SPI routines. This module uploads the SRAM-only payload
from ``appotech-sdk/boot`` through the ROM's ``0xcb`` command, runs it, and uses
it for JEDEC/read/erase/program. The ROM resets itself about two seconds after
the last USB interrupt, so the first payload call disables its watchdog and the
host keeps traffic flowing until then.
"""
from __future__ import annotations

import glob
import hashlib
import os
import struct
import time
from pathlib import Path
from types import SimpleNamespace

from .errors import ApxError, DeviceError, TransferError
from .protocol import RAM_TRANSFER_FUNCTION, build_ram_transfer_cdb
from .sdk_flash import CAPACITY, FlashClient
from .transport import LinuxSCSITransport, inquiry, require_complete

ENTRY = 0x2000
CONTROL = 0x3900
SRAM_LIMIT = 0x3C00
HEADER = struct.Struct("<6I")
MAGIC = 0x52585041
LOADER_ID = ("1908", "3319")
JEDEC = bytes.fromhex("5e4015")
WATCHDOG_SPR = 0x880C
SNAPSHOT_SPRS = (0x880C, 0x8858, 0x885C, 0x8864, 0x9000, 0x9244, 0x9248, 0x924C)

CMD_JEDEC, CMD_READ, CMD_PROGRAM, CMD_ERASE, CMD_SPR, CMD_RAM, CMD_SNAPSHOT, CMD_PM_ENABLE, CMD_PM_READ = 0, 1, 2, 3, 4, 5, 6, 7, 8
PM_REGISTERS = tuple(range(0x40, 0x5C, 2))  # read addresses; writes go to address + 1
JEDEC_ROM_STATE = 1  # command 0 address bit: probe with the BootROM's SPI setup

# Images whose provenance is recorded in the repository, for dump identification.
KNOWN_IMAGES = {
    "bc2086e3a8173066fe618788653ffe43019047f63f9f63012355eb0dcc68ebe1": "SDK camera-demo SPI image (failed cold boot)",
    "063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66": "vendor image with a saved test photo, pre-SDK-install backup (not shipped)",
    "2d0d864fb3092c28ff174f73a6df7d7a47fb12ccbce1562f2b7bfc66d7d02137": "stock firmware with user photos removed (Action_Digital_Kids_Camera_DOOM/dumps/flash.bin)",
    "03d9919a02a302b660b4a19eb1a1f3031e53d65b8407e5c4b0e5f0185b89c961": "vendor image, flash.bin.golden",
}


def default_payload() -> Path:
    override = os.environ.get("APXTOOL_ROM_RESCUE")
    if override:
        return Path(override)
    return Path(__file__).resolve().parents[3] / "appotech-sdk" / "build" / "rom-rescue" / "rom-rescue.bin"


def check_boot_image(image: bytes) -> None:
    """Refuse images the BootROM would not accept (checks mirror BootROM 0x100440+)."""
    if len(image) != CAPACITY:
        raise ValueError("boot image must be exactly 2 MiB")
    if struct.unpack_from("<H", image, 2)[0] != 2 or image[4:8] != b"BLDR":
        raise ValueError("image lacks the BLDR boot header")
    if image[510:512] != b"\x55\xaa":
        raise ValueError("image header sector lacks the 0x55aa signature")


class ROMRescueFlash(FlashClient):
    """FlashClient backed by the SRAM rescue payload in the forced BootROM."""

    def __init__(self, device, image, *, spi_init: str = "auto"):
        if spi_init not in ("auto", "rom", "sdk", "none"):
            raise ValueError("spi_init must be auto, rom, sdk or none")
        self.device = device
        self.debug = SimpleNamespace(transport=device)
        self.sequence = 0
        self.notes: list[str] = []
        code = Path(image).read_bytes() if not isinstance(image, bytes) else image
        if not code or len(code) > 4096:
            raise ValueError("SRAM rescue exceeds its code reservation")
        if not inquiry(device).is_loader:
            raise ValueError("forced BootROM loader required")
        # Verify the upload before running it; the first call disables the
        # watchdog, and the whole sequence is a few milliseconds of traffic.
        self._write(ENTRY, code)
        if self._read(ENTRY, len(code)) != code:
            raise TransferError("SRAM rescue upload readback failed")
        self.snapshot = self._take_snapshot()
        self.watchdog_off = self.snapshot[WATCHDOG_SPR] == 0
        if not self.watchdog_off:
            self.notes.append(
                f"watchdog SPR 0x880c reads {self.snapshot[WATCHDOG_SPR]:#x} after disable; "
                "relying on continuous host traffic to keep the ROM alive"
            )
        self.spi_path = self._bring_up_spi(spi_init)

    # -- low level -----------------------------------------------------------------
    @staticmethod
    def _cdb(address, callback=0xFFFFFFFF):
        return build_ram_transfer_cdb(
            function_address=RAM_TRANSFER_FUNCTION, memory_address=address, callback_address=callback
        )

    def _read(self, address, n):
        if not (0x2000 <= address and address + n <= SRAM_LIMIT):
            raise ValueError("rescue SRAM read outside reservation")
        return require_complete(self.device.execute(self._cdb(address), data_in_length=n), n, "SRAM rescue read")

    def _write(self, address, data):
        if not (0x2000 <= address and address + len(data) <= SRAM_LIMIT):
            raise ValueError("rescue SRAM write outside reservation")
        for offset in range(0, len(data), 512):
            part = data[offset:offset + 512]
            # A data-out transfer carries no response bytes; only the residual says whether it completed.
            transfer = self.device.execute(self._cdb(address + offset), data_out=part)
            if transfer.residual:
                raise TransferError(f"SRAM rescue upload left residual {transfer.residual} of {len(part)} bytes")

    def _transact(self, cmd, address, length, data=b""):
        """Run one payload command; returns (status, response data)."""
        if not 0 <= length <= 512:
            raise ValueError("SRAM rescue payload exceeds 512 bytes")
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF
        request = HEADER.pack(MAGIC, self.sequence, cmd, address, length, 0xFFFFFFFF) + data
        self._write(CONTROL, request)
        if cmd in (CMD_PROGRAM, CMD_ERASE) and self._read(CONTROL, len(request)) != request:
            raise TransferError("SRAM request readback mismatch")
        self.device.execute(self._cdb(CONTROL, ENTRY), data_in_length=24)
        result = self._read(CONTROL, 24 + (3 if cmd == CMD_JEDEC else length))
        magic, seq, operation, a, n, status = HEADER.unpack_from(result)
        if (magic, seq, operation, a, n) != (MAGIC, self.sequence, cmd, address, length):
            raise TransferError("stale SRAM rescue response")
        return status, result[24:]

    def command(self, cmd, address, length, data=b""):
        status, result = self._transact(cmd, address, length, data)
        if status:
            raise TransferError(f"SRAM rescue status {status} for command {cmd}")
        return result

    # -- bring-up --------------------------------------------------------------------
    def _take_snapshot(self):
        data = self.command(CMD_SNAPSHOT, 0, 32)
        return dict(zip(SNAPSHOT_SPRS, struct.unpack("<8I", data)))

    def _bring_up_spi(self, mode):
        if mode == "none":  # PM-chip work needs no flash bus
            return "not used"
        attempts = []
        if mode in ("auto", "rom"):
            status, data = self._transact(CMD_JEDEC, JEDEC_ROM_STATE, 0)
            if not status:
                return "rom-state"
            attempts.append(f"ROM state read {data.hex()}")
        if mode in ("auto", "sdk"):
            status, data = self._transact(CMD_JEDEC, 0, 0)
            if not status:
                return "sdk-init"
            attempts.append(f"SDK init read {data.hex()}")
        raise TransferError(
            "rescue could not read the expected JEDEC ID 5e4015 ("
            + "; ".join(attempts)
            + "). Is the SPI short released and the flash powered?"
        )

    def _request(self, command, payload=b""):
        if not command:
            return (
                b'{"jedec":6176789,"capacity":2097152,"host_read_only":false,'
                b'"write_protocol":1,"backend":"forced-rom-sram"}'
            )
        if command == 1:
            address, length = struct.unpack("<II", payload)
            result = bytearray()
            while len(result) < length:
                n = min(512, length - len(result))
                result.extend(self.command(CMD_READ, address + len(result), n))
            return bytes(result)
        if command in (2, 3) and payload[:4] == b"APXF":
            address = struct.unpack_from("<I", payload, 4)[0]
            self.command(command, address, len(payload) - 8, payload[8:])
            return b""
        raise ValueError("unsupported SRAM rescue command")

    def read_sram(self, address, n):
        """Plain BootROM RAM read (no payload command): SRAM survives resets, e.g. the SDK's boot marks."""
        return self._read(address, n)

    def pm_read(self):
        """Registers 0x40..0x5a of the always-powered PM chip, as {address: value}."""
        data = self.command(CMD_PM_READ, 0, len(PM_REGISTERS))
        return dict(zip(PM_REGISTERS, data))

    def pm_enable(self):
        """Apply the stock start-up power sequence; returns the registers read back afterwards."""
        data = self.command(CMD_PM_ENABLE, 0, len(PM_REGISTERS))
        return dict(zip(PM_REGISTERS, data))

    def spr(self, address):
        return struct.unpack("<I", self.command(CMD_SPR, address, 4))[0]


# -- device discovery ---------------------------------------------------------------
def loader_sg_paths() -> list[str]:
    """SCSI generic nodes of USB devices identifying as the forced BootROM."""
    found = []
    for device in glob.glob("/sys/bus/usb/devices/*"):
        try:
            ids = (Path(device, "idVendor").read_text().strip(), Path(device, "idProduct").read_text().strip())
        except OSError:
            continue
        if ids != LOADER_ID:
            continue
        for node in glob.glob(f"{device}/*/host*/target*/*/scsi_generic/sg*"):
            found.append("/dev/" + Path(node).name)
    return sorted(found)


def open_loader(path: str, *, grace: float = 1.0) -> LinuxSCSITransport:
    """Open an sg node, riding out the short window before udev grants access."""
    deadline = time.monotonic() + grace
    while True:
        try:
            return LinuxSCSITransport(path)
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def _flush_print(*args):
    print(*args, flush=True)


def run_when_caught(action, *, payload, timeout, spi_init="auto", device=None, log=_flush_print, poll=0.005, attempt_limit=None):
    """Catch the forced BootROM, bring up the rescue payload and call ``action(client, attempt)``.

    The ROM cycles every few seconds while idle, so a failure after the catch
    simply waits for the next cycle and starts again; ``action`` must be safe to
    repeat (all of this module's actions are).
    """
    deadline = time.monotonic() + timeout
    attempt = 0
    last = None
    log("waiting for the forced BootROM (USB 1908:3319): hold the SPI short, then connect USB power")
    while time.monotonic() < deadline and (attempt_limit is None or attempt < attempt_limit):
        paths = [device] if device else loader_sg_paths()
        if not paths:
            time.sleep(poll)
            continue
        attempt += 1
        started = time.monotonic()
        try:
            with open_loader(paths[0]) as dev:
                client = ROMRescueFlash(dev, payload, spi_init=spi_init)
                log(
                    f"attempt {attempt}: rescue payload up in {time.monotonic() - started:.2f}s, "
                    f"SPI via {client.spi_path}, watchdog SPR {client.snapshot[WATCHDOG_SPR]:#x}"
                )
                for note in client.notes:
                    log("note: " + note)
                return action(client, attempt)
        except (OSError, ApxError, ValueError) as exc:
            last = exc
            log(f"attempt {attempt} failed after {time.monotonic() - started:.2f}s: {exc}")
            # Let the ROM drop off the bus before looking for the next cycle.
            time.sleep(0.5)
    if not attempt:
        raise DeviceError(f"no forced-BootROM device (USB 1908:3319) appeared within {timeout:g}s")
    raise DeviceError(f"no rescue session completed before the timeout (last error: {last})")


# -- actions -------------------------------------------------------------------------
def describe_image(image: bytes) -> str:
    digest = hashlib.sha256(image).hexdigest()
    return f"{digest} ({KNOWN_IMAGES.get(digest, 'unrecognised image')})"


def action_jedec(client, attempt):
    header = client.read(0, 512)
    flags = header[10]
    valid = struct.unpack_from("<H", header, 2)[0] == 2 and header[4:8] == b"BLDR" and header[510:512] == b"\x55\xaa"
    return {
        "jedec": "5e4015",
        "spi_path": client.spi_path,
        "watchdog_off": client.watchdog_off,
        "snapshot": {f"{k:#06x}": f"{v:#010x}" for k, v in client.snapshot.items()},
        "boot_header_valid": valid,
        "header_flags": flags,
        "load_address": struct.unpack_from("<I", header, 0x20)[0],
        "load_sectors": struct.unpack_from("<I", header, 0x28)[0],
        "header_sha256": hashlib.sha256(header).hexdigest(),
    }


def action_pm(client, attempt, apply):
    before = client.pm_read()
    after = client.pm_enable() if apply else None
    return {"before": before, "after": after, "keep_alive": (after or before)[0x5A] == 0x08}


def format_pm(result):
    lines = ["PM chip registers (stock-running camera: 0x40=0x16 0x44=0x00 0x54=0x21 0x58=0x0c 0x5a=0x08):"]
    for address in PM_REGISTERS:
        before = result["before"][address]
        after = result["after"][address] if result["after"] else None
        lines.append(f"  0x{address:02x}: {before:#04x}" + (f" -> {after:#04x}" if after is not None and after != before else ""))
    lines.append("keep-alive register 0x5a = 0x08: " + ("yes" if result["keep_alive"] else "NO"))
    return "\n".join(lines)


def action_marks(client, attempt):
    from . import trace as flight_recorder

    return flight_recorder.decode_marks(client.read_sram(flight_recorder.MARKS_ADDRESS, flight_recorder.MARKS_BYTES))


def action_dump(client, attempt, output):
    output = Path(output)
    image = client.read(0, CAPACITY)
    with output.open("xb") as file:
        file.write(image)
        file.flush()
        os.fsync(file.fileno())
    return describe_image(image)


def action_restore(client, attempt, image, backup, expected_sha256, log=_flush_print):
    """Back up the chip, then restore ``image`` (header sector last, per-sector verify)."""
    backup = Path(backup)
    if attempt > 1:
        backup = backup.with_name(f"{backup.name}.retry{attempt}")
    current = client.read(0, CAPACITY)
    digest = hashlib.sha256(current).hexdigest()
    log(f"chip currently holds {describe_image(current)}")
    from . import trace as flight_recorder

    recorded = current[flight_recorder.SECTOR:flight_recorder.SECTOR + flight_recorder.SECTOR_SIZE]
    if recorded[:4] == b"APXT":  # evidence of the last boot, before the sector is rewritten
        log("flight recorder found on the chip:\n" + flight_recorder.format_report(flight_recorder.decode(recorded)))
    if expected_sha256 not in (None, "auto") and digest != expected_sha256.lower():
        raise ApxError(f"chip hash {digest} differs from --expect-current-sha256; nothing was erased")
    if current == image:
        log("chip already holds the requested image")
        return {"status": "unchanged", "sha256": digest}
    changed = sum(1 for a in range(0, CAPACITY, 4096) if current[a:a + 4096] != image[a:a + 4096])
    log(f"writing {changed} changed 4 KiB sectors; do not disconnect")
    state = client.replace(
        image,
        backup,
        expected_sha256=digest,
        current=current,
        progress=lambda done, total: log(f"  verified sector {done}/{total}") if done % 25 == 0 or done == total else None,
    )
    return state


# -- command line --------------------------------------------------------------------
def add_arguments(sub) -> None:
    rescue = sub.add_parser(
        "rom-rescue",
        help="recover flash through the forced BootROM loader (USB 1908:3319) using an SRAM payload",
    )
    rescue.add_argument("--timeout", type=float, default=300.0, help="seconds to wait for a completed session")
    rescue.add_argument("--payload", type=Path, default=None, help="rescue payload binary (default: SDK build output)")
    rescue.add_argument("--spi-init", choices=("auto", "rom", "sdk", "none"), default="auto",
                        help="SPI setup: reuse the BootROM's, apply the SDK's, or try both (default)")
    rescue.add_argument("--yes", action="store_true", help="acknowledge destructive flash writes")
    actions = rescue.add_subparsers(dest="rescue_command", required=True)
    actions.add_parser("jedec", help="read-only: JEDEC ID, boot header sector and diagnostic SPRs")
    dump = actions.add_parser("dump", help="read the whole 2 MiB chip into a new file")
    dump.add_argument("output", type=Path)
    trace = actions.add_parser("trace", help="read-only: decode the SDK cold-boot flight recorder sector")
    trace.add_argument("--save", type=Path, help="also save the raw 4 KiB sector to a new file")
    actions.add_parser("marks", help="read-only: the production build's always-on SRAM boot breadcrumbs (0x2f00)")
    pm = actions.add_parser("pm", help="read the PM chip registers; with --apply, run the stock power sequence (no flash access)")
    pm.add_argument("--apply", action="store_true", help="write the stock start-up sequence (requires --yes)")
    restore = actions.add_parser("restore", help="back up the chip, then write a boot image over it")
    restore.add_argument("image", type=Path)
    restore.add_argument("--backup", type=Path, required=True, help="new file for the pre-restore chip contents")
    restore.add_argument("--expect-current-sha256", default="auto",
                         help="refuse to erase unless the chip hashes to this value (default: accept and record)")


def run_cli(args) -> int:
    import json

    payload = args.payload or default_payload()
    if not Path(payload).is_file():
        raise ApxError(f"rescue payload not built: {payload} (run make -C appotech-sdk/boot)")
    if args.timeout <= 0:
        raise ValueError("--timeout must be positive")
    common = dict(payload=payload, timeout=args.timeout, spi_init=args.spi_init, device=args.device)
    if args.rescue_command == "jedec":
        print(json.dumps(run_when_caught(action_jedec, **common), indent=2))
        return 0
    if args.rescue_command == "pm":
        if args.apply and not args.yes:
            raise ApxError("--apply writes PM chip registers; pass --yes to confirm")
        result = run_when_caught(lambda c, n: action_pm(c, n, args.apply), **{**common, "spi_init": "none"})
        print(format_pm(result))
        return 0
    if args.rescue_command == "marks":
        from . import trace as flight_recorder

        print(flight_recorder.format_marks(run_when_caught(action_marks, **{**common, "spi_init": "none"})))
        return 0
    if args.rescue_command == "trace":
        from . import trace as flight_recorder

        if args.save and args.save.exists():
            raise FileExistsError(f"refusing to overwrite {args.save}")

        def read_trace(client, attempt):
            sector = client.read(flight_recorder.SECTOR, flight_recorder.SECTOR_SIZE)
            if args.save:
                with args.save.open("xb") as file:
                    file.write(sector)
            return sector

        print(flight_recorder.format_report(flight_recorder.decode(run_when_caught(read_trace, **common))))
        return 0
    if args.rescue_command == "dump":
        if args.output.exists():
            raise FileExistsError(f"refusing to overwrite {args.output}")
        print("dumped", run_when_caught(lambda c, n: action_dump(c, n, args.output), **common))
        return 0
    # restore: validate everything locally before touching the device.
    if not args.yes:
        raise ApxError("restore erases and reprograms flash; pass --yes to confirm")
    image = args.image.read_bytes()
    check_boot_image(image)
    if args.backup.exists() or args.backup.with_suffix(args.backup.suffix + ".json").exists():
        raise FileExistsError(f"backup/journal must be new: {args.backup}")
    print(f"restoring {describe_image(image)}")
    state = run_when_caught(
        lambda c, n: action_restore(c, n, image, args.backup, args.expect_current_sha256), **common
    )
    print(json.dumps({k: v for k, v in state.items() if k not in ("verified_sectors", "changed_sectors")}, indent=2))
    return 0
