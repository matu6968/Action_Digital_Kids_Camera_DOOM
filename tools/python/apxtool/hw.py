"""Bounded JT5295 diagnostics and retained, CLI-disabled clock-trial research.

The inspect, ram-test and clock-test commands never write clocks, timers or SPI.
"""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import time
from typing import Callable

from .errors import ApxError, ProtocolUnavailable, UnsafeOperation
from .protocol import RAM_BASE, RAM_LIMIT, RAM_WRITE_CHUNK, RAM_TRANSFER_FUNCTION, build_ram_transfer_cdb
from .ram import read_ram, validate_ram_range, write_ram_experimental
from .spi import PAYLOAD_ADDRESS, SCRATCH_SIZE, CONTROL_ADDRESS, CONTROL_SLOT_COUNT, CALLBACK_STACK_ADDRESS, CALLBACK_STACK_SIZE
from .transport import SCSITransport, inquiry, require_complete

NORMAL_TRANSFER_FUNCTION = 0x0203BDF4


class NormalMemoryTransport:
    """Translate the proven RAM CDB layout to the stock firmware's 0xcd helper."""
    def __init__(self, transport: SCSITransport):
        self.transport = transport

    def execute(self, cdb: bytes, *, data_out: bytes | None = None, data_in_length: int = 0):
        if cdb[0] == 0xCB:
            if len(cdb) != 16 or int.from_bytes(cdb[1:5], "little") != RAM_TRANSFER_FUNCTION:
                raise ValueError("normal-mode adapter accepts only the proven RAM transfer layout")
            request = bytearray(cdb)
            request[0] = 0xCD
            request[1:5] = NORMAL_TRANSFER_FUNCTION.to_bytes(4, "little")
            cdb = bytes(request)
        elif cdb[0] != 0x12:
            raise ValueError("normal-mode diagnostic adapter only accepts RAM transfers and INQUIRY")
        return self.transport.execute(cdb, data_out=data_out, data_in_length=data_in_length)

    def close(self) -> None:
        self.transport.close()


def require_diagnostic_mode(transport: SCSITransport, mode: str) -> None:
    identity = inquiry(transport)
    if mode == "loader" and identity.is_loader:
        return
    if mode == "normal" and identity.is_buildwin_camera and not identity.is_loader:
        return
    raise ProtocolUnavailable(f"camera is no longer in {mode} mode")

BLOCK_SIZE = 64
CMD_BRANCH = 1
CMD_REGISTER = 2
CMD_LOOP = 3
CMD_SAMPLE = 4
CMD_TIMED_LOOP = 5
CMD_CLOCK_TRIAL = 6
CMD_CLOCK_PAIR = 8
CMD_PAIR_HOLD = 9
CMD_PAIR_RESTORE = 10
CMD_PAIR_CAPABILITY = 11
PAIR_PAYLOAD_ADDRESS = 0x02185000
HOLD_PAYLOAD_ADDRESS = 0x02188000
RUNTIME_CLOCK_ADDRESS = 0x02170000
RUNTIME_CLOCK_SCRATCH_SIZE = 0x5000
RUNTIME_CLOCK_CONTROL_ADDRESS = 0x02171000
PAIR_BASELINE = {0x8848: 0x01014003, 0x884C: 0x80000001, 0x8858: 0x1623,
                 0x885C: 0x0B71B000, 0x8860: 0x1623, 0x8864: 0x0BEBC200,
                 0x8868: 0x044DF82E, 0x9800: 0x721, 0x9808: 0x3FFF}


def pair_attempt_path(image: bytes) -> Path:
    return Path(__file__).resolve().parents[2] / "research" / ("clock-pair-attempt-" + hashlib.sha256(image).hexdigest()[:16] + ".json")


def next_pair_receipt(image: bytes, *, verified_repeat: bool = False) -> Path:
    path = pair_attempt_path(image)
    if not path.exists():
        return path
    hold = path.with_name(path.stem + "-hold.json")
    previous = json.loads(path.read_text())
    held = json.loads(hold.read_text()) if hold.exists() else {}
    repeated = path.with_name(path.stem + "-repeat-2.json")
    if (verified_repeat and previous.get("status") == "completed" and previous.get("clock_restoration_verified") is True
            and held.get("status") == "completed" and held.get("clock_restoration_verified") is True
            and held.get("ram_restoration_verified") is True
            and not repeated.exists()):
        return repeated
    raise ProtocolUnavailable(f"this exploratory payload was already attempted; failed/unknown attempts cannot repeat: {path}")


def require_unused_pair_payload(image_path: Path, *, verified_repeat: bool = False) -> None:
    next_pair_receipt(image_path.expanduser().read_bytes(), verified_repeat=verified_repeat)
REGISTER_WHITELIST = (0x0002, 0x0018, 0x880C, 0x8840, 0x8858, 0x885C,
                      0x8848, 0x884C, 0x8860, 0x8864, 0x8868, 0x9800, 0x9804, 0x9808,
                      0x9840, 0xF860, 0xF818, 0xF81C)
# All have returned valid isolated APXH results; deliberately excludes earlier
# failed CPUCFGR, SR and clock-gate probes. No register-range sweep.
CLOCK_PROFILE_REGISTERS = (0x8848, 0x884C, 0x8858, 0x885C, 0x8860, 0x8864,
                           0x8868, 0x9800, 0x9808)
