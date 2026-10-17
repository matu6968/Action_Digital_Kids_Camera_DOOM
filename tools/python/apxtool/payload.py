from __future__ import annotations

from pathlib import Path

from .errors import ApxError, UnsafeOperation
from .protocol import RAM_BASE, RAM_LIMIT
from .ram import read_ram, write_ram_experimental
from .spi import (
    CALLBACK_STACK_ADDRESS,
    CALLBACK_STACK_SIZE,
    CONTROL_ADDRESS,
    SCRATCH_SIZE,
)
from .transport import SCSITransport


MARKER_LOAD_ADDRESS = 0x02180000
MARKER_RESULT_ADDRESS = 0x02181000
MARKER_RESULT = b"APXT"
MARKER_RESULT_SIZE = 8
PAYLOAD_SCRATCH_SIZE = SCRATCH_SIZE
RAM_CACHE_EVICTION_READ = 0x2000


def run_marker_payload(
    transport: SCSITransport,
    image_path: Path,
    *,
    experimental: bool,
    confirmed: bool,
) -> bytes:
    if not experimental or not confirmed:
        raise UnsafeOperation("payload execution requires both --experimental and --yes")
    image = image_path.expanduser().read_bytes()
    if not image:
        raise ValueError("payload image is empty")
    if MARKER_LOAD_ADDRESS + len(image) > CONTROL_ADDRESS:
        raise ValueError("marker payload must fit below its control block at 0x02181000")
    if MARKER_RESULT_ADDRESS + MARKER_RESULT_SIZE > RAM_LIMIT:
        raise ValueError("marker result lies outside the captured RAM window")

    original_scratch = read_ram(transport, MARKER_LOAD_ADDRESS, PAYLOAD_SCRATCH_SIZE)
    original_stack = read_ram(transport, CALLBACK_STACK_ADDRESS, CALLBACK_STACK_SIZE)
    if any(original_scratch) or any(original_stack):
        raise UnsafeOperation(
            "the candidate payload or callback-stack scratch range is not all zero; "
            "refusing to overwrite it"
        )

    upload_started = False
    payload_clock = None
    operation_error: Exception | None = None
    try:
        upload_started = True
        from .clock import configured_clock
        payload_clock = configured_clock(transport, image_path)
        if payload_clock is not None:
            payload_clock.start()
        write_ram_experimental(transport, MARKER_LOAD_ADDRESS, image)
        echoed = read_ram(transport, MARKER_LOAD_ADDRESS, len(image))
        if echoed != image:
            raise ApxError("uploaded payload did not match its RAM readback")

        # A related Buildwin camera needed an 8 KiB read to evict recently
        # written instructions from its data cache. This is a cautious,
        # read-only compatibility step; it remains unverified on this JT5295.
        read_ram(transport, RAM_BASE, RAM_CACHE_EVICTION_READ)
        # 0xcb's first address is its required ROM transfer handler. The
        # optional CDB callback field is the RAM function pointer. Supplying
        # the payload as the transfer callback is different from calling its
        # address as the main handler (which reset the camera).
        read_ram(
            transport,
            RAM_BASE,
            16,
            callback_address=MARKER_LOAD_ADDRESS,
        )
        marker = read_ram(transport, MARKER_RESULT_ADDRESS, MARKER_RESULT_SIZE)
        if marker[: len(MARKER_RESULT)] != MARKER_RESULT:
            raise ApxError(
                f"payload returned without the APXT marker (read {marker[:4]!r}); "
                "execution semantics remain unverified"
            )
        return marker
    except Exception as exc:
        operation_error = exc
        raise
    finally:
        if upload_started:
            try:
                if payload_clock is not None:
                    payload_clock.close(operation_error)
                    if payload_clock.report.get("clock_restoration_verified") is not True:
                        raise UnsafeOperation("clock rollback is unverified; preserving payload RAM; power cycle required")
                # The original capture showed this candidate area as zero.
                # Restore those exact bytes even when the marker check fails.
                restore_errors: list[str] = []
                try:
                    write_ram_experimental(transport, MARKER_LOAD_ADDRESS, original_scratch)
                    if (
                        read_ram(transport, MARKER_LOAD_ADDRESS, len(original_scratch))
                        != original_scratch
                    ):
                        restore_errors.append("payload scratch readback mismatch")
                except Exception as restore_error:
                    restore_errors.append(f"payload scratch restore: {restore_error}")
                try:
                    write_ram_experimental(
                        transport, CALLBACK_STACK_ADDRESS, original_stack
                    )
                    if (
                        read_ram(transport, CALLBACK_STACK_ADDRESS, len(original_stack))
                        != original_stack
                    ):
                        restore_errors.append("callback stack readback mismatch")
                except Exception as restore_error:
                    restore_errors.append(f"callback stack restore: {restore_error}")
                if restore_errors:
                    raise ApxError("failed to restore volatile scratch: " + "; ".join(restore_errors))
            except Exception as restore_error:
                if operation_error is None:
                    raise
                raise ApxError(
                    f"payload operation failed: {operation_error}; volatile scratch restore "
                    f"also failed: {restore_error}"
                ) from operation_error
