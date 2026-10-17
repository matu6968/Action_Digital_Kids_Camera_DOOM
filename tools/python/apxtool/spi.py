from __future__ import annotations

from pathlib import Path

from .errors import ApxError, ProtocolUnavailable, TransferError, UnsafeOperation
from .protocol import (
    DEFAULT_FLASH_CHUNK,
    DEFAULT_FLASH_SIZE,
    DEFAULT_PAGE_SIZE,
    DEFAULT_SECTOR_SIZE,
    FlashGeometry,
    RAM_BASE,
)
from .ram import read_ram, write_ram_experimental
from .transport import SCSITransport


PAYLOAD_ADDRESS = 0x02180000
DATA_ADDRESS = 0x02182000
# The 64 KiB staging range ends here; keep callback blocks beyond it.
CONTROL_ADDRESS = 0x02192000
# Preflight and restore payload, staging data, and the full callback-slot arena.
SCRATCH_SIZE = 0x22000
CALLBACK_STACK_ADDRESS = 0x02200000
CALLBACK_STACK_SIZE = 0x1000
CONTROL_RESPONSE_SIZE = 20
CONTROL_BLOCK_SIZE = CONTROL_RESPONSE_SIZE
CONTROL_SLOT_STRIDE = 0x100
CONTROL_ARENA_END = PAYLOAD_ADDRESS + SCRATCH_SIZE
CONTROL_SLOT_COUNT = (CONTROL_ARENA_END - CONTROL_ADDRESS) // CONTROL_SLOT_STRIDE
CONTROL_MAGIC = b"APXS"
PROTOCOL_VERSION = 1
RESULT_DONE = 0xA5

CMD_ID = 1
CMD_READ = 2
CMD_ERASE = 3
CMD_PROGRAM = 4
CMD_HELLO = 0
CMD_STAGE_PROBE = 0x7C
CMD_DIRECT_ID_PROBE = 0x7D
CMD_REGISTER_SNAPSHOT = 0x7E
CMD_INIT_PROBE = 0x7F

SPI_REGISTER_SNAPSHOT = (
    0x0002,  # OpenRISC CPUCFGR: includes the no-delay-slot (ND) capability bit.
    0x0011,  # OpenRISC SR: read-only privilege-context diagnostic.
    0x8840,
    0x8844,
    0x8850,
    0x9000,
    0x9004,
    0x9008,
    0x900C,
    0x9244,
    0x9248,
    0x924C,
    0x9250,
    0x9254,
    0x9258,
    0x925C,
    0x9260,
    0x9264,
    0x9268,
    0x926C,
    0x9270,
    0x9274,
    0x9278,
    0x927C,
)
ROM_API_SIGNATURES = (
    (0x02032C08, bytes.fromhex("5b 00 60 18 fc 4f e1 d7"), "GPIO/SPI setup"),
    (0x02032CFC, bytes.fromhex("fc 4f e1 d7 f8 17 e1 d7"), "JEDEC ID"),
    (0x02032E8C, bytes.fromhex("fc 4f e1 d7 ec 17 e1 d7"), "SPI read"),
    (0x02032F54, bytes.fromhex("fc 4f e1 d7 00 ff a0 9c"), "SPI page program"),
    (0x02033070, bytes.fromhex("fc 4f e1 d7 00 f0 80 9c"), "SPI sector erase"),
)

_ERROR_NAMES = {
    1: "invalid command block",
    2: "flash range or alignment rejected by payload",
    7: "ROM flash routine reported failure",
}


