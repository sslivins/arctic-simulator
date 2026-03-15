#!/usr/bin/env python3
"""
Playback Integration Test — Arctic Simulator

Replays a JSONL capture file against the simulator's REST API and verifies
that every register written is actually readable with the expected value.

Tests cover:
  1. Holding registers (2000–2057) — set via bulk, readback verified
  2. Input registers (2100–2138) — same, including STATUS_2 bit flags
  3. Simulation disable/enable — confirms STATUS_2 survives bulk write
     when simulation is off, and gets recomputed when re-enabled
  4. Single-value entries (fc:6) — mapped to single register write
  5. Multi-value entries (fc:3) — mapped to contiguous range write
  6. Full capture replay — every snapshot from the file verified

Usage:
    python tests/test_playback.py                          # uses default host
    python tests/test_playback.py --host 192.168.4.1       # custom IP
    python tests/test_playback.py --capture captures/sniff_cap3.jsonl
    python tests/test_playback.py -v                       # verbose per-register
"""

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Optional

try:
    import requests
except ImportError:
    print("ERROR: 'requests' package required.  Install with:  pip install requests")
    sys.exit(1)


# ── Defaults ─────────────────────────────────────────────────────────────────

DEFAULT_HOST = "arctic-sim.local"
DEFAULT_PORT = 80
DEFAULT_CAPTURE = "captures/example.jsonl"

# Register ranges (must match register_map.h)
HOLDING_BASE = 2000
HOLDING_END = 2057
INPUT_BASE = 2100
INPUT_END = 2138


# ── Helpers ──────────────────────────────────────────────────────────────────