# Vendor read accesses, reviewed in raw instructions before adding these probes.
# SPI locations are file offsets, not bootstrap execution addresses.
CLOCK_DEPENDENCY_REGISTERS = {
    0x9840: {"role": "USB PHY control candidate", "rom_read": 0x00105D70},
    0xF860: {"role": "memory-controller initialization status candidate", "spi_read_offset": 0x1A88},
    0xF818: {"role": "memory-controller timing/status candidate", "spi_read_offset": 0x1848},
    0xF81C: {"role": "memory-controller calibration candidate", "spi_read_offset": 0x11C0},
}
LOADER_BOOST_BLOCKERS = (
    "PLL1 control field [10:8] changes 6 -> 1; its live-update semantics are unresolved",
    "0x884c bit 31 is set during loader USB initialization; its clock-source relationship is unresolved",
    "memory-controller clock source and live retiming/self-refresh sequence are unresolved",
    "vendor initialization is not a verified live transition/rollback with an active USB session",
)


def require_supported_loader_boost() -> None:
    """Fail closed until a supported transition replaces the historical trials."""
    raise ProtocolUnavailable("clock-trial is disabled: " + "; ".join(LOADER_BOOST_BLOCKERS))


def compare_clock_profiles(normal: dict, loader: dict) -> dict:
    """Offline comparison; matching end states never authorize a live write."""
    for report, mode in ((normal, "normal"), (loader, "loader")):
        if report.get("device_mode") != mode or report.get("status") != "completed":
            raise ValueError(f"requires a completed {mode} profile")
        if not any(e.get("kind") == "restoration" and e.get("verified") for e in report.get("events", [])):
            raise ValueError(f"{mode} profile lacks verified scratch restoration")
    registers = CLOCK_PROFILE_REGISTERS + tuple(CLOCK_DEPENDENCY_REGISTERS)
    values = []
    for register in registers:
        key = f"0x{register:04x}"
        n, l = normal.get("registers", {}).get(key), loader.get("registers", {}).get(key)
        if n is None or l is None:
            raise ValueError(f"incomplete clock profiles: missing {key}")
        values.append({"register": key, "normal": n, "loader": l, "xor": n ^ l})
    normal_control, loader_control = normal["registers"]["0x8858"], loader["registers"]["0x8858"]
    return {"register_comparison": values,
            "pll_control_field": {"mask": 0x700, "shift": 8,
                                  "normal": (normal_control >> 8) & 7,
                                  "loader": (loader_control >> 8) & 7,
                                  "arithmetic_difference": loader_control - normal_control,
                                  "xor": loader_control ^ normal_control},
            "clock_writes_enabled": False,
            "blocking_dependencies": list(LOADER_BOOST_BLOCKERS),
            "confidence": "end-state registers measured; live transition and rollback unsupported"}
TEST_START = 0x02140000
TEST_END = 0x023F0000
PROTECTED = (
    (RAM_BASE, TEST_START),
    (RUNTIME_CLOCK_ADDRESS, RUNTIME_CLOCK_ADDRESS + RUNTIME_CLOCK_SCRATCH_SIZE),
    (PAYLOAD_ADDRESS, PAYLOAD_ADDRESS + SCRATCH_SIZE),
    (0x02200000, 0x02204000),
    (TEST_END, RAM_LIMIT),
)


def event(report: dict, kind: str, **fields: object) -> None:
    report.setdefault("events", []).append({"kind": kind, **fields})
    journal = report.get("_journal")
    if journal:
        journal()


def new_report(command: str, device: str, mode: str) -> dict:
    return {
        "schema_version": 1,
        "command": command,
        "device": device,
        "device_mode": mode,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "status": "running",
        "cpu_hz": None,
        "cpu_hz_estimate": None,
        "crystal_marking": "32.768",
        "crystal_candidate_hz": [32768, 32768000],
        "confidence": "unresolved",
        "events": [],
    }


def validate_test_range(address: int, length: int) -> None:
    validate_ram_range(address, length)
    if length <= 0 or address % 4 or length % 4:
        raise ValueError("RAM test ranges must be nonempty and word-aligned")
    if any(address < end and address + length > start for start, end in PROTECTED):
        raise UnsafeOperation("RAM test overlaps code, reserved data, stack, or diagnostic scratch")


def inspect_snapshot(snapshot: bytes) -> dict:
    if len(snapshot) != RAM_LIMIT - RAM_BASE:
        raise ValueError("inspection requires a complete 4 MiB snapshot")
    # This rejects simple modulo mirrors, not arbitrary bank-dependent aliasing.
    mirrors = []
    for bits in range(12, 22):
        stride = 1 << bits
        mirrors.append({"stride": stride, "identical": snapshot[:-stride] == snapshot[stride:]})
    pages = [
        {"address": RAM_BASE + offset, "nonzero_bytes": sum(bool(v) for v in snapshot[offset:offset + 0x10000])}
        for offset in range(0, len(snapshot), 0x10000)
    ]
    return {
        "ram_window": {"base": RAM_BASE, "end_exclusive": RAM_LIMIT, "bytes": len(snapshot)},
        "ram_sha256": hashlib.sha256(snapshot).hexdigest(),
        "runtime_code_sha256": hashlib.sha256(snapshot[:0x5CB00]).hexdigest(),
        "simple_mirror_comparisons": mirrors,
        "pages_64k": pages,
        "memory_evidence": {
            "copied_image_end": 0x02090334,
            "application_bss_clear": [0x02093D94, 0x023FEC00],
            "allocator_candidate": [0x020AD200, 0x023FEC00],
            "allocator_candidate_bytes": 0x351A00,
            "startup_stack_pointer": 0x023FFFFC,
            "scope": "static boundaries; allocator ownership and stock-mode free bytes unmeasured",
        },
        "physical_ram_bytes": None,
        "confidence": "4 MiB mapped window; physical capacity not yet verified",
    }