class JT5295SPIFlash:
    """Flash operations through a volatile callback uploaded with SCSI 0xcb.

    The command block is held in camera RAM; no private SCSI opcode is used.
    The surrounding scratch area must be zeroed so the exact prior contents can
    be restored when this object exits.
    """

    def __init__(self, transport: SCSITransport, payload_path: Path):
        self.transport = transport
        self.payload_path = payload_path.expanduser()
        self.geometry = FlashGeometry(
            DEFAULT_FLASH_SIZE,
            DEFAULT_PAGE_SIZE,
            DEFAULT_SECTOR_SIZE,
            DEFAULT_FLASH_CHUNK,
            major=PROTOCOL_VERSION,
        )
        self._original: bytes | None = None
        self._original_stack: bytes | None = None
        self._started = False
        self._next_control_slot = 0
        self._clock = None

    @property
    def control_slots_remaining(self) -> int:
        return CONTROL_SLOT_COUNT - self._next_control_slot

    def ensure_command_capacity(self, command_count: int) -> None:
        if command_count < 0:
            raise ValueError("APXS command count cannot be negative")
        remaining = self.control_slots_remaining
        if command_count > remaining:
            raise ProtocolUnavailable(
                f"write needs {command_count} more APXS callbacks but this payload has "
                f"only {remaining} fresh control slots; split the update into smaller "
                "sector ranges"
            )

    def __enter__(self) -> "JT5295SPIFlash":
        self._start_payload()
        return self

    def __exit__(self, exc_type, exc, _traceback) -> bool:
        try:
            self.close(exc)
        except Exception as restore_error:
            if exc is not None:
                raise ApxError(
                    f"flash operation failed: {exc}; volatile scratch restore also failed: "
                    f"{restore_error}"
                ) from exc
            raise
        return False

    def _start_payload(self) -> None:
        image = self.payload_path.read_bytes()
        if not image or len(image) > DATA_ADDRESS - PAYLOAD_ADDRESS:
            raise ValueError("SPI payload must be nonempty and fit below staging RAM at 0x02182000")
        mismatches = check_rom_api_signatures(self.transport)
        if mismatches:
            raise ProtocolUnavailable(
                "firmware API signature mismatch: " + ", ".join(mismatches)
            )
        self._original = read_ram(self.transport, PAYLOAD_ADDRESS, SCRATCH_SIZE)
        self._original_stack = read_ram(
            self.transport, CALLBACK_STACK_ADDRESS, CALLBACK_STACK_SIZE
        )
        if any(self._original):
            raise UnsafeOperation(
                "candidate SPI payload scratch range 0x02180000..0x021a1fff is not all zero"
            )
        if any(self._original_stack):
            self._original = None
            self._original_stack = None
            raise UnsafeOperation(
                "candidate callback stack 0x02200000..0x02200fff is not all zero"
            )
        try:
            self._started = True
            from .clock import configured_clock
            self._clock = configured_clock(self.transport, self.payload_path)
            if self._clock is not None:
                self._clock.start()
            write_ram_experimental(self.transport, PAYLOAD_ADDRESS, image)
            if read_ram(self.transport, PAYLOAD_ADDRESS, len(image)) != image:
                raise ApxError("uploaded SPI payload did not match RAM readback")
            # The earlier marker callback needed this to make freshly uploaded
            # instructions visible on a related Buildwin camera. Keep it here
            # until JT5295 instruction-cache maintenance is characterized.
            read_ram(self.transport, RAM_BASE, 0x2000)
        except Exception as operation_error:
            try:
                self.close(operation_error)
            except Exception as restore_error:
                raise ApxError(
                    f"SPI payload upload failed: {operation_error}; scratch restore also "
                    f"failed: {restore_error}"
                ) from operation_error
            raise

    def close(self, operation_error=None) -> None:
        if not self._started or self._original is None:
            return
        # Restore clocks and their resident rollback code before removing SPI
        # code/data. Unknown clocks preserve both scratch areas for recovery.
        if self._clock is not None:
            self._clock.close(operation_error)
            if self._clock.report.get("clock_restoration_verified") is not True:
                raise UnsafeOperation("clock rollback is unverified; preserving SPI RAM; power cycle required")
        original = self._original
        original_stack = self._original_stack
        errors: list[str] = []
        try:
            write_ram_experimental(self.transport, PAYLOAD_ADDRESS, original)
            if read_ram(self.transport, PAYLOAD_ADDRESS, len(original)) != original:
                errors.append("payload scratch readback mismatch")
        except Exception as exc:
            errors.append(f"payload scratch restore: {exc}")
        if original_stack is not None:
            try:
                write_ram_experimental(self.transport, CALLBACK_STACK_ADDRESS, original_stack)
                if (
                    read_ram(self.transport, CALLBACK_STACK_ADDRESS, len(original_stack))
                    != original_stack
                ):
                    errors.append("callback stack readback mismatch")
            except Exception as exc:
                errors.append(f"callback stack restore: {exc}")
        if errors:
            raise ApxError("failed to restore volatile SPI state: " + "; ".join(errors))
        self._started = False
        self._original = None
        self._original_stack = None

    def hello(self) -> FlashGeometry:
        response = self._rpc(CMD_HELLO)
        if response[16:20] != b"APX1":
            raise ProtocolUnavailable(
                f"SPI payload handshake returned an invalid capability marker "
                f"{response[16:20]!r}"
            )
        return self.geometry

    def probe_staging_buffer(self) -> bytes:
        """Check target CPU writes and loader reads at the SPI staging address."""
        response = self._rpc(CMD_STAGE_PROBE)
        block = read_ram(self.transport, DATA_ADDRESS, 4)
        target_sample = response[16:20]
        if target_sample != block:
            raise ApxError(
                "RAM staging probe mismatch: callback saw "
                f"{target_sample.hex()} but 0xcb returned {block.hex()}"
            )
        return block

    def _rpc(self, command: int, *, address: int = 0, length: int = 0) -> bytes:
        if not self._started:
            raise ProtocolUnavailable("SPI payload is not running; use it inside its context")
        if not 0 <= address <= 0xFFFFFFFF or not 0 <= length <= 0xFFFFFFFF:
            raise ValueError("SPI address and length must fit in 32 bits")
        control_address = CONTROL_ADDRESS + self._next_control_slot * CONTROL_SLOT_STRIDE
        if self._next_control_slot >= CONTROL_SLOT_COUNT:
            raise ProtocolUnavailable(
                "the volatile APXS control-slot arena is exhausted; restart the loader "
                "before issuing more callback commands"
            )
        self._next_control_slot += 1

        # Each command uses a fresh cache line. Clear the response area as
        # well, so a callback that fails to run cannot reuse a prior result.
        request = bytearray(CONTROL_BLOCK_SIZE)
        request[0:4] = CONTROL_MAGIC
        request[4] = PROTOCOL_VERSION
        request[5] = command
        request[8:12] = address.to_bytes(4, "little")
        request[12:16] = length.to_bytes(4, "little")
        write_ram_experimental(self.transport, control_address, bytes(request))
        cleared = read_ram(self.transport, control_address, CONTROL_RESPONSE_SIZE)
        if cleared[6:8] != b"\0\0" or cleared[16:20] != b"\0" * 4:
            raise ProtocolUnavailable(
                "could not clear the prior APXS callback response before dispatch"
            )

        # The BootROM passes the CDB memory address in r3 and invokes the
        # callback before returning data-in bytes from that same address.
        # A fresh request slot keeps its bytes visible to target code and lets
        # this transfer return the completed response directly.
        response = read_ram(
            self.transport,
            control_address,
            CONTROL_RESPONSE_SIZE,
            callback_address=PAYLOAD_ADDRESS,
        )
        if (
            response[:4] != CONTROL_MAGIC
            or response[4] != PROTOCOL_VERSION
            or response[5] != command
        ):
            raise ProtocolUnavailable("SPI callback returned an invalid APXS command block")
        if response[6] != RESULT_DONE:
            if response[6] == 0 and response[7] == 0 and response[16:20] == b"\0" * 4:
                raise ProtocolUnavailable(
                    "SPI callback left the cleared APXS response untouched; callback did not run"
                )
            detail = response[7]
            reason = _ERROR_NAMES.get(detail, f"payload error {detail}")
            raise ApxError(
                f"JT5295 SPI payload rejected command: {reason} "
                f"(status=0x{response[6]:02x}, detail=0x{detail:02x}, "
                f"result={response[16:20].hex()})"
            )
        return response

    def flash_id(self) -> bytes:
        response = self._rpc(CMD_ID)
        return response[16:19]

    def probe_spi_init(self) -> bytes:
        """Call the candidate SPI/GPIO setup routine without issuing a flash command."""
        return self._rpc(CMD_INIT_PROBE)[16:19]

    def probe_id_without_init(self) -> bytes:
        """Call the read-only JEDEC routine without the candidate GPIO/SPI initializer."""
        return self._rpc(CMD_DIRECT_ID_PROBE)[16:19]

    def read_controller_register(self, address: int) -> int:
        """Read one whitelisted setup-related SPR without touching the flash."""
        if address not in SPI_REGISTER_SNAPSHOT:
            raise ValueError(f"SPR 0x{address:04x} is not in the diagnostic whitelist")
        response = self._rpc(CMD_REGISTER_SNAPSHOT, address=address)
        return int.from_bytes(response[16:20], "little")

    def read(self, address: int, length: int) -> bytes:
        self.geometry.check_range(address, length)
        if length <= 0:
            raise ValueError("flash read length must be positive")
        output = bytearray()
        while len(output) < length:
            count = min(self.geometry.max_transfer, length - len(output))
            response = self._rpc(CMD_READ, address=address + len(output), length=count)
            block = read_ram(self.transport, DATA_ADDRESS, count)
            if len(block) != count:
                raise TransferError(f"flash RAM staging read returned {len(block)}; expected {count}")
            sample_size = min(count, 4)
            target_sample = response[16 : 16 + sample_size]
            if target_sample != block[:sample_size]:
                raise ApxError(
                    "SPI read target/RAM mismatch: callback saw "
                    f"{target_sample.hex()} but 0xcb returned {block[:sample_size].hex()}"
                )
            output.extend(block)
        return bytes(output)

    def erase_sector(self, address: int) -> None:
        self.geometry.check_range(address, self.geometry.sector_size)
        if address % self.geometry.sector_size:
            raise ValueError("sector erase address must be sector aligned")
        self._rpc(CMD_ERASE, address=address, length=self.geometry.sector_size)

    def program_page(self, address: int, data: bytes) -> None:
        self.geometry.check_range(address, len(data))
        if len(data) != self.geometry.page_size:
            raise ValueError(f"the JT5295 ROM page routine requires exactly {self.geometry.page_size} bytes")
        if address % self.geometry.page_size:
            raise ValueError("page program address must be page aligned")
        write_ram_experimental(self.transport, DATA_ADDRESS, data)
        self._rpc(CMD_PROGRAM, address=address, length=len(data))


def check_rom_api_signatures(transport: SCSITransport) -> list[str]:
    """Validate static firmware API bytes over 0xcb before uploading code."""
    mismatches: list[str] = []
    for address, expected, name in ROM_API_SIGNATURES:
        observed = read_ram(transport, address, len(expected))
        if observed != expected:
            mismatches.append(
                f"{name} at 0x{address:08x}: got {observed.hex()} expected {expected.hex()}"
            )
    return mismatches
