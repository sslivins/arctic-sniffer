# arctic-sniffer native tests

Host-build unit tests for `tuya_codec`. No ESP-IDF, no FreeRTOS, no external
dependencies — just CMake + a C++17 compiler. Runs in CI on every push and
is the regression net protecting downstream consumers (`arctic-simulator`,
`arctic-controller`) from codec drift.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows with the Visual Studio generator the binary lands in
`build/Release/test_tuya_codec.exe`; on single-config generators it lands in
`build/test_tuya_codec`. Either way `ctest` finds it.

## What's covered

- Frame layout constants and the known-windows table
- `compute_checksum` against a known-good captured frame
- `frame_total_len` for both directions, including bad-dir rejection
- `parse_frame` against real captured request + both window responses,
  plus every `ParseResult` failure mode
  (`TRUNCATED` / `BAD_MAGIC` / `BAD_DIR` / `BAD_FC` / `UNKNOWN_WINDOW` /
  `BAD_CHECKSUM`)
- `encode_request` / `encode_response` round-trip through `parse_frame`,
  including input validation
- `find_frame_start` for clean, garbage-prefixed, bad-header-then-real,
  and no-frame inputs

## Fixtures

Golden frames are baked into the test source as hex strings, extracted from
`arctic_capture_raw.jsonl` lines 1 and 3. If the wire protocol changes,
re-capture and update the literals here — don't loosen the assertions.