def restore_regions(transport: SCSITransport, regions: list[tuple[int, bytes]], report: dict,
                    *, budget: float = 20.0, clock: Callable[[], float] = time.monotonic) -> None:
    """One responsiveness check, no retries; stop at the first failed restore."""
    deadline = clock() + budget
    try:
        if report.get("clock_restoration_verified") is False:
            raise UnsafeOperation("clock restoration is unverified; preserving diagnostic RAM for recovery")
        require_diagnostic_mode(transport, report.get("device_mode", "loader"))
        for address, original in reversed(regions):
            for offset in range(0, len(original), RAM_WRITE_CHUNK):
                if clock() >= deadline:
                    raise ProtocolUnavailable("restoration time budget exhausted")
                write_ram_experimental(transport, address + offset, original[offset:offset + RAM_WRITE_CHUNK])
            for offset in range(0, len(original), 0x10000):
                if clock() >= deadline:
                    raise ProtocolUnavailable("restoration verification time budget exhausted")
                block = original[offset:offset + 0x10000]
                if read_ram(transport, address + offset, len(block)) != block:
                    raise ApxError(f"restoration readback mismatch at 0x{address + offset:08x}")
        event(report, "restoration", verified=True, regions=len(regions))
    except Exception as exc:
        event(report, "restoration", verified=False, error=str(exc), recovery="power cycle required before further probes")
        raise ApxError(f"volatile restoration failed: {exc}; power cycle before further probes") from exc


