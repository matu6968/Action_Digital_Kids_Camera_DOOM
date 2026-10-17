from __future__ import annotations

import argparse
import json
import hashlib
import os
from pathlib import Path
import sys
import tempfile

from . import __version__, rom_rescue
from .devices import enter_loader, enter_loader_with_device, require_loader, validate_camera_path
from .errors import ApxError, DeviceError, ProtocolUnavailable, TransferError, UnsafeOperation
from .flash import erase_sector, read_to_file, require_flash_id, verify_file, write_file
from .payload import run_marker_payload
from .hw import HardwareSession, NormalMemoryTransport, REGISTER_WHITELIST, clock_test, clock_profile, clock_trial, clock_pair_trial, clock_pair_hold, require_unused_pair_payload, verify_clock_trial_rom, require_supported_loader_boost, event, inspect_snapshot, new_report, ram_test
from .protocol import (
    RAM_BASE,
    RAM_LIMIT,
    RAM_READ_CHUNK,
)
from .ram import dump_ram_file, read_ram
from .spi import JT5295SPIFlash, SPI_REGISTER_SNAPSHOT, check_rom_api_signatures
from .transport import (
    Inquiry,
    LinuxSCSITransport,
    SCSITransport,
    inquiry,
    require_camera,
    require_complete,
)


def _integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from exc


