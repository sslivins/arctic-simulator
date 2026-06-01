# tuya_codec (vendored)

Source of truth: `sslivins/arctic-sniffer` at tag **v0.4.0** (sha `624cdd249e64f3805133cce410f34db13f115934`).

Files in this folder are an unmodified copy of:

- `arctic-sniffer/main/tuya_codec.h`
- `arctic-sniffer/main/tuya_codec.cpp`

## When you change something here

**Don't.** Make the change in `arctic-sniffer`, land it, bump the
sniffer tag, then re-vendor here by re-copying both files and updating
the version stamp above. The two repos must stay byte-identical so the
codec behaves the same in the passive sniffer and the active simulator.

## Why a copy and not a submodule

Two files, no external dependencies, both repos are ESP-IDF projects.
A submodule of an entire firmware repo just to pull two source files
would be heavier than the diff it's preventing. The bench-test harness
in p2e will catch any drift before it ships.