class HardwareSession:
    def __init__(self, transport: SCSITransport, image_path: Path, *, experimental: bool,
                 confirmed: bool, report: dict, stack: bool = False, exploratory_pair: bool = False, pair_hold: bool = False,
                 runtime_clock: bool = False):
        if not experimental or not confirmed:
            raise UnsafeOperation("hardware payload requires both --experimental and --yes")
        self.transport = transport
        self.image = image_path.expanduser().read_bytes()
        if not self.image or len(self.image) > 0x1000 or len(self.image) % 4:
            raise ValueError("hardware payload must be word-aligned and fit below 0x02181000")
        self.report = report
        self.exploratory_pair = exploratory_pair
        self.pair_hold = pair_hold
        if pair_hold and not exploratory_pair:
            raise UnsafeOperation("hold requires an exploratory pair session")
        if runtime_clock and not (pair_hold and exploratory_pair):
            raise UnsafeOperation("runtime clock requires the checked hold payload")
        self.payload_address = RUNTIME_CLOCK_ADDRESS if runtime_clock else HOLD_PAYLOAD_ADDRESS if pair_hold else PAIR_PAYLOAD_ADDRESS if exploratory_pair else PAYLOAD_ADDRESS
        self.scratch_address = RUNTIME_CLOCK_ADDRESS if runtime_clock else PAYLOAD_ADDRESS
        self.scratch_size = RUNTIME_CLOCK_SCRATCH_SIZE if runtime_clock else SCRATCH_SIZE
        self.control_address = RUNTIME_CLOCK_CONTROL_ADDRESS if runtime_clock else CONTROL_ADDRESS
        self.control_slot_count = (self.scratch_address + self.scratch_size - self.control_address) // 0x100
        if exploratory_pair:
            certificate = json.loads(image_path.expanduser().with_suffix(".checked.json").read_text())
            if (report.get("device_mode") != "loader" or stack or certificate.get("clock_pair") is not True
                    or certificate.get("payload_address") != self.payload_address
                    or certificate.get("sha256") != hashlib.sha256(self.image).hexdigest()
                    or certificate.get("commands") != ([8, 9, 10, 11] if pair_hold else [8, 11])):
                raise UnsafeOperation("exploratory pair payload is not checked or is not in loader mode")
        self.original: bytes | None = None
        self.started = False
        self.failed = False
        self.slot = 0
        self.use_stack = stack
        self.original_stack: bytes | None = None
        self.pair_used = False
        self.hold_used = False
        self.restore_used = False

    def __enter__(self) -> "HardwareSession":
        if self.report.get("clock_restoration_verified") is False:
            raise UnsafeOperation("clock restoration is unverified; power cycle before another diagnostic session")
        require_diagnostic_mode(self.transport, self.report.get("device_mode", "loader"))
        self.original = read_ram(self.transport, self.scratch_address, self.scratch_size)
        if any(self.original):
            raise UnsafeOperation("diagnostic scratch is not all zero; refusing upload")
        if self.use_stack:
            self.original_stack = read_ram(self.transport, CALLBACK_STACK_ADDRESS, CALLBACK_STACK_SIZE)
            if any(self.original_stack):
                raise UnsafeOperation("clock-trial callback stack is occupied; refusing upload")
        event(self.report, "payload", sha256=hashlib.sha256(self.image).hexdigest(), bytes=len(self.image), address=self.payload_address)
        self.started = True
        try:
            write_ram_experimental(self.transport, self.payload_address, self.image)
            if read_ram(self.transport, self.payload_address, len(self.image)) != self.image:
                raise ApxError("diagnostic upload readback mismatch")
            read_ram(self.transport, RAM_BASE, 0x2000)
            return self
        except BaseException as exc:
            self.failed = True
            self.__exit__(type(exc), exc, None)
            raise

    def __exit__(self, _type, exc, _traceback) -> bool:
        if self.started and self.original is not None:
            # Mark closed before cleanup so a failing cleanup cannot be retried.
            self.started = False
            try:
                regions = [(self.scratch_address, self.original)]
                if self.original_stack is not None:
                    regions.append((CALLBACK_STACK_ADDRESS, self.original_stack))
                restore_regions(self.transport, regions, self.report)
            except Exception as restore_error:
                if exc is not None:
                    raise ApxError(f"hardware operation failed: {exc}; {restore_error}") from exc
                raise
        return False

    def rpc(self, command: int, arg: int = 0, count: int = 0) -> bytes:
        if not self.started or self.failed:
            raise ProtocolUnavailable("diagnostic session is closed or failed; callbacks cannot be retried")
        if command not in (CMD_BRANCH, CMD_REGISTER, CMD_LOOP, CMD_SAMPLE, CMD_TIMED_LOOP, CMD_CLOCK_TRIAL, CMD_CLOCK_PAIR, CMD_PAIR_CAPABILITY, CMD_PAIR_HOLD, CMD_PAIR_RESTORE):
            raise ValueError("unknown diagnostic command")
        if command == CMD_REGISTER and arg not in REGISTER_WHITELIST:
            raise ValueError("register is not in the diagnostic whitelist")
        if command == CMD_SAMPLE:
            validate_test_range(arg, 16)
        if command in (CMD_LOOP, CMD_TIMED_LOOP) and not 1 <= count <= 1000000:
            raise ValueError("loop count must be between 1 and 1,000,000")
        if command == CMD_CLOCK_TRIAL:
            require_supported_loader_boost()
        if command in (CMD_CLOCK_PAIR, CMD_PAIR_CAPABILITY, CMD_PAIR_HOLD, CMD_PAIR_RESTORE):
            if not self.exploratory_pair or self.report.get("device_mode") != "loader":
                raise UnsafeOperation("new pair commands require the checked exploratory loader payload")
            if command == CMD_CLOCK_PAIR and (arg != 144 or count != 1000000):
                raise ValueError("only the fixed temporary 144 MHz pair measurement is allowed")
            if command == CMD_CLOCK_PAIR and self.pair_used:
                raise ProtocolUnavailable("pair trial has already been dispatched in this session")
            if command in (CMD_PAIR_HOLD, CMD_PAIR_RESTORE) and not self.pair_hold:
                raise UnsafeOperation("hold/restore requires its separately checked payload")
            if command == CMD_PAIR_HOLD and (arg != 144 or count != 1000000 or not self.pair_used or self.hold_used
                    or not self.report.get("clock_pair_trial") or self.report.get("clock_restoration_verified") is not True):
                raise UnsafeOperation("hold requires one successful temporary trial and cannot repeat")
            if command == CMD_PAIR_RESTORE and (arg != 96 or count != 0 or not self.hold_used or self.restore_used):
                raise UnsafeOperation("restore is limited to one rollback of the active hold")
        if self.slot >= self.control_slot_count:
            raise ProtocolUnavailable("fresh diagnostic response slots exhausted")
        address = self.control_address + self.slot * 0x100
        self.slot += 1
        sequence = self.slot
        request = bytearray(BLOCK_SIZE)
        request[:6] = b"APXH\x01" + bytes([command])
        request[8:12] = arg.to_bytes(4, "little")
        request[12:16] = count.to_bytes(4, "little")
        request[16:20] = sequence.to_bytes(4, "little")
        event(self.report, "dispatch", command=command, argument=arg, count=count,
              sequence=sequence, response_address=address)
        try:
            write_ram_experimental(self.transport, address, bytes(request))
            if read_ram(self.transport, address, BLOCK_SIZE) != request:
                raise ProtocolUnavailable("request readback mismatch or uncleared response")
            start = time.monotonic_ns()
            if command in (CMD_CLOCK_PAIR, CMD_PAIR_HOLD, CMD_PAIR_RESTORE):
                if command == CMD_CLOCK_PAIR:
                    self.pair_used = True
                elif command == CMD_PAIR_HOLD:
                    self.hold_used = True
                else:
                    self.restore_used = True
                self.report["clock_write_attempted"] = True
                self.report["clock_restoration_verified"] = False
                event(self.report, "exploratory_clock_dispatch", sequence=sequence)
            response = read_ram(self.transport, address, BLOCK_SIZE, callback_address=self.payload_address)
            elapsed = time.monotonic_ns() - start
            event(self.report, "response", sequence=sequence, raw_hex=response.hex(), elapsed_ns=elapsed)
            if (response[:6] != request[:6] or response[8:20] != request[8:20]
                    or response[6] != 0xA5 or response[7] != 0
                    or int.from_bytes(response[20:24], "little") != 4
                    or int.from_bytes(response[36:40], "little") != sequence):
                raise ProtocolUnavailable("incomplete, stale, or invalid diagnostic response")
            return response
        except BaseException as exc:
            self.failed = True
            event(self.report, "probe_failed", sequence=sequence, error=str(exc))
            # A responsive device may retain the stage before a clock access
            # disrupted the transfer. Capture it once, without another callback.
            try:
                require_diagnostic_mode(self.transport, self.report.get("device_mode", "loader"))
                partial = read_ram(self.transport, address, BLOCK_SIZE)
                event(self.report, "partial_response", sequence=sequence,
                      raw_hex=partial.hex(), stage=int.from_bytes(partial[20:24], "little"))
            except Exception as partial_error:
                event(self.report, "partial_response_unavailable", sequence=sequence, error=str(partial_error))
            raise

    def branch_test(self) -> int:
        result = int.from_bytes(self.rpc(CMD_BRANCH)[24:28], "little")
        if result not in (0, 1):
            self.failed = True
            raise ProtocolUnavailable("branch experiment returned an invalid result")
        self.report["branch_delay_slot_executed"] = bool(result)
        event(self.report, "branch_behavior", delay_slot_executed=bool(result))
        return result


