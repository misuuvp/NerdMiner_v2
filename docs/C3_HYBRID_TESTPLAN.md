# ESP32-C3 hybrid SHA miner – experiment 1

This branch is based on the working `webmanager` branch; it is an **experimental candidate, not a proven faster firmware**.

## Sources of ideas

- Existing NerdMiner V2: use current working midstate + register-minimised HW SHA-256 and keep webmanager, mining pool, Wi-Fi configuration.
- [NerdMinerV2-FX1](https://github.com/samkruzlic/NerdMinerV2-FX1): avoid dropped worker results and handle nonce wrap correctly. Implemented independently in the original project's existing worker.
- [NMMiner](https://github.com/NMminer1024/NMMiner): **study of publicly distributed C3 machine code only** suggested moving safe CPU-only job checks into SHA execution time. No NMMiner source/binary code was copied; no licensing/activation bypass.
- [SparkMiner](https://github.com/SneezeGUI/SparkMiner): inspiration for a later hardware/SW reference self-test; GPL code **not copied** into this MIT project.

## Changes in V1

1. **Queue backpressure:** instead of silently dropping a completed worker result if the 16-item queue is full, wait for room without holding the mutex. Restart the watchdog while waiting.
2. **Nonce wraparound:** walk an unsigned offset from zero to the exact requested count, rather than computing an overflow-prone end nonce. Use offset for completed-work count on stale job.
3. **Optional C3-only safe SHA overlap:** sample the existing job-change flag during the **second** SHA calculation. Both original SHA_BUSY checks remain. No SHA registers are touched before BUSY goes idle.

The optional overlap is **off by default**. There is no automatic speed increase without compiling and testing the optional variant.

## How to build

Build baseline using the already configured `ESP32-C3-super-mini` environment:

```sh
pio run -e ESP32-C3-super-mini
```

To build the A/B experimental variant, add this flag in **only** the `[env:ESP32-C3-super-mini]` `build_flags` list before compilation, then remove it for a baseline comparison:

```ini
-D C3_SHA_SAFE_OVERLAP=1
```

## Test protocol

- Record 60-second local hashrate over 15+ minutes; record the pool's measured rate and accepted/rejected shares for at least 1 hour (longer is better due to variance).
- Keep the same XEC pool, address, difficulty policy, 160 MHz clock, Wi-Fi, and firmware settings.
- First test **V1 without the macro**. Check accepted shares, reconnects and system uptime before trying overlap.
- Test **V1 with overlap** separately. Reject if the share rate worsens consistently, invalid shares occur, or it becomes unstable.
- Compare changes relative to the approximately 310.4 kH/s local baseline; the NMMiner claimed 396.9 kH/s is **not** a measurement on this device.
- Do not erase NVS/LittleFS unnecessarily. Keep your previous known-good firmware/build to restore.

## Not done yet

- No physical C3 hardware test, benchmark, or validation of faster speed.
- No new complete SHA engine or SparkMiner source graft.
- No automatic fallback self-test yet.
- No modifications to `webmanager`, no device flashing.
