# doom-probe

The hardware probe the port was planned around (single file, `main.c`). It is an *example of the SDK* (`appotech-sdk/examples/doom-probe/`
in the companion SDK repository), not part of this build; it is kept here as the record of how these facts were measured:

* `l.mul`, `l.muli`, `l.div`, `l.divu` against the software routines, with timing (test A). The SDK now also has `examples/muldiv-test` (copy in `muldiv-test/`): 700 known-answer vectors plus timing, for `HW_MULDIV=0` and `1`
* is `0x02000000..0x0217ffff` usable RAM after takeover (test B)
* raw ADC values while key combinations are pressed (test C, the loop at the end of `main`)
* SD read throughput (test D)
* DAC DMA rate, and the next-block queue (tests E2, E3)
* palette-to-NV21 conversion time and display frame time (test F)

Build it in the SDK with `make EXAMPLE=doom-probe HEAP_SIZE=12288` and launch it with `tools/launch.sh path/to/doom-probe.manifest.json`.
The version here runs the last experiment configuration (audio queue, SD); the earlier tests are still in the file, marked unused.