def ram_test(session: HardwareSession, snapshot: bytes, report: dict) -> None:
    summary = inspect_snapshot(snapshot)
    report.update(summary)
    if any(item["identical"] for item in summary["simple_mirror_comparisons"]):
        raise UnsafeOperation("snapshot permits simple RAM mirroring; refusing pattern writes")
    addresses = []
    for address in range(TEST_START, TEST_END, 0x10000):
        try:
            validate_test_range(address, BLOCK_SIZE)
        except UnsafeOperation:
            continue
        addresses.append(address)
    originals = []
    for address in addresses:
        original = read_ram(session.transport, address, BLOCK_SIZE)
        if any(original) or original != snapshot[address - RAM_BASE:address - RAM_BASE + BLOCK_SIZE]:
            raise UnsafeOperation(f"candidate page 0x{address:08x} is occupied or changed since snapshot")
        originals.append((address, original))
    report["tested_ranges"] = [{"address": a, "length": 16, "guard_length": BLOCK_SIZE} for a in addresses]
    # Record every original before the first write, including partial-write failures.
    wrote = False
    try:
        for phase in (0, 1):
            expected = {}
            for address in addresses:
                seed = address ^ (0x13579BDF if phase == 0 else 0xECA86420)
                words = (seed, seed ^ 0xFFFFFFFF, seed ^ 0x55AA55AA, seed ^ 0xAA55AA55)
                block = b"".join(w.to_bytes(4, "little") for w in words)
                expected[address] = block
                wrote = True
                write_ram_experimental(session.transport, address, block)
            # Pressure on the loader data path and, independently, the CPU data path.
            read_ram(session.transport, RAM_BASE, 0x100000)
            for address, original in originals:
                response = session.rpc(CMD_SAMPLE, address)
                target = response[24:36] + response[52:56]
                host = read_ram(session.transport, address, BLOCK_SIZE)
                event(report, "ram_sample", phase=phase, address=address, target_hex=target.hex(), host_hex=host.hex())
                if target != expected[address] or host != expected[address] + original[16:]:
                    raise ApxError(f"RAM independence or guard check failed at 0x{address:08x}")
        report["ram_patterns_passed"] = True
        report["confidence"] = "distinct tested RAM addresses; cache eviction inferred, physical capacity not fully proven"
        report["cache_validation"] = "1 MiB unrelated reads in loader and CPU; no verified cache-control operation"
    finally:
        if wrote:
            restore_regions(session.transport, originals, report)


def analyze_clocks(report: dict) -> None:
    """Keep oscillator and CPU routing inferences separate from measured registers."""
    registers = report.get("registers", {})
    if "0x885c" in registers:
        word = registers["0x885c"]
        report["pll1_candidate_hz"] = word * 32768 / 65536
        report["pll1_alternative_hz_32mhz"] = word * 32768000 / 65536
        report["cpu_hz_estimate"] = report["pll1_candidate_hz"]
        report["clock_derivation"] = {
            "multiplier_format": "Q16.16 inferred from bootstrap constants and calibration",
            "reference_hz": 32768,
            "reference_confidence": "inferred from firmware constants and timer rate; crystal has no unit marking",
            "cpu_routing": "PLL1-to-CPU relationship remains inferred; not a verified CPU cycle count",
        }
        throughput = report.get("estimated_loops_per_second")
        if throughput:
            report["implied_cycles_per_iteration"] = report["cpu_hz_estimate"] / throughput
        report["confidence"] = "PLL1 frequency inferred; matching CPU clock is an estimate, not verified MHz"
    samples = report.get("timer_samples", [])
    if samples:
        # Prefer long loops to reduce USB overhead. Never interpret a wrapped
        # or stationary counter as a frequency measurement.
        rates = []
        control = report.get("timer_config", {}).get("control")
        reload = report.get("timer_config", {}).get("reload")
        for sample in samples if control == 0x721 and reload is not None and reload >= 0x3FFF else []:
            if sample["iterations"] == 1000000 and sample["after"] > sample["before"]:
                rates.append((sample["after"] - sample["before"]) * 1e9 / sample["elapsed_ns"])
        report["candidate_timer_hz_observed"] = statistics.median(rates) if rates else None
        report["timer_limit"] = "rate includes USB overhead; counter wrap/reload and clock routing not fully characterized"
        if control != 0x721 or reload is None or reload < 0x3FFF:
            report["timer_limit"] = "counter uses an uncalibrated/reloading profile; raw samples only, no rate inferred"


def clock_test(session: HardwareSession, report: dict, *, register: int = 0x8868,
               loop_only: bool = False, register_only: bool = False, timer_loop: bool = False) -> None:
    if register not in REGISTER_WHITELIST:
        raise ValueError("register is not in the diagnostic whitelist")
    if register_only:
        response = session.rpc(CMD_REGISTER, arg=register)
        report["registers"] = {f"0x{register:04x}": int.from_bytes(response[24:28], "little")}
        report["confidence"] = "raw register measured; CPU clock unresolved"
        event(report, "register_value", register=register, value=report["registers"][f"0x{register:04x}"])
        analyze_clocks(report)
        return
    timings = []
    if timer_loop:
        report["timer_config"] = {}
        for label, current in (("control", 0x9800), ("reload", 0x9808)):
            response = session.rpc(CMD_REGISTER, arg=current)
            report["timer_config"][label] = int.from_bytes(response[24:28], "little")
    for count in (10000, 100000, 1000000):
        elapsed = []
        for _ in range(3):
            response = session.rpc(CMD_TIMED_LOOP if timer_loop else CMD_LOOP, count=count)
            if int.from_bytes(response[24:28], "little") != count:
                raise ApxError("loop completion count mismatch")
            elapsed.append(report["events"][-1]["elapsed_ns"])
            if timer_loop:
                before = int.from_bytes(response[28:32], "little")
                after = int.from_bytes(response[32:36], "little")
                report.setdefault("timer_samples", []).append({"counter_spr": 0x9804, "iterations": count,
                    "before": before, "after": after, "delta_mod32": (after - before) & 0xFFFFFFFF,
                    "elapsed_ns": elapsed[-1]})
        timings.append({"iterations": count, "elapsed_ns": elapsed, "median_ns": statistics.median(elapsed)})
    report["loop_timings"] = timings
    slope = (timings[-1]["median_ns"] - timings[0]["median_ns"]) / 990000
    report["estimated_loops_per_second"] = 1e9 / slope if slope > 0 else None
    report["timing_limit"] = "callback round-trip includes USB/host overhead; loop throughput is not CPU Hz"
    report["clock_limit"] = "oscillator/PLL/divider semantics and independent timer reference not established"
    event(report, "loop_summary", timings=timings, estimated_loops_per_second=report["estimated_loops_per_second"])
    if not loop_only:
        report["registers"] = {}
        # Each read is isolated, journaled, and stops the session on failure.
        for current in ((register, 0x8858, 0x885C) if register == 0x8868 else (register,)):
            response = session.rpc(CMD_REGISTER, arg=current)
            value = int.from_bytes(response[24:28], "little")
            report["registers"][f"0x{current:04x}"] = value
            event(report, "register_value", register=current, value=value)
    report["confidence"] = "loop throughput measured; CPU clock unresolved"
    analyze_clocks(report)