class SimClient:
    """Thin REST client for the simulator API."""

    def __init__(self, base_url: str, timeout: float = 5.0):
        self.base = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()

    def get_status(self) -> dict:
        r = self.session.get(f"{self.base}/api/status", timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def get_registers(self) -> dict:
        """Return { "holding": { "2000": v, ... }, "input": { "2100": v, ... } }"""
        r = self.session.get(f"{self.base}/api/registers", timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def get_register(self, addr: int) -> int:
        r = self.session.get(
            f"{self.base}/api/registers", params={"addr": addr}, timeout=self.timeout
        )
        r.raise_for_status()
        return int(r.json()["value"])

    def set_register(self, addr: int, value: int):
        r = self.session.put(
            f"{self.base}/api/registers",
            params={"addr": addr},
            json={"value": value},
            timeout=self.timeout,
        )
        r.raise_for_status()
        return r.json()

    def bulk_set(self, regs: dict[str, int]) -> dict:
        r = self.session.post(
            f"{self.base}/api/registers/bulk",
            json={"registers": regs},
            timeout=self.timeout,
        )
        r.raise_for_status()
        return r.json()

    def set_simulation(self, enabled: bool) -> dict:
        r = self.session.post(
            f"{self.base}/api/simulation",
            json={"enabled": enabled},
            timeout=self.timeout,
        )
        r.raise_for_status()
        return r.json()

    def load_preset(self, name: str) -> dict:
        r = self.session.post(
            f"{self.base}/api/preset",
            json={"name": name},
            timeout=self.timeout,
        )
        r.raise_for_status()
        return r.json()


def parse_capture(path: str) -> list[dict]:
    """Parse a JSONL capture file into a list of entry dicts."""
    entries = []
    with open(path, "r") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                entries.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"  WARN: skipping line {line_no}: {e}")
    return entries


def entry_to_regs(entry: dict) -> dict[str, int]:
    """Convert a capture entry to a { "addr": value } dict for bulk write."""
    regs = {}
    addr = entry.get("addr", 0)
    if "values" in entry:
        for i, v in enumerate(entry["values"]):
            regs[str(addr + i)] = v
    elif "value" in entry:
        regs[str(addr)] = entry["value"]
    return regs


def status2_bits(value: int) -> list[str]:
    """Decode STATUS_2 bit flags into human-readable names."""
    names = []
    bit_labels = [
        (0, "UNIT_ON"),
        (1, "COMPRESSOR"),
        (2, "FAN_HIGH"),
        (3, "FAN_MED"),
        (4, "FAN_LOW"),
        (5, "WATER_PUMP"),
        (6, "4WAY_VALVE"),
        (7, "ELEC_HEATER"),
        (8, "WATER_FLOW"),
        (9, "HP_SWITCH"),
        (10, "LP_SWITCH"),
        (11, "REMOTE_ON"),
        (12, "MODE_SWITCH"),
        (13, "3WAY_V1"),
        (14, "3WAY_V2"),
        (15, "BRINE_FLOW"),
    ]
    for bit, label in bit_labels:
        if value & (1 << bit):
            names.append(label)
    return names


# ── Test Cases ───────────────────────────────────────────────────────────────

class TestRunner:
    def __init__(self, client: SimClient, verbose: bool = False):
        self.client = client
        self.verbose = verbose
        self.passed = 0
        self.failed = 0
        self.errors: list[str] = []

    def assert_eq(self, description: str, expected, actual):
        if expected == actual:
            self.passed += 1
            if self.verbose:
                print(f"    PASS: {description} (={expected})")
        else:
            self.failed += 1
            msg = f"    FAIL: {description} — expected {expected}, got {actual}"
            print(msg)
            self.errors.append(msg)

    def assert_true(self, description: str, condition: bool):
        if condition:
            self.passed += 1
            if self.verbose:
                print(f"    PASS: {description}")
        else:
            self.failed += 1
            msg = f"    FAIL: {description}"
            print(msg)
            self.errors.append(msg)

    # ── Individual Tests ─────────────────────────────────────────────────

    def test_connectivity(self):
        """Verify the simulator is reachable and responds to /api/status."""
        print("\n[1] Connectivity")
        status = self.client.get_status()
        self.assert_eq("firmware field", "arctic-simulator", status.get("firmware"))
        self.assert_true("version present", bool(status.get("version")))
        print(f"    Firmware: {status.get('firmware')} v{status.get('version')}")

    def test_bulk_holding_registers(self):
        """Write a batch of holding registers and read each one back."""
        print("\n[2] Bulk write → holding registers (2000–2007)")
        test_values = {
            "2000": 1,     # UNIT_ON_OFF = ON
            "2001": 2,     # WORKING_MODE = Fan Coil Heat
            "2002": 12,    # COOLING_SETPOINT
            "2003": 40,    # HEATING_SETPOINT
            "2004": 50,    # HOT_WATER_SETPOINT
            "2005": 3,     # COOLING_DELTA_T
            "2006": 4,     # HEATING_DELTA_T
            "2007": 6,     # HOT_WATER_DELTA_T
        }
        resp = self.client.bulk_set(test_values)
        self.assert_eq("updated count", len(test_values), resp.get("updated"))

        # Read back each one
        for addr_s, expected in test_values.items():
            actual = self.client.get_register(int(addr_s))
            self.assert_eq(f"register {addr_s}", expected, actual)

    def test_bulk_input_registers(self):
        """Write input registers and verify they stick (simulation disabled)."""
        print("\n[3] Bulk write → input registers (simulation disabled)")

        # Disable simulation so it won't overwrite STATUS_2
        resp = self.client.set_simulation(False)
        self.assert_eq("simulation disabled", False, resp.get("simulation"))

        test_values = {
            "2100": 350,   # WATER_TANK_TEMP
            "2102": 280,   # OUTLET_WATER_TEMP
            "2103": 260,   # INLET_WATER_TEMP
            "2110": 150,   # OUTDOOR_AMBIENT_TEMP
            "2118": 55,    # COMPRESSOR_FREQ
            "2119": 720,   # FAN_SPEED
            "2120": 230,   # AC_VOLTAGE
            "2135": 0x0933,  # STATUS_2 (specific bit pattern)
        }
        resp = self.client.bulk_set(test_values)
        self.assert_eq("updated count", len(test_values), resp.get("updated"))

        # Small delay for register update
        time.sleep(0.1)

        # Read back each one
        for addr_s, expected in test_values.items():
            actual = self.client.get_register(int(addr_s))
            self.assert_eq(f"register {addr_s}", expected, actual)

        # Decode STATUS_2 for readability
        sts2 = self.client.get_register(2135)
        print(f"    STATUS_2 = 0x{sts2:04X} → {status2_bits(sts2)}")

        # Re-enable simulation
        resp = self.client.set_simulation(True)
        self.assert_eq("simulation re-enabled", True, resp.get("simulation"))

    def test_simulation_overwrites_status2(self):
        """Confirm that re-enabling simulation recomputes STATUS_2."""
        print("\n[4] Simulation re-enable recomputes STATUS_2")

        # Set a known holding state: unit ON, floor heating
        self.client.bulk_set({"2000": 1, "2001": 1})
        time.sleep(0.1)

        # With simulation enabled, STATUS_2 should reflect unit on + floor heating
        sts2 = self.client.get_register(2135)
        self.assert_true(
            f"UNIT_ON bit set (0x{sts2:04X})", bool(sts2 & 0x0001)
        )
        self.assert_true(
            f"COMPRESSOR bit set (0x{sts2:04X})", bool(sts2 & 0x0002)
        )
        self.assert_true(
            f"WATER_PUMP bit set (0x{sts2:04X})", bool(sts2 & 0x0020)
        )
        print(f"    STATUS_2 = 0x{sts2:04X} → {status2_bits(sts2)}")

    def test_simulation_disabled_preserves_status2(self):
        """Write STATUS_2 with simulation off, verify it's not overwritten."""
        print("\n[5] Simulation disabled → STATUS_2 preserved through bulk writes")

        self.client.set_simulation(False)

        # Write a specific STATUS_2 value
        custom_sts2 = 0x092B  # arbitrary bit pattern
        self.client.bulk_set({"2135": custom_sts2})
        time.sleep(0.1)

        actual = self.client.get_register(2135)
        self.assert_eq(
            f"STATUS_2 preserved (0x{custom_sts2:04X})", custom_sts2, actual
        )

        # Now also write holding registers — STATUS_2 should NOT change
        self.client.bulk_set({"2000": 0, "2001": 0})  # unit off
        time.sleep(0.1)

        actual = self.client.get_register(2135)
        self.assert_eq(
            f"STATUS_2 still preserved after holding write (0x{custom_sts2:04X})",
            custom_sts2,
            actual,
        )

        self.client.set_simulation(True)

    def test_single_register_write(self):
        """fc:6 style entry — single register write and readback."""
        print("\n[6] Single register write (fc:6 style)")

        self.client.set_register(2000, 1)
        actual = self.client.get_register(2000)
        self.assert_eq("UNIT_ON_OFF = 1", 1, actual)

        self.client.set_register(2003, 42)
        actual = self.client.get_register(2003)
        self.assert_eq("HEATING_SETPOINT = 42", 42, actual)

    def test_capture_replay(self, capture_path: str):
        """Replay every entry from a capture file, verify registers after each."""
        entries = parse_capture(capture_path)
        if not entries:
            print(f"\n[7] Capture replay — SKIP (no entries in {capture_path})")
            return

        print(f"\n[7] Full capture replay — {len(entries)} entries from {capture_path}")

        # Reset to idle and disable simulation
        self.client.load_preset("idle")
        self.client.set_simulation(False)
        time.sleep(0.1)

        failures_before = self.failed
        entries_verified = 0
        registers_verified = 0

        for i, entry in enumerate(entries):
            regs = entry_to_regs(entry)
            if not regs:
                continue

            # Write the registers
            resp = self.client.bulk_set(regs)
            expected_count = len(regs)
            actual_count = resp.get("updated", 0)
            if actual_count != expected_count:
                self.failed += 1
                msg = f"    FAIL: entry {i} bulk updated {actual_count}/{expected_count}"
                print(msg)
                self.errors.append(msg)
                continue

            # Verify every register in this entry
            all_regs = self.client.get_registers()
            entry_ok = True
            for addr_s, expected in regs.items():
                addr = int(addr_s)
                # Look up in appropriate section
                if HOLDING_BASE <= addr <= HOLDING_END:
                    actual = int(all_regs["holding"].get(addr_s, -1))
                elif INPUT_BASE <= addr <= INPUT_END:
                    actual = int(all_regs["input"].get(addr_s, -1))
                else:
                    continue

                if actual != expected:
                    self.failed += 1
                    extra = ""
                    if addr == 2135:
                        extra = (
                            f" bits: expected={status2_bits(expected)}"
                            f" actual={status2_bits(actual)}"
                        )
                    msg = (
                        f"    FAIL: entry {i} reg {addr_s}: "
                        f"expected {expected}, got {actual}{extra}"
                    )
                    print(msg)
                    self.errors.append(msg)
                    entry_ok = False
                else:
                    self.passed += 1
                    registers_verified += 1

            if entry_ok:
                entries_verified += 1

        replay_failures = self.failed - failures_before
        print(
            f"    Entries: {entries_verified}/{len(entries)} OK  |  "
            f"Registers: {registers_verified} verified  |  "
            f"Failures: {replay_failures}"
        )

        # Re-enable simulation
        self.client.set_simulation(True)

    def test_status2_bit_patterns(self):
        """Write several known STATUS_2 patterns and verify each bit."""
        print("\n[8] STATUS_2 bit-pattern verification")

        self.client.set_simulation(False)

        patterns = [
            (0x0000, "all off"),
            (0x0001, "UNIT_ON only"),
            (0x0923, "unit+compressor+water_pump+water_flow+remote_on"),
            (0xFFFF, "all bits set"),
            (0x092B, "sniff_cap3 pattern A"),
            (0x0927, "sniff_cap3 pattern B"),
            (0x0905, "sniff_cap3 pattern C"),
        ]

        for value, desc in patterns:
            self.client.bulk_set({"2135": value})
            time.sleep(0.05)
            actual = self.client.get_register(2135)
            self.assert_eq(
                f"STATUS_2 0x{value:04X} ({desc})", value, actual
            )
            if self.verbose:
                print(f"      bits: {status2_bits(actual)}")

        self.client.set_simulation(True)

    def test_mixed_holding_and_input_bulk(self):
        """Single bulk write containing both holding and input registers."""
        print("\n[9] Mixed bulk write (holding + input in one call)")

        self.client.set_simulation(False)

        mixed = {
            "2000": 1,      # holding: UNIT_ON_OFF
            "2001": 6,      # holding: WORKING_MODE = Auto
            "2003": 45,     # holding: HEATING_SETPOINT
            "2100": 400,    # input: WATER_TANK_TEMP
            "2110": 100,    # input: OUTDOOR_AMBIENT_TEMP
            "2118": 60,     # input: COMPRESSOR_FREQ
            "2135": 0x0933, # input: STATUS_2
        }
        resp = self.client.bulk_set(mixed)
        self.assert_eq("updated count", len(mixed), resp.get("updated"))

        time.sleep(0.1)

        for addr_s, expected in mixed.items():
            actual = self.client.get_register(int(addr_s))
            self.assert_eq(f"register {addr_s}", expected, actual)

        self.client.set_simulation(True)

    # ── Runner ───────────────────────────────────────────────────────────

    def run_all(self, capture_path: str):
        print("=" * 60)
        print("Arctic Simulator — Playback Integration Tests")
        print("=" * 60)
        print(f"Target: {self.client.base}")

        try:
            self.test_connectivity()
        except Exception as e:
            print(f"\n  FATAL: Cannot reach simulator at {self.client.base}")
            print(f"         {e}")
            print("\n  Make sure the device is powered on and connected.")
            return False

        # Reset to a known state
        try:
            self.client.load_preset("idle")
            self.client.set_simulation(True)
        except Exception:
            pass

        self.test_bulk_holding_registers()
        self.test_bulk_input_registers()
        self.test_simulation_overwrites_status2()
        self.test_simulation_disabled_preserves_status2()
        self.test_single_register_write()
        self.test_status2_bit_patterns()
        self.test_mixed_holding_and_input_bulk()
        self.test_capture_replay(capture_path)

        # Restore clean state
        try:
            self.client.load_preset("idle")
            self.client.set_simulation(True)
        except Exception:
            pass

        # Summary
        total = self.passed + self.failed
        print("\n" + "=" * 60)
        if self.failed == 0:
            print(f"ALL {total} CHECKS PASSED")
        else:
            print(f"RESULT: {self.passed}/{total} passed, {self.failed} FAILED")
            print("\nFailures:")
            for err in self.errors:
                print(err)
        print("=" * 60)
        return self.failed == 0


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Playback integration tests for the Arctic Simulator"
    )
    parser.add_argument(
        "--host",
        default=DEFAULT_HOST,
        help=f"Simulator hostname or IP (default: {DEFAULT_HOST})",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"HTTP port (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--capture",
        default=DEFAULT_CAPTURE,
        help=f"JSONL capture file to replay (default: {DEFAULT_CAPTURE})",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print every passing assertion",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="HTTP request timeout in seconds (default: 5.0)",
    )
    args = parser.parse_args()

    base_url = f"http://{args.host}:{args.port}"
    client = SimClient(base_url, timeout=args.timeout)
    runner = TestRunner(client, verbose=args.verbose)

    success = runner.run_all(args.capture)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