def _cdb_bytes(value: str) -> bytes:
    compact = value.replace(":", " ").replace(",", " ")
    try:
        data = bytes.fromhex(compact)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("CDB must be hexadecimal bytes") from exc
    if len(data) != 16:
        raise argparse.ArgumentTypeError("raw-cdb requires exactly 16 bytes (32 hex digits)")
    return data


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="apxtool",
        description="Experimental SCSI and loader utility for the AppoTech JT5295 camera",
    )
    parser.add_argument("--version", action="version", version=f"apxtool {__version__}")
    parser.add_argument(
        "--device",
        required=False,
        help="explicit Linux SCSI generic path, for example /dev/sg0",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    rom_rescue.add_arguments(sub)
    trace_cmd = sub.add_parser("trace", help="decode the SDK cold-boot flight recorder from a saved sector or flash dump")
    trace_cmd.add_argument("file", type=Path, help="4 KiB sector, or a 2 MiB flash dump (sector read at 0x1ff000)")

    sub.add_parser("info", help="read standard SCSI INQUIRY and show camera mode")

    loader = sub.add_parser("loader", help="control the camera's built-in loader")
    loader_sub = loader.add_subparsers(dest="loader_command", required=True)
    start = loader_sub.add_parser("start", help="send 0xda and wait for BLDR re-enumeration")
    start.add_argument("--timeout", type=float, default=20.0)

    ram = sub.add_parser("ram", help="read the known RAM window")
    ram_sub = ram.add_subparsers(dest="ram_command", required=True)
    dump = ram_sub.add_parser("dump", help="read RAM using 0xcb in bounded chunks")
    dump.add_argument("output", type=Path)
    dump.add_argument("--address", type=_integer, default=RAM_BASE)
    dump.add_argument("--length", type=_integer, default=RAM_LIMIT - RAM_BASE)
    dump.add_argument(
        "--start-loader",
        action="store_true",
        help="send 0xda and capture immediately after loader re-enumeration",
    )
    dump.add_argument("--timeout", type=float, default=20.0)

    raw = sub.add_parser("raw-cdb", help="send an exact 16-byte SCSI CDB to the camera")
    raw.add_argument("cdb", type=_cdb_bytes, help="32 hex digits; spaces or ':' are allowed")
    raw.add_argument("--direction", choices=("none", "in", "out"), default="none")
    raw.add_argument("--length", type=_integer, help="data-in length; required for --direction in")
    raw.add_argument("--input", type=Path, help="data-out file; required for --direction out")
    raw.add_argument("--output", type=Path, help="write data-in bytes here instead of printing hex")
    raw.add_argument("--unsafe-write", action="store_true", help="allow a raw data-out transfer")
    raw.add_argument("--yes", action="store_true", help="confirm raw data-out transfer")

    payload = sub.add_parser("payload", help="run a volatile proof-of-execution payload")
    payload_sub = payload.add_subparsers(dest="payload_command", required=True)
    marker = payload_sub.add_parser(
        "marker",
        help="experimental 0xcb upload/callback test that writes APXT into zeroed RAM",
    )
    marker.add_argument("--image", type=Path, default=Path(__file__).resolve().parents[2] / "firmware/build/marker.bin")
    marker.add_argument("--experimental", action="store_true", required=True)
    marker.add_argument("--yes", action="store_true", required=True)
    marker.add_argument("--start-loader", action="store_true")
    marker.add_argument("--report-u32", action="store_true", help="print payload result bytes 4..7 as a little-endian word")
    marker.add_argument("--timeout", type=float, default=20.0)

    flash = sub.add_parser("flash", help="read and update SPI NOR through a volatile JT5295 payload")
    flash_sub = flash.add_subparsers(dest="flash_command", required=True)

    def add_flash_payload_options(command: argparse.ArgumentParser) -> None:
        command.add_argument("--payload", type=Path, default=Path(__file__).resolve().parents[2] / "firmware/build/spi.bin")
        command.add_argument("--start-loader", action="store_true")
        command.add_argument("--timeout", type=float, default=20.0)

    flash_sub.add_parser(
        "hello",
        help="verify the uploaded payload callback and APXS version without touching SPI",
    )
    add_flash_payload_options(flash_sub.choices["hello"])
    flash_sub.add_parser(
        "stage-probe",
        help="check volatile CPU write and 0xcb read visibility at the SPI staging buffer",
    )
    add_flash_payload_options(flash_sub.choices["stage-probe"])
    flash_sub.add_parser("probe", help="check candidate firmware API signatures without SPI access")
    add_flash_payload_options(flash_sub.choices["probe"])
    flash_sub.add_parser("init-probe", help="call the candidate SPI/GPIO init routine only")
    add_flash_payload_options(flash_sub.choices["init-probe"])
    flash_sub.add_parser("id-probe", help="call JEDEC read without GPIO/SPI initialization")
    add_flash_payload_options(flash_sub.choices["id-probe"])
    flash_regs = flash_sub.add_parser(
        "regs",
        help="experimental read of one controller SPR; does not send an SPI command",
    )
    add_flash_payload_options(flash_regs)
    flash_regs.add_argument(
        "--register",
        type=_integer,
        default=0x9000,
        help="whitelisted SPR address (default: 0x9000)",
    )
    flash_regs.add_argument("--experimental", action="store_true", required=True)
    flash_regs.add_argument("--yes", action="store_true", required=True)
    flash_id = flash_sub.add_parser("id", help="read the SPI NOR JEDEC ID")
    add_flash_payload_options(flash_id)
    flash_read = flash_sub.add_parser("read", help="read bytes to a new file")
    flash_read.add_argument("output", type=Path)
    flash_read.add_argument("--address", type=_integer, default=0)
    flash_read.add_argument("--length", type=_integer, required=True)
    flash_read.add_argument(
        "--skip-jedec-check",
        action="store_true",
        help="diagnostic read only: attempt SPI read even if JEDEC ID is invalid",
    )
    add_flash_payload_options(flash_read)
    flash_write = flash_sub.add_parser("write", help="sector-preserving flash update")
    flash_write.add_argument("input", type=Path)
    flash_write.add_argument("--address", type=_integer, required=True)
    flash_write.add_argument("--backup", type=Path, required=True)
    flash_write.add_argument("--yes", action="store_true", required=True)
    add_flash_payload_options(flash_write)
    flash_erase = flash_sub.add_parser("erase-sector", help="erase one sector after a full backup")
    flash_erase.add_argument("address", type=_integer)
    flash_erase.add_argument("--backup", type=Path, required=True)
    flash_erase.add_argument("--yes", action="store_true", required=True)
    add_flash_payload_options(flash_erase)
    verify = flash_sub.add_parser("verify", help="compare a file with bytes read from flash")
    verify.add_argument("input", type=Path)
    verify.add_argument("--address", type=_integer, required=True)
    add_flash_payload_options(verify)

    hw = sub.add_parser("hw", help="inspect memory and run volatile hardware diagnostics")
    hw_sub = hw.add_subparsers(dest="hw_command", required=True)
    for name in ("inspect", "ram-test", "clock-test", "clock-profile", "clock-trial", "clock-pair-trial", "clock-pair-hold"):
        command = hw_sub.add_parser(name, help="disabled after hardware transition failures" if name == "clock-trial" else None)
        command.add_argument("--start-loader", action="store_true")
        command.add_argument("--timeout", type=float, default=20.0)
        command.add_argument("--json", type=Path, help="new JSON report, journaled during probes")
        if name != "inspect":
            command.add_argument("--experimental", action="store_true", required=True)
            command.add_argument("--yes", action="store_true", required=True)
            payload_name = "clock-trial.bin" if name == "clock-trial" else "hw.bin"
            if name == "clock-pair-trial":
                payload_name = "clock-pair.bin"
            if name == "clock-pair-hold":
                payload_name = "clock-hold.bin"
            command.add_argument("--payload", type=Path, default=Path(__file__).resolve().parents[2] / "firmware/build" / payload_name)
    hw_sub.choices["inspect"].add_argument("--ram-output", type=Path, help="also preserve the complete RAM snapshot")
    for name in ("inspect", "clock-test", "clock-profile"):
        hw_sub.choices[name].add_argument("--normal-mode", action="store_true",
            help="use the stock firmware 0xcd RAM helper without entering the loader")
    hw_sub.choices["clock-profile"].add_argument("--include-dependencies", action="store_true",
        help="also read four statically reviewed USB/memory candidate SPRs, separately")
    hw_sub.choices["clock-test"].add_argument("--register", type=_integer, default=0x8868,
                                             help="one whitelisted SPR to read after loop tests")
    clock_mode = hw_sub.choices["clock-test"].add_mutually_exclusive_group()
    clock_mode.add_argument("--loop-only", action="store_true",
                                             help="measure loop throughput without reading an SPR")
    clock_mode.add_argument("--register-only", action="store_true", help="one register read after branch validation")
    clock_mode.add_argument("--timer-loop", action="store_true", help="also read candidate counter SPR 0x9804 around each loop; no timer writes")

    from .debug_cli import configure
    configure(parser, sub)
    return parser


def _run_hw(dev: SCSITransport, identity: Inquiry, args: argparse.Namespace, path: str) -> int:
    if args.hw_command == "clock-trial":
        require_supported_loader_boost()
    mode = "normal" if getattr(args, "normal_mode", False) else "loader"
    if args.hw_command in ("clock-pair-trial", "clock-pair-hold"):
        require_unused_pair_payload(args.payload, verified_repeat=args.hw_command == "clock-pair-hold")
    if mode == "loader":
        require_loader(identity)
    elif not identity.is_buildwin_camera or identity.is_loader:
        raise DeviceError("--normal-mode requires stock Buildwin Media-Player firmware")
    report = new_report(args.hw_command, path, mode)
    report["device_identity"] = {"vendor": identity.vendor, "product": identity.product,
                                 "revision": identity.revision}
    workspace = Path(__file__).resolve().parents[3]
    report["source_images"] = []
    for name in ("flash-icon-overlay-postwrite-v2.bin", "apxtool/research/jt5295-rom-00100000-64k.bin"):
        image = workspace / name
        if image.is_file():
            report["source_images"].append({"path": str(image), "sha256": hashlib.sha256(image.read_bytes()).hexdigest()})
    report_path = args.json.expanduser().resolve() if args.json else None
    if report_path is not None:
        # Reserve the requested name before taking a snapshot or uploading.
        with report_path.open("xb"):
            pass

    def journal() -> None:
        if report_path is not None:
            temp_path = None
            try:
                with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=report_path.parent,
                                                 prefix=report_path.name + ".", delete=False) as stream:
                    temp_path = Path(stream.name)
                    json.dump({k: v for k, v in report.items() if not k.startswith("_")}, stream, indent=2)
                    stream.write("\n")
                    stream.flush()
                    os.fsync(stream.fileno())
                temp_path.replace(report_path)
            finally:
                if temp_path is not None:
                    temp_path.unlink(missing_ok=True)

    report["_journal"] = journal
    journal()
    try:
        if args.hw_command in ("inspect", "ram-test"):
            snapshot = read_ram(dev, RAM_BASE, RAM_LIMIT - RAM_BASE)
            report.update(inspect_snapshot(snapshot))
            reference = workspace / "flash-icon-overlay-postwrite-v2.bin"
            if reference.exists():
                report["runtime_code_matches_reference"] = snapshot[:0x5CB00] == reference.read_bytes()[0x2200:0x2200 + 0x5CB00]
            if args.hw_command == "ram-test" and not report.get("runtime_code_matches_reference"):
                raise UnsafeOperation("loaded code differs from the analyzed firmware; RAM ownership boundaries require re-analysis")
            event(report, "snapshot", sha256=report["ram_sha256"])
            if args.hw_command == "inspect":
                if args.ram_output:
                    with args.ram_output.expanduser().open("xb") as output:
                        output.write(snapshot)
            else:
                with HardwareSession(dev, args.payload, experimental=args.experimental,
                                     confirmed=args.yes, report=report) as session:
                    session.branch_test()
                    ram_test(session, snapshot, report)
        elif args.hw_command in ("clock-pair-trial", "clock-pair-hold"):
            reference = workspace / "flash-icon-overlay-postwrite-v2.bin"
            prefix = read_ram(dev, RAM_BASE, 0x5CB00)
            if prefix != reference.read_bytes()[0x2200:0x2200 + len(prefix)]:
                raise UnsafeOperation("runtime code differs from analyzed firmware; pair trial refused")
            report["runtime_code_matches_reference"] = True
            report["runtime_code_sha256"] = hashlib.sha256(prefix).hexdigest()
            with HardwareSession(dev, args.payload, experimental=args.experimental,
                                 confirmed=args.yes, report=report, exploratory_pair=True,
                                 pair_hold=args.hw_command == "clock-pair-hold") as session:
                session.branch_test()
                if args.hw_command == "clock-pair-hold":
                    clock_pair_hold(session, report)
                else:
                    clock_pair_trial(session, report)
            if args.hw_command == "clock-pair-hold":
                receipt_path = Path(report["hold_receipt_path"])
                receipt = json.loads(receipt_path.read_text())
                receipt["ram_restoration_verified"] = True
                receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
        elif args.hw_command == "clock-profile":
            with HardwareSession(dev, args.payload, experimental=args.experimental,
                                 confirmed=args.yes, report=report) as session:
                session.branch_test()
                clock_profile(session, report, include_dependencies=args.include_dependencies)
        elif args.hw_command == "clock-trial":
            verify_clock_trial_rom(dev, report)
            with HardwareSession(dev, args.payload, experimental=args.experimental,
                                 confirmed=args.yes, report=report, stack=True) as session:
                session.branch_test()
                clock_trial(session, report)
        else:
            with HardwareSession(dev, args.payload, experimental=args.experimental,
                                 confirmed=args.yes, report=report) as session:
                session.branch_test()
                clock_test(session, report, register=args.register, loop_only=args.loop_only,
                           register_only=args.register_only, timer_loop=args.timer_loop)
        report["status"] = "completed"
        print(f"Hardware {args.hw_command} completed in {mode} mode via {path}")
        if "ram_window" in report:
            print("RAM window: 4 MiB at 0x02000000..0x023fffff")
        if "branch_delay_slot_executed" in report:
            print(f"Taken jump executes delay slot: {report['branch_delay_slot_executed']}")
        if report.get("ram_patterns_passed"):
            print(f"Two RAM pattern passes verified at {len(report['tested_ranges'])} guarded addresses; originals restored")
        if "registers" in report:
            for register, value in report["registers"].items():
                print(f"SPR {register}: 0x{value:08x}")
        if report.get("estimated_loops_per_second"):
            print(f"Estimated loop throughput: {report['estimated_loops_per_second']:.0f} iterations/s (includes timing uncertainty)")
        if "clock_trial" in report:
            trial = report["clock_trial"]
            print(f"Temporary 96 -> 144 -> 96 MHz trial: speedup {trial['speedup']:.3f}x; clock registers restored")
        if "clock_pair_trial" in report:
            print(f"Exploratory paired trial: {report['clock_pair_trial']['speedup']:.3f}x loop speedup; sampled clocks restored")
        if "clock_hold" in report:
            print(f"Higher clock held for {report['clock_hold']['duration_seconds']:.2f}s; 100 callbacks and five guarded RAM patterns passed; clocks restored")
        print(f"Confidence: {report['confidence']}")
        if report.get("cpu_hz_estimate"):
            print(f"CPU clock estimate: {report['cpu_hz_estimate'] / 1e6:g} MHz; PLL interpretation and CPU routing inferred")
        else:
            print("CPU frequency: unresolved; no verified oscillator/divider and independent timer relationship")
        return 0
    except BaseException as exc:
        report["status"] = "failed"
        report["error"] = str(exc)
        event(report, "operation_failed", error=str(exc))
        raise
    finally:
        journal()
        if report_path is not None:
            print(f"Hardware report: {report_path}", file=sys.stderr)