def clock_profile(session: HardwareSession, report: dict, *, include_dependencies: bool = False) -> None:
    """Comparable mode snapshots, with a separate fresh callback per SPR."""
    report["registers"] = {}
    report["profile_limit"] = "sequential isolated reads, not an atomic clock-tree snapshot"
    registers = CLOCK_PROFILE_REGISTERS + (tuple(CLOCK_DEPENDENCY_REGISTERS) if include_dependencies else ())
    report["register_sources"] = {f"0x{r:04x}": source for r, source in CLOCK_DEPENDENCY_REGISTERS.items()
                                  if include_dependencies}
    for register in registers:
        response = session.rpc(CMD_REGISTER, arg=register)
        value = int.from_bytes(response[24:28], "little")
        report["registers"][f"0x{register:04x}"] = value
        event(report, "register_value", register=register, value=value)
    analyze_clocks(report)


def clock_pair_trial(session: HardwareSession, report: dict, *, receipt_path: Path | None = None) -> None:
    """One explicitly authorized experiment; successful end states do not remove uncertainty."""
    if not session.exploratory_pair or report.get("device_mode") != "loader":
        raise UnsafeOperation("clock pair trial requires its checked experimental loader session")
    capability = session.rpc(CMD_PAIR_CAPABILITY)
    if int.from_bytes(capability[24:28], "little") != (0x50414932 if session.pair_hold else 0x50414952):
        raise ProtocolUnavailable("new clock-pair capability was not executed; clock writes refused")
    before = {}
    for register, expected in PAIR_BASELINE.items():
        response = session.rpc(CMD_REGISTER, arg=register)
        before[f"0x{register:04x}"] = int.from_bytes(response[24:28], "little")
        if before[f"0x{register:04x}"] != expected:
            raise UnsafeOperation(f"exploratory pair baseline mismatch at SPR {register:#x}; clock writes refused")
    report["clock_registers_before"] = before
    report["experimental_clock_limit"] = "live PLL handoff and memory/USB dependencies unresolved; owner authorized hard-reset recovery"
    receipt_path = receipt_path or next_pair_receipt(session.image, verified_repeat=session.pair_hold)
    receipt = {"status": "reserved", "payload_sha256": hashlib.sha256(session.image).hexdigest(),
               "started_utc": datetime.now(timezone.utc).isoformat(), "baseline": before,
               "scope": "one temporary stock-pair transition; sustained loader clock untested"}
    with receipt_path.open("x") as stream:
        json.dump(receipt, stream, indent=2)
        stream.flush()
        import os
        os.fsync(stream.fileno())
    event(report, "exploratory_pair_reserved", receipt=str(receipt_path))
    try:
        response = session.rpc(CMD_CLOCK_PAIR, arg=144, count=1000000)
        elapsed_ns = report["events"][-1]["elapsed_ns"]
        if (int.from_bytes(response[28:32], "little") != 0x0B71B000
                or int.from_bytes(response[60:64], "little") != 0x52455354):
            raise ProtocolUnavailable("pair rollback marker/coefficient not verified; power cycle required")
        after = {}
        for register in PAIR_BASELINE:
            current = session.rpc(CMD_REGISTER, arg=register)
            after[f"0x{register:04x}"] = int.from_bytes(current[24:28], "little")
        report["clock_registers_after"] = after
        if after != before:
            raise ProtocolUnavailable("pair trial did not restore sampled clocks; power cycle required")
        report["clock_restoration_verified"] = True
        event(report, "clock_restoration", verified=True)
        if (int.from_bytes(response[24:28], "little") != 0x112A8800
                or int.from_bytes(response[40:44], "little") != 0x1123):
            raise ProtocolUnavailable("temporary stock coefficient/control pair was not observed")
        words = [int.from_bytes(response[offset:offset + 4], "little") for offset in (32, 48, 52, 56)]
        period = before["0x9808"] + 1
        baseline_ticks, trial_ticks = (words[1] - words[0]) % period, (words[3] - words[2]) % period
        if not baseline_ticks or not trial_ticks:
            raise ProtocolUnavailable("pair counter did not advance; frequency unresolved")
        if elapsed_ns >= period * 1e9 / 32768:
            raise ProtocolUnavailable("pair round-trip spans a timer period; wrap ambiguity prevents accepting the timing")
        report["clock_pair_trial"] = {"baseline_ticks": baseline_ticks, "trial_ticks": trial_ticks,
                                      "speedup": baseline_ticks / trial_ticks, "loop_iterations": 1000000,
                                      "coefficient_during_trial": 0x112A8800, "control_during_trial": 0x1123,
                                      "clock_registers_restored": True, "sustained_loader_usb": "not tested"}
        report["confidence"] = "temporary stock pair observed and rollback measured; absolute CPU MHz and sustained USB remain unverified"
        receipt.update(status="completed", clock_restoration_verified=True, measurements=report["clock_pair_trial"])
        event(report, "clock_pair_result", **report["clock_pair_trial"])
    except BaseException as exc:
        receipt.update(status="failed", error=str(exc), clock_restoration_verified=report.get("clock_restoration_verified", False))
        event(report, "exploratory_pair_stopped", error=str(exc), automatic_repeat=False)
        raise
    finally:
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")


