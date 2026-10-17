"""Build-selected clocks for operational payload sessions; diagnostics stay neutral."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from uuid import uuid4

from .errors import ApxError, ProtocolUnavailable, UnsafeOperation
from .protocol import RAM_BASE
from .ram import read_ram

ACCEPTED_CLOCK_SHA256 = "9b02950c8a7a18edf8971d3a5cffaaaffc45425fe09cfbc54e8b5567af2d2728"
ANALYZED_RUNTIME_SHA256 = "e45574603968448467a1519b7c71d179ebafc7c382f98b00b9767d671fcfd5a1"


def configured_clock(transport, payload_path: Path):
    """Historical/custom images without a policy retain the incoming clock."""
    path = payload_path.expanduser()
    policy_path = path.with_suffix(".clock.json")
    if not policy_path.exists():
        return None
    policy = json.loads(policy_path.read_text())
    if (policy.get("schema_version") != 1 or policy.get("cpu_mhz") not in (96, 144)
            or policy.get("payload_sha256") != hashlib.sha256(path.read_bytes()).hexdigest()
            or policy.get("clock_payload_sha256") != ACCEPTED_CLOCK_SHA256
            or policy.get("clock_payload") != "clock-runtime.bin"):
        raise UnsafeOperation("payload clock policy is unsupported or does not match the build")
    clock_path = path.parent / policy["clock_payload"]
    if hashlib.sha256(clock_path.read_bytes()).hexdigest() != ACCEPTED_CLOCK_SHA256:
        raise UnsafeOperation("runtime clock payload differs from the accepted instructions")
    session = PayloadClockSession(transport, clock_path, policy["cpu_mhz"])
    session.report.update(operational_payload=str(path.resolve()), operational_payload_sha256=policy["payload_sha256"])
    return session


class PayloadClockSession:
    def __init__(self, transport, image_path: Path, mhz: int, *, report_path: Path | None = None):
        from .hw import HardwareSession, new_report
        if mhz not in (96, 144):
            raise UnsafeOperation("only the 96 and 144 MHz build profiles are supported")
        if hashlib.sha256(image_path.read_bytes()).hexdigest() != ACCEPTED_CLOCK_SHA256:
            raise UnsafeOperation("runtime clock payload differs from the accepted instructions")
        self.transport = transport
        self.mhz = mhz
        self.report_path = report_path or Path(__file__).resolve().parents[2] / "research" / f"runtime-clock-{uuid4().hex}.json"
        self.report = new_report("payload-session-clock", getattr(transport, "path", "transport"), "loader")
        self.report.update(session_id=uuid4().hex, selected_cpu_mhz_estimate=mhz,
                           cpu_hz_estimate=mhz * 1000000,
                           clock_restoration_verified=True, clock_payload_sha256=ACCEPTED_CLOCK_SHA256)
        self.session = HardwareSession(transport, image_path, experimental=True, confirmed=True,
                                      report=self.report, exploratory_pair=True, pair_hold=True, runtime_clock=True)
        self.started = False
        self.closed = False
        self.active = False

    def _journal(self):
        temporary = self.report_path.with_suffix(".json.tmp")
        with temporary.open("w") as stream:
            json.dump({k: v for k, v in self.report.items() if not k.startswith("_")}, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(self.report_path)

    def _registers(self):
        from .hw import CMD_REGISTER, PAIR_BASELINE
        return {f"0x{r:04x}": int.from_bytes(self.session.rpc(CMD_REGISTER, r)[24:28], "little")
                for r in PAIR_BASELINE}

    def start(self):
        from .hw import CMD_PAIR_CAPABILITY, CMD_PAIR_HOLD, PAIR_BASELINE, clock_pair_trial, event, require_diagnostic_mode
        if self.started or self.closed:
            raise ProtocolUnavailable("payload clock session cannot be reused")
        with self.report_path.open("x"):
            pass
        self.report["_journal"] = self._journal
        self._journal()
        try:
            require_diagnostic_mode(self.transport, "loader")
            prefix = read_ram(self.transport, RAM_BASE, 0x5CB00)
            if hashlib.sha256(prefix).hexdigest() != ANALYZED_RUNTIME_SHA256:
                raise UnsafeOperation("runtime firmware differs from the analyzed image; clock setup refused")
            self.report["runtime_code_sha256"] = hashlib.sha256(prefix).hexdigest()
            self.session.__enter__()
            self.started = True
            self.session.branch_test()
            if self.mhz == 144:
                receipt = self.report_path.with_name(self.report_path.stem + "-trial.json")
                clock_pair_trial(self.session, self.report, receipt_path=receipt)
                if not 1.4 <= self.report["clock_pair_trial"]["speedup"] <= 1.6:
                    raise ProtocolUnavailable("temporary clock performance differs from acceptance; session refused")
                response = self.session.rpc(CMD_PAIR_HOLD, 144, 1000000)
                if (int.from_bytes(response[60:64], "little") != 0x484F4C44
                        or int.from_bytes(response[24:28], "little") != 0x112A8800
                        or int.from_bytes(response[40:44], "little") != 0x1123):
                    raise ProtocolUnavailable("runtime clock hold was not acknowledged; power cycle required")
                self.active = True
                self.report["clock_registers_during_payload"] = self._registers()
                peak = dict(self.report["clock_registers_before"], **{"0x8858": 0x1123, "0x885c": 0x112A8800})
                if self.report["clock_registers_during_payload"] != peak:
                    raise ProtocolUnavailable("runtime clock registers differ from the supported profile")
            else:
                capability = self.session.rpc(CMD_PAIR_CAPABILITY)
                if int.from_bytes(capability[24:28], "little") != 0x50414932:
                    raise ProtocolUnavailable("runtime clock capability response is invalid")
                before = self._registers()
                if before != {f"0x{r:04x}": v for r, v in PAIR_BASELINE.items()}:
                    raise UnsafeOperation("downclocked build requires the original 96 MHz loader baseline")
                self.report["clock_registers_before"] = before
                self.report["clock_registers_during_payload"] = before
            self.report["confidence"] = "selected PLL profile measured; CPU clock routing/reference remain inferred"
            event(self.report, "payload_clock_ready", mhz_estimate=self.mhz)
            return self
        except BaseException as error:
            self.report["status"] = "failed"
            self.report["error"] = str(error)
            try:
                self.close(error)
            except Exception as cleanup:
                raise ApxError(f"clock setup failed: {error}; {cleanup}") from error
            raise

    def close(self, operation_error=None):
        from .hw import CMD_PAIR_RESTORE, event, require_diagnostic_mode
        if self.closed:
            return
        self.closed = True
        try:
            if self.active:
                # One bounded restoration while identity and current profile support it.
                # A failed clock RPC already prevents further callbacks.
                require_diagnostic_mode(self.transport, "loader")
                peak = dict(self.report["clock_registers_before"], **{"0x8858": 0x1123, "0x885c": 0x112A8800})
                if self._registers() != peak:
                    raise ProtocolUnavailable("clock state changed during payload; preserving recovery RAM")
                response = self.session.rpc(CMD_PAIR_RESTORE, 96)
                if (int.from_bytes(response[28:32], "little") != 0x0B71B000
                        or int.from_bytes(response[60:64], "little") != 0x52455354):
                    raise ProtocolUnavailable("payload clock rollback marker is invalid; power cycle required")
                after = self._registers()
                if after != self.report["clock_registers_before"]:
                    raise ProtocolUnavailable("payload clock rollback register mismatch; power cycle required")
                self.report["clock_registers_after"] = after
                self.report["clock_restoration_verified"] = True
                self.active = False
                event(self.report, "clock_restoration", verified=True, phase="payload-session")
            elif self.started and self.mhz == 96 and "clock_registers_before" in self.report:
                require_diagnostic_mode(self.transport, "loader")
                self.report["clock_registers_after"] = self._registers()
                if self.report["clock_registers_after"] != self.report["clock_registers_before"]:
                    self.report["clock_restoration_verified"] = False
                    raise ProtocolUnavailable("downclocked payload changed the loader clock profile; power cycle required")
            if self.started:
                self.started = False
                self.session.__exit__(None, operation_error, None)
            if operation_error is not None:
                self.report.update(status="failed", error=str(operation_error))
            elif self.report["status"] != "failed":
                self.report["status"] = "completed"
        except BaseException as error:
            if self.started and self.mhz == 96 and "clock_registers_before" in self.report:
                self.report["clock_restoration_verified"] = False
            self.report.update(status="failed", error=str(error), recovery="power cycle before further probes")
            raise
        finally:
            self._journal()