def _raw_data_out(transport: SCSITransport, args: argparse.Namespace) -> int:
    if not args.unsafe_write or not args.yes:
        raise UnsafeOperation("raw data-out requires both --unsafe-write and --yes")
    if args.input is None:
        raise ValueError("--direction out requires --input FILE")
    data = args.input.expanduser().read_bytes()
    if not data:
        raise ValueError("raw data-out file is empty")
    transfer = transport.execute(args.cdb, data_out=data)
    print(f"sent {len(data)} bytes; residual={transfer.residual}")
    return 0 if transfer.residual == 0 else 2


def _raw_data_in(transport: SCSITransport, args: argparse.Namespace) -> int:
    if args.length is None or args.length <= 0:
        raise ValueError("--direction in requires a positive --length")
    transfer = transport.execute(args.cdb, data_in_length=args.length)
    data = require_complete(transfer, args.length, "raw CDB data-in")
    if args.output is not None:
        path = args.output.expanduser().resolve()
        if not path.parent.is_dir():
            raise FileNotFoundError(path.parent)
        with path.open("xb") as stream:
            stream.write(data)
        print(f"received {len(data)} bytes; residual=0; wrote {path}")
    else:
        print(f"received {len(data)} bytes; residual=0")
        if len(data) <= 256:
            print(data.hex(" "))
    return 0