def clock_pair_hold(session: HardwareSession, report: dict, *, receipt_path: Path | None = None,
                    clock: Callable[[], float] = time.monotonic, sleeper: Callable[[float], None] = time.sleep) -> None:
    """Thirty seconds and 100 callbacks; one rollback, no retry after a failed probe."""
    if not session.pair_hold:
        raise UnsafeOperation("hold requires its checked dedicated payload")
    trial_receipt = receipt_path or next_pair_receipt(session.image, verified_repeat=True)
    clock_pair_trial(session, report, receipt_path=trial_receipt)
    if not 1.4 <= report["clock_pair_trial"]["speedup"] <= 1.6:
        raise ProtocolUnavailable("temporary performance ratio is unexpected; hold refused")
    witness = 0x02160000
    validate_test_range(witness, BLOCK_SIZE)
    original = read_ram(session.transport, witness, BLOCK_SIZE)
    if any(original):
        raise UnsafeOperation("held-clock RAM witness is occupied; hold refused")
    before = report["clock_registers_before"]
    peak = dict(before, **{"0x8858": 0x1123, "0x885c": 0x112A8800})
    hold_path = trial_receipt.with_name(trial_receipt.stem + "-hold.json")
    report["hold_receipt_path"] = str(hold_path)
    receipt = {"status": "reserved", "duration_seconds": 30, "required_callbacks": 100,
               "payload_sha256": hashlib.sha256(session.image).hexdigest()}
    with hold_path.open("x") as stream:
        json.dump(receipt, stream, indent=2)
    active = False
    wrote = False

    def registers() -> dict:
        values = {}
        for register in PAIR_BASELINE:
            response = session.rpc(CMD_REGISTER, register)
            values[f"0x{register:04x}"] = int.from_bytes(response[24:28], "little")
        return values

    def rollback() -> None:
        response = session.rpc(CMD_PAIR_RESTORE, 96)
        if (int.from_bytes(response[28:32], "little") != 0x0B71B000
                or int.from_bytes(response[60:64], "little") != 0x52455354):
            raise ProtocolUnavailable("hold rollback marker was not verified; power cycle required")
        report["clock_registers_after_hold"] = registers()
        if report["clock_registers_after_hold"] != before:
            raise ProtocolUnavailable("hold rollback register mismatch; power cycle required")
        report["clock_restoration_verified"] = True
        event(report, "clock_restoration", verified=True, phase="hold")

    try:
        response = session.rpc(CMD_PAIR_HOLD, 144, 1000000)
        if (int.from_bytes(response[60:64], "little") != 0x484F4C44
                or int.from_bytes(response[24:28], "little") != 0x112A8800
                or int.from_bytes(response[40:44], "little") != 0x1123):
            raise ProtocolUnavailable("hold did not acknowledge the stock pair; power cycle required")
        active = True
        report["clock_registers_during_hold"] = registers()
        if report["clock_registers_during_hold"] != peak:
            raise ProtocolUnavailable("held clock register mismatch")
        start = clock()
        hold_ticks = []
        period = before["0x9808"] + 1
        for index in range(100):
            target = start + index * 30 / 99
            delay = target - clock()
            if delay > 0:
                sleeper(delay)
            if clock() > start + 35:
                raise ProtocolUnavailable("held-clock acceptance time budget exceeded")
            response = session.rpc(CMD_TIMED_LOOP, count=1000000)
            if int.from_bytes(response[24:28], "little") != 1000000:
                raise ProtocolUnavailable("held callback count mismatch")
            first = int.from_bytes(response[28:32], "little")
            last = int.from_bytes(response[32:36], "little")
            ticks = (last - first) % period
            elapsed_ns = report["events"][-1]["elapsed_ns"]
            if not ticks or elapsed_ns >= period * 1e9 / 32768:
                raise ProtocolUnavailable("held-loop timer is stalled or has ambiguous reloads")
            hold_ticks.append(ticks)
            event(report, "hold_loop_timing", callback=index + 1, iterations=1000000,
                  timer_before=first, timer_after=last, timer_ticks=ticks, elapsed_ns=elapsed_ns)
            if index % 20 == 0:
                pattern = b"".join((0x13579BDF ^ (index << 16) ^ offset).to_bytes(4, "little") for offset in range(4))
                wrote = True
                write_ram_experimental(session.transport, witness, pattern)
                sample = session.rpc(CMD_SAMPLE, witness)
                host = read_ram(session.transport, witness, BLOCK_SIZE)
                if sample[24:36] + sample[52:56] != pattern or host != pattern + original[16:]:
                    raise ProtocolUnavailable("held-clock RAM sample or adjacent guard mismatch")
                event(report, "hold_ram_sample", callback=index + 1, address=witness, pattern_hex=pattern.hex(), guards_verified=True)
                write_ram_experimental(session.transport, witness, original[:16])
                restored = read_ram(session.transport, witness, BLOCK_SIZE)
                sample = session.rpc(CMD_SAMPLE, witness)
                if restored != original or sample[24:36] + sample[52:56] != original[:16]:
                    raise ProtocolUnavailable("held-clock RAM restoration or guard mismatch")
                event(report, "hold_ram_restore_verified", callback=index + 1, address=witness)
            event(report, "hold_callback", completed=index + 1, elapsed_seconds=clock() - start)
        elapsed = clock() - start
        report["clock_registers_end_hold"] = registers()
        if report["clock_registers_end_hold"] != peak or elapsed < 30:
            raise ProtocolUnavailable("held-clock acceptance end state/duration mismatch")
        speedup = report["clock_pair_trial"]["baseline_ticks"] / statistics.median(hold_ticks)
        if not 1.4 <= speedup <= 1.6:
            raise ProtocolUnavailable("sustained loop throughput does not support the expected clock increase")
        rollback()
        active = False
        report["clock_hold"] = {"duration_seconds": elapsed, "callbacks": 100, "ram_patterns": 5,
                               "guard_bytes": 48, "stock_pair_mhz_estimate": 144,
                               "registers_restored": True, "usb_callbacks_passed": True,
                               "ram_restores_during_hold": 5, "loop_iterations": 1000000,
                               "timer_ticks": hold_ticks, "median_timer_ticks": statistics.median(hold_ticks),
                               "sustained_loop_speedup": speedup,
                               "timer_reference_hz_inferred": 32768, "cpu_hz_verified": False}
        report["confidence"] = "stock pair held across USB callbacks and guarded RAM transfers; absolute CPU MHz inferred"
        receipt.update(status="completed", clock_restoration_verified=True, measurements=report["clock_hold"])
        event(report, "clock_hold_verified", **report["clock_hold"])
    except BaseException as exc:
        if active and not session.failed and not session.restore_used:
            try:
                rollback()
                active = False
            except Exception as error:
                event(report, "hold_rollback_failed", error=str(error))
        receipt.update(status="failed", error=str(exc), clock_restoration_verified=report.get("clock_restoration_verified", False))
        event(report, "hold_stopped", error=str(exc), automatic_repeat=False)
        raise
    finally:
        hold_path.write_text(json.dumps(receipt, indent=2) + "\n")
        if wrote and report.get("clock_restoration_verified") is True:
            restore_regions(session.transport, [(witness, original)], report)


