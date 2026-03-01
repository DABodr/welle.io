# Plan: Implement Channel Scan ("Plan") Mode for welle-cli

## Overview

Add a `--scan` / plan mode to welle-cli that:
1. Iterates all Band III + Band L DAB channels
2. Briefly tunes each channel and waits for FIC sync
3. Records discovered ensembles and their services
4. Outputs a structured JSON report of all found multiplexes

This is analogous to the "Sendersuchlauf" (channel scan) found in consumer DAB receivers.

---

## Existing Infrastructure to Reuse

| Component | Location | Usage |
|---|---|---|
| `Channels` class | `src/various/channels.h/cpp` | Provides full channel list and freq lookups |
| `RadioReceiver` | `src/backend/radio-receiver.h/cpp` | Already supports channel-by-channel tuning |
| `RadioControllerInterface` | `src/backend/radio-receiver.h` | Callback interface for sync/service events |
| `WebRadioInterface::retune()` | `src/welle-cli/webradiointerface.cpp:retune` | Channel switching pattern to replicate |
| `WelleCli` main | `src/welle-cli/welle-cli.cpp` | Entry point for new mode flag |

---

## Implementation Plan

### Step 1 — Add CLI argument

File: `src/welle-cli/welle-cli.cpp`

Add `--scan` (or `-S`) flag to the argument parser. Existing flags follow a clear pattern; the new flag will trigger `run_scan_mode()` instead of the usual radio/web-server loop.

Optional sub-flags:
- `--scan-timeout <seconds>` — dwell time per channel (default 10 s)
- `--scan-output <file>` — write JSON report to file (default: stdout)

### Step 2 — Implement `ChannelScanner` class

New files: `src/welle-cli/channelscanner.h` / `channelscanner.cpp`

```
class ChannelScanner : RadioControllerInterface {
    // For each channel:
    //   1. set input device frequency
    //   2. create RadioReceiver
    //   3. wait until isSynced() OR timeout
    //   4. if synced: collect ensemble + services from FIB processor
    //   5. destroy RadioReceiver, advance to next channel

    struct ScanResult {
        std::string channel;
        uint32_t    frequency_Hz;
        std::string ensemble_label;
        uint32_t    ensemble_id;
        float       snr;
        std::vector<ServiceInfo> services;
    };

    void run(int timeout_seconds);
    void printJsonReport(std::ostream& out) const;
};
```

Key callbacks to implement (from `RadioControllerInterface`):
- `onSNR(float)` — capture SNR
- `onServiceDetected(uint32_t sId)` — accumulate services
- `onNewEnsemble(uint32_t eid, const std::string& label)` — capture ensemble info

### Step 3 — Wire into `main()`

In `welle-cli.cpp::main()`:

```cpp
if (options.scan_mode) {
    ChannelScanner scanner(input_device);
    scanner.run(options.scan_timeout);
    scanner.printJsonReport(output_stream);
    return 0;
}
```

### Step 4 — JSON output format

```json
{
  "scan": {
    "timestamp": "2026-03-01T12:00:00Z",
    "timeout_per_channel_s": 10,
    "bands_scanned": ["III", "L"],
    "channels_scanned": 52,
    "ensembles_found": 3
  },
  "results": [
    {
      "channel": "5C",
      "frequency_hz": 178352000,
      "ensemble": {
        "id": "0x4C86",
        "label": "BBC National DAB"
      },
      "snr_db": 18.4,
      "services": [
        { "sid": "0xC221", "label": "BBC Radio 1", "bitrate_kbps": 128 },
        { "sid": "0xC222", "label": "BBC Radio 2", "bitrate_kbps": 128 }
      ]
    }
  ]
}
```

### Step 5 — Optional: Web UI scan button

In `index.html` / `index.js`: add a "Scan All Channels" button that:
- POSTs to `/scan` to trigger a scan
- Polls `/scan/status` for progress
- Displays results in a modal table

In `webradiointerface.cpp`: add `/scan` POST endpoint and `/scan/status` GET endpoint.

---

## Files to Create / Modify

| File | Action |
|---|---|
| `src/welle-cli/channelscanner.h` | **Create** — scanner class declaration |
| `src/welle-cli/channelscanner.cpp` | **Create** — scanner implementation |
| `src/welle-cli/welle-cli.cpp` | **Modify** — add `--scan` argument + `run_scan_mode()` |
| `src/welle-cli/CMakeLists.txt` | **Modify** — add new source files |
| `src/welle-cli/index.html` | **Modify (optional)** — scan button + results modal |
| `src/welle-cli/index.js` | **Modify (optional)** — scan trigger + polling logic |
| `src/welle-cli/webradiointerface.cpp` | **Modify (optional)** — `/scan` endpoints |

---

## Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Timeout too short — misses slow-sync channels | Default 10 s; user-configurable via `--scan-timeout` |
| Race condition destroying RadioReceiver while callbacks fire | Use the same mutex/join pattern as `WebRadioInterface::retune()` |
| Not all input devices support rapid retuning | Document limitation; emit warning and skip if retune fails |