def _camera_transport(path: str) -> tuple[LinuxSCSITransport, Inquiry]:
    validate_camera_path(path)
    dev = LinuxSCSITransport(path)
    try:
        identity = inquiry(dev)
        require_camera(identity)
        return dev, identity
    except Exception:
        dev.close()
        raise


def _run_flash(dev: SCSITransport, args: argparse.Namespace, device_path: str) -> int:
    if args.timeout <= 0:
        raise ValueError("--timeout must be positive")
    with JT5295SPIFlash(dev, args.payload) as flash_client:
        geometry = flash_client.hello()
        if args.flash_command == "hello":
            print(
                f"SPI payload callback responded; APXS v{geometry.major}; "
                f"flash={geometry.size} bytes page={geometry.page_size} "
                f"sector={geometry.sector_size} max-transfer={geometry.max_transfer}; "
                "no SPI command sent"
            )
            return 0
        if args.flash_command == "stage-probe":
            sample = flash_client.probe_staging_buffer()
            print(f"SPI staging RAM is coherent across callback and 0xcb: {sample.hex()}")
            return 0
        if args.flash_command == "init-probe":
            marker = flash_client.probe_spi_init()
            if marker != b"INI":
                raise ApxError(f"SPI init probe returned unexpected marker {marker!r}")
            print("candidate GPIO/SPI initialization routine returned")
            return 0
        if args.flash_command == "id-probe":
            jedec_id = flash_client.probe_id_without_init()
            require_flash_id(jedec_id)
            print(f"JEDEC ID without initialization: {jedec_id.hex().upper()}")
            return 0
        if args.flash_command == "regs":
            if args.register not in SPI_REGISTER_SNAPSHOT:
                raise ValueError(
                    f"SPR 0x{args.register:04x} is not in the diagnostic whitelist"
                )
            value = flash_client.read_controller_register(args.register)
            print(f"SPR 0x{args.register:04x}: 0x{value:08x}")
            print("read-only register probe; no SPI command sent")
            return 0
        if args.flash_command == "read" and args.skip_jedec_check:
            jedec_id = None
        else:
            jedec_id = flash_client.flash_id()
            require_flash_id(jedec_id)
        if args.flash_command == "id":
            print(
                f"JEDEC ID: {jedec_id.hex().upper()}  "
                f"flash={geometry.size} bytes page={geometry.page_size} "
                f"sector={geometry.sector_size} max-transfer={geometry.max_transfer}"
            )
            return 0
        if args.flash_command == "read":
            digest = read_to_file(flash_client, args.address, args.length, args.output)
            print(f"read {args.length} bytes; SHA-256 {digest}")
            return 0
        if args.flash_command == "verify":
            matches, expected, actual = verify_file(flash_client, args.address, args.input)
            print(f"expected SHA-256 {expected}")
            print(f"actual   SHA-256 {actual}")
            print("verified" if matches else "mismatch")
            return 0 if matches else 2
        if args.flash_command == "erase-sector":
            backup = erase_sector(
                flash_client,
                address=args.address,
                backup_path=args.backup,
                confirmed=args.yes,
            )
            print(f"sector erase verified; full-chip backup: {backup.path} ({backup.sha256})")
            return 0
        if args.flash_command == "write":
            backup = write_file(
                flash_client,
                address=args.address,
                input_path=args.input,
                backup_path=args.backup,
                confirmed=args.yes,
            )
            print(f"write verified; full-chip backup: {backup.path} ({backup.sha256})")
            return 0
    raise AssertionError(f"unhandled flash command {args.flash_command} via {device_path}")


