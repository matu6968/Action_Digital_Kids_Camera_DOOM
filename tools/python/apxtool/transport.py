from __future__ import annotations

from dataclasses import dataclass
import glob
import os
from pathlib import Path
import sys
import time
from typing import Callable, Protocol

from .errors import ApxError, DeviceError, TransferError


@dataclass(frozen=True)
class SCSITransfer:
    data: bytes = b""
    residual: int = 0


@dataclass(frozen=True)
class Inquiry:
    vendor: str
    product: str
    revision: str

    @property
    def is_buildwin_camera(self) -> bool:
        vendor = "".join(character for character in self.vendor.casefold() if character.isalnum())
        product = "".join(character for character in self.product.casefold() if character.isalnum())
        return vendor == "buildwin" and product in {"mediaplayer", "video050loader"}

    @property
    def is_loader(self) -> bool:
        vendor = "".join(character for character in self.vendor.casefold() if character.isalnum())
        product = "".join(character for character in self.product.casefold() if character.isalnum())
        return vendor == "buildwin" and product == "video050loader"


class SCSITransport(Protocol):
    def execute(
        self,
        cdb: bytes,
        *,
        data_out: bytes | None = None,
        data_in_length: int = 0,
    ) -> SCSITransfer: ...

    def close(self) -> None: ...


def _load_scsiio() -> tuple[type, type[Exception]]:
    """Import the existing sibling scsiio package without vendoring it."""
    try:
        from scsiio import SCSIDev  # type: ignore[import-not-found]
        from scsiio.common import SCSIException  # type: ignore[import-not-found]

        return SCSIDev, SCSIException
    except ImportError:
        pass

    override = os.environ.get("APXTOOL_JL_UBOOT_TOOL")
    if override:
        candidates = [Path(override).expanduser()]
    else:
        # Editable installs live in <workspace>/apxtool/src/apxtool; the
        # dependency checkout is the sibling <workspace>/jl-uboot-tool.
        workspace = Path(__file__).resolve().parents[3]
        candidates = [workspace / "jl-uboot-tool"]

    for candidate in candidates:
        if candidate.is_dir() and str(candidate) not in sys.path:
            sys.path.insert(0, str(candidate))
            try:
                from scsiio import SCSIDev  # type: ignore[import-not-found]
                from scsiio.common import SCSIException  # type: ignore[import-not-found]

                return SCSIDev, SCSIException
            except ImportError:
                sys.path.remove(str(candidate))

    raise ApxError(
        "could not import scsiio; keep jl-uboot-tool beside apxtool or set "
        "APXTOOL_JL_UBOOT_TOOL to its checkout path"
    )


class LinuxSCSITransport:
    """Thin adapter over jl-uboot-tool/scsiio's Linux SG_IO backend."""

    def __init__(self, path: str):
        if not sys.platform.startswith("linux"):
            raise ApxError("this apxtool build supports Linux SCSI pass-through only")
        SCSIDev, self._scsi_exception = _load_scsiio()
        self._device = SCSIDev(path)
        self.path = path

    def execute(
        self,
        cdb: bytes,
        *,
        data_out: bytes | None = None,
        data_in_length: int = 0,
    ) -> SCSITransfer:
        if not cdb or len(cdb) > 16:
            raise ValueError("SCSI CDB must contain between 1 and 16 bytes")
        if data_out is not None and data_in_length:
            raise ValueError("SCSI pass-through cannot transfer data in both directions")
        if data_in_length < 0:
            raise ValueError("data-in length cannot be negative")

        try:
            if data_in_length:
                response = bytearray(data_in_length)
                residual = self._device.execute(cdb, None, response)
                return SCSITransfer(bytes(response), int(residual or 0))
            residual = self._device.execute(cdb, data_out, None)
            return SCSITransfer(b"", int(residual or 0))
        except Exception as exc:
            # Keep the backend's sense text, host/driver status, and errno.
            if isinstance(exc, (self._scsi_exception, OSError)):
                raise TransferError(str(exc)) from exc
            raise

    def close(self) -> None:
        self._device.close()

    def __enter__(self) -> "LinuxSCSITransport":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


INQUIRY_CDB = b"\x12\x00\x00\x00\x24\x00"


def inquiry(transport: SCSITransport) -> Inquiry:
    transfer = transport.execute(INQUIRY_CDB, data_in_length=36)
    require_complete(transfer, 36, "SCSI INQUIRY")
    data = transfer.data
    return Inquiry(
        vendor=data[8:16].decode("ascii", errors="replace").strip(),
        product=data[16:32].decode("ascii", errors="replace").strip(),
        revision=data[32:36].decode("ascii", errors="replace").strip(),
    )


def require_camera(identity: Inquiry) -> None:
    if not identity.is_buildwin_camera:
        raise DeviceError(
            f"refusing camera command for INQUIRY target {identity.vendor!r} "
            f"{identity.product!r}; expected Buildwin Media-Player or Video050Loader"
        )


def require_complete(transfer: SCSITransfer, expected: int, label: str) -> bytes:
    if transfer.residual != 0:
        raise TransferError(
            f"{label} returned residual {transfer.residual} for {expected} requested bytes"
        )
    if len(transfer.data) != expected:
        raise TransferError(
            f"{label} returned {len(transfer.data)} bytes; expected {expected}"
        )
    return transfer.data


def iter_sg_paths() -> list[str]:
    return sorted(glob.glob("/dev/sg*"))


def wait_for_loader(
    *,
    timeout: float,
    paths: Callable[[], list[str]] = iter_sg_paths,
    opener: Callable[[str], SCSITransport] = LinuxSCSITransport,
    clock: Callable[[], float] = time.monotonic,
    sleeper: Callable[[float], None] = time.sleep,
) -> str:
    """Wait for the Buildwin Video050Loader SCSI identity after re-enumeration."""
    deadline = clock() + timeout
    while clock() < deadline:
        for path in paths():
            try:
                with opener(path) as dev:  # type: ignore[attr-defined]
                    identity = inquiry(dev)
                    if identity.is_loader:
                        return path
            except (OSError, ApxError):
                continue
        sleeper(min(0.25, max(0.0, deadline - clock())))
    raise DeviceError(
        f"timed out after {timeout:g}s waiting for Buildwin Video050Loader; "
        "check USB re-enumeration and SCSI generic device permissions"
    )