def verify_clock_trial_rom(transport: SCSITransport, report: dict) -> None:
    """Retained signature check for the first failed BootROM transition trial."""
    path = Path(__file__).resolve().parents[2] / "research/jt5295-rom-00100000-64k.bin"
    expected = path.read_bytes()[0x44D8:0x4604]
    if len(expected) != 0x12C:
        raise ProtocolUnavailable("retained BootROM is incomplete")
    cdb = build_ram_transfer_cdb(function_address=RAM_TRANSFER_FUNCTION, memory_address=0x001044D8)
    actual = require_complete(transport.execute(cdb, data_in_length=len(expected)), len(expected), "PLL ROM signature")
    if actual != expected:
        raise ProtocolUnavailable("live PLL routine differs from the analyzed BootROM")
    event(report, "pll_rom_verified", address=0x001044D8, sha256=hashlib.sha256(actual).hexdigest())


def clock_trial(session: HardwareSession, report: dict) -> None:
    # Historical implementation retained below for RE, never dispatched.
    require_supported_loader_boost()
    if report.get("device_mode") != "loader":
        raise UnsafeOperation("clock trial is limited to loader mode")
    before = {}
    for register in (0x8858, 0x885C, 0x8848, 0x884C, 0x8868, 0x8860, 0x8864, 0x9800, 0x9808):
        response = session.rpc(CMD_REGISTER, arg=register)
        before[f"0x{register:04x}"] = int.from_bytes(response[24:28], "little")
    report["clock_registers_before"] = before
    if (before["0x8858"] != 0x1623 or before["0x885c"] != 0x0B71B000
            or before["0x8868"] != 0x044DF82E or before["0x9800"] != 0x721):
        raise UnsafeOperation("clock trial baseline differs from the measured 96 MHz loader profile")
    event(report, "clock_trial_begin", target_mhz=144, restore_before_usb_return=True)
    try:
        response = session.rpc(CMD_CLOCK_TRIAL, arg=144, count=1000000)
    except BaseException:
        report["clock_restoration_verified"] = False
        event(report, "clock_state_unverified", recovery="inspect clocks or power cycle; RAM restoration alone does not verify clocks")
        raise
    boosted = int.from_bytes(response[24:28], "little")
    restored = int.from_bytes(response[28:32], "little")
    if boosted != 0x112A8800 or restored != 0x0B71B000 or int.from_bytes(response[60:64], "little") != 0x52455354:
        raise ApxError("clock trial coefficient or restoration marker mismatch")
    start, end = int.from_bytes(response[32:36], "little"), int.from_bytes(response[48:52], "little")
    high_start, high_end = int.from_bytes(response[52:56], "little"), int.from_bytes(response[56:60], "little")
    period = before["0x9808"] + 1
    # Both bounded loops are under one reload period on the known 32.768 kHz profile.
    base_ticks, high_ticks = (end - start) % period, (high_end - high_start) % period
    after = {}
    for register in (0x8858, 0x885C, 0x8848, 0x884C, 0x8868, 0x8860, 0x8864, 0x9800, 0x9808):
        current = session.rpc(CMD_REGISTER, arg=register)
        after[f"0x{register:04x}"] = int.from_bytes(current[24:28], "little")
    report["clock_registers_after"] = after
    if before != after:
        raise ApxError("clock trial did not restore all sampled clock/timer registers")
    report["clock_restoration_verified"] = True
    if not base_ticks or not high_ticks:
        raise ApxError("clock trial counter did not advance")
    report["clock_trial"] = {"baseline_mhz_estimate": 96, "trial_mhz_estimate": 144,
        "baseline_ticks": base_ticks, "trial_ticks": high_ticks, "speedup": base_ticks / high_ticks,
        "registers_restored": True, "coefficient_during_trial": boosted,
        "scope": "temporary change inside one callback; sustained loader USB operation at 144 MHz untested"}
    report["confidence"] = "temporary 144 MHz PLL transition and performance change measured; original clocks restored"
    event(report, "clock_trial_verified", **report["clock_trial"])