def _run_flash_probe(dev: SCSITransport) -> int:
    mismatches = check_rom_api_signatures(dev)
    if mismatches:
        raise ProtocolUnavailable("; ".join(mismatches))
    print("firmware SPI API signatures match; no RAM upload or SPI operation was sent")
    return 0


def _run(args: argparse.Namespace) -> int:
    if args.command == "rom-rescue":
        return rom_rescue.run_cli(args)
    if args.command == "trace":
        from . import trace as flight_recorder

        data = args.file.read_bytes()
        if len(data) == 2 * 1024 * 1024:
            data = data[flight_recorder.SECTOR:flight_recorder.SECTOR + flight_recorder.SECTOR_SIZE]
        print(flight_recorder.format_report(flight_recorder.decode(data)))
        return 0
    if args.command in ("sdk", "debug"):
        from .debug_cli import run
        return run(args)
    validate_camera_path(args.device)

    if args.command == "hw":
        if args.hw_command == "clock-trial":
            require_supported_loader_boost()
        if args.hw_command in ("clock-pair-trial", "clock-pair-hold"):
            require_unused_pair_payload(args.payload, verified_repeat=args.hw_command == "clock-pair-hold")
        if args.timeout <= 0:
            raise ValueError("--timeout must be positive")
        if args.hw_command == "clock-test" and args.register not in REGISTER_WHITELIST:
            raise ValueError("register is not in the diagnostic whitelist")
        if getattr(args, "normal_mode", False) and args.start_loader:
            raise ValueError("--normal-mode cannot be combined with --start-loader")
        for output in (args.json, getattr(args, "ram_output", None)):
            if output and output.expanduser().exists():
                raise FileExistsError(f"refusing to overwrite existing output: {output}")
            if output and not output.expanduser().parent.is_dir():
                raise FileNotFoundError(output.expanduser().parent)
        if args.json and getattr(args, "ram_output", None) and args.json.expanduser().resolve() == args.ram_output.expanduser().resolve():
            raise ValueError("JSON and RAM snapshot require different output paths")
        if args.start_loader:
            path, dev, identity = enter_loader_with_device(args.device, timeout=args.timeout)
        else:
            path = args.device
            dev, identity = _camera_transport(path)
        if getattr(args, "normal_mode", False):
            dev = NormalMemoryTransport(dev)
        try:
            return _run_hw(dev, identity, args, path)
        finally:
            dev.close()

    if args.command == "loader":
        if args.timeout <= 0:
            raise ValueError("--timeout must be positive")
        path = enter_loader(args.device, timeout=args.timeout)
        print(f"camera is in Buildwin Video050Loader mode at {path}")
        return 0

    if args.command == "ram" and args.start_loader:
        path, dev, identity = enter_loader_with_device(args.device, timeout=args.timeout)
        try:
            if args.length <= 0:
                raise ValueError("RAM dump length must be positive")
            dump_ram_file(dev, identity, args.output, args.address, args.length)
            print(
                f"wrote {args.length} bytes from 0x{args.address:08x} to "
                f"{args.output.expanduser().resolve()} via {path}"
            )
            return 0
        finally:
            dev.close()

    if args.command == "payload" and args.payload_command == "marker" and args.start_loader:
        path, dev, identity = enter_loader_with_device(args.device, timeout=args.timeout)
        try:
            require_loader(identity)
            marker_result = run_marker_payload(
                dev,
                args.image,
                experimental=args.experimental,
                confirmed=args.yes,
            )
            print(f"volatile APXT marker written and scratch RAM restored via {path}")
            if args.report_u32:
                print(f"payload u32: 0x{int.from_bytes(marker_result[4:8], 'little'):08x}")
            return 0
        finally:
            dev.close()

    if args.command == "flash" and args.start_loader:
        if args.timeout <= 0:
            raise ValueError("--timeout must be positive")
        path, dev, identity = enter_loader_with_device(args.device, timeout=args.timeout)
        try:
            require_loader(identity)
            if args.flash_command == "probe":
                return _run_flash_probe(dev)
            return _run_flash(dev, args, path)
        finally:
            dev.close()

    if args.command == "flash" and args.flash_command == "probe":
        dev, identity = _camera_transport(args.device)
        try:
            require_loader(identity)
            return _run_flash_probe(dev)
        finally:
            dev.close()

    if args.command == "raw-cdb":
        dev, _identity = _camera_transport(args.device)
        try:
            if args.direction == "none":
                transfer = dev.execute(args.cdb)
                print(f"no-data CDB completed; residual={transfer.residual}")
                return 0 if transfer.residual == 0 else 2
            if args.direction == "in":
                return _raw_data_in(dev, args)
            return _raw_data_out(dev, args)
        finally:
            dev.close()

    dev = LinuxSCSITransport(args.device)
    try:
        identity = inquiry(dev)
        if args.command == "info":
            print(f"Device:   {args.device}")
            print(f"Vendor:   {identity.vendor}")
            print(f"Product:  {identity.product}")
            print(f"Revision: {identity.revision}")
            if identity.is_loader:
                print("Mode:     Buildwin ROM loader (1908:3319 family)")
            elif identity.is_buildwin_camera:
                print("Mode:     normal camera firmware")
            else:
                print("Mode:     unknown SCSI target")
            return 0

        require_camera(identity)

        if args.command == "ram":
            require_loader(identity)
            if args.length <= 0:
                raise ValueError("RAM dump length must be positive")
            dump_ram_file(dev, identity, args.output, args.address, args.length)
            print(
                f"wrote {args.length} bytes from 0x{args.address:08x} to "
                f"{args.output.expanduser().resolve()} (chunk <= 0x{RAM_READ_CHUNK:x})"
            )
            return 0

        if args.command == "payload":
            require_loader(identity)
            marker_result = run_marker_payload(
                dev,
                args.image,
                experimental=args.experimental,
                confirmed=args.yes,
            )
            print("volatile APXT marker written and scratch RAM restored")
            if args.report_u32:
                print(f"payload u32: 0x{int.from_bytes(marker_result[4:8], 'little'):08x}")
            return 0

        if args.command == "flash":
            require_loader(identity)
            return _run_flash(dev, args, args.device)

        raise AssertionError(f"unhandled command {args.command}")
    finally:
        dev.close()


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        return _run(args)
    except (ApxError, OSError, ValueError) as exc:
        print(f"apxtool: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
