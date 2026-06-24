"""
Set LCU lighting state via CAN — Candlelight USB adapter.

Sends SWU_LCU_INPUTS frames (0x302, extended) to control individual lights
on the Lighting Control Unit, matching the steering-wheel lighting inputs.

Lights (SWU_LCU_INPUTS byte 0):
  bit 0: HAZARD
  bit 1: BEAM
  bit 2: POSITION
  bit 3: RIGHT_INDICATOR
  bit 4: LEFT_INDICATOR
  bit 5: BRAKE_LIGHT

Usage:
    python lights_can.py --beam on --brake on
    python lights_can.py --hazard on
    python lights_can.py --left-indicator on --position on
    python lights_can.py --all-off
    python lights_can.py --all-on
    python lights_can.py --raw 0x3F             # all SWU_LCU_INPUTS bits on

Requirements:
    pip install python-can[gs_usb] libusb
    PeakCAN/PCAN: install PEAK PCAN-Basic drivers; use --interface pcan
"""

import argparse
import sys
import time

try:
    import can
except ImportError:
    print("Error: python-can not installed. Run: pip install python-can[gs_usb] libusb")
    print("PeakCAN/PCAN also requires PEAK PCAN-Basic drivers.")
    sys.exit(1)

from can_backend import add_can_backend_args, backend_label, open_bus


def _patch_libusb() -> None:
    try:
        import libusb as _libusb_pkg
        import usb.backend.libusb1 as _lb1
        dll_path = _libusb_pkg.dll._name
        _orig = _lb1.get_backend
        _lb1.get_backend = lambda find_library=None: _orig(
            find_library=lambda _: dll_path
        )
    except Exception:
        pass


_patch_libusb()

# ── CAN IDs (extended frames) ────────────────────────────────────────────────
SWU_LCU_INPUTS_ID = 0x302
LCU_STATUS_ID     = 0x401

# ── Bit positions in SWU_LCU_INPUTS byte 0 ───────────────────────────────────
BIT_HAZARD          = 0
BIT_BEAM            = 1
BIT_POSITION        = 2
BIT_RIGHT_INDICATOR = 3
BIT_LEFT_INDICATOR  = 4
BIT_BRAKE_LIGHT     = 5

_LIGHT_NAMES = {
    BIT_HAZARD:          "HAZARD",
    BIT_BEAM:            "BEAM",
    BIT_POSITION:        "POSITION",
    BIT_RIGHT_INDICATOR: "RIGHT_INDICATOR",
    BIT_LEFT_INDICATOR:  "LEFT_INDICATOR",
    BIT_BRAKE_LIGHT:     "BRAKE_LIGHT",
}


def _decode_lcu_status(data: bytes) -> str:
    if len(data) < 1:
        return "empty"
    b = data[0]
    instance = "FRONT" if (b & 0x01) == 0 else "REAR"
    fault    = "FAULT" if (b & 0x02) else "OK"
    lights = []
    if b & 0x04: lights.append("HEADLIGHT")
    if b & 0x08: lights.append("POSITION")
    if b & 0x10: lights.append("BRAKE")
    if b & 0x20: lights.append("LEFT_IND")
    if b & 0x40: lights.append("RIGHT_IND")
    if b & 0x80: lights.append("HAZARD")
    return f"{instance} {fault} [{', '.join(lights) or 'none'}]"


def _print_mask(mask: int) -> None:
    print(f"  SWU_LCU_INPUTS byte: 0x{mask:02X} (0b{mask:08b})")
    for bit, name in sorted(_LIGHT_NAMES.items()):
        state = "ON" if mask & (1 << bit) else "off"
        print(f"    {name:<20} {state}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Set LCU lighting state via CAN",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    parser.add_argument("--beam", "--headlight", dest="beam", choices=["on", "off"])
    parser.add_argument("--position",        choices=["on", "off"])
    parser.add_argument("--brake",           choices=["on", "off"])
    parser.add_argument("--left-indicator",  choices=["on", "off"])
    parser.add_argument("--right-indicator", choices=["on", "off"])
    parser.add_argument("--hazard",          choices=["on", "off"])
    parser.add_argument("--all-on",  action="store_true", help="Turn all SWU lighting inputs on")
    parser.add_argument("--all-off", action="store_true", help="Turn all SWU lighting inputs off")
    parser.add_argument("--raw", type=lambda x: int(x, 0), help="Send raw byte value (e.g. 0x3F)")

    parser.add_argument("--repeat", "-r", type=float, default=0,
                        help="Repeat interval in seconds (0 = send once)")
    parser.add_argument("--listen", "-l", type=float, default=2.0,
                        help="Listen for LCU_STATUS replies for this many seconds after sending")
    parser.add_argument("--bitrate", "-b", type=int, default=500000,
                        help="CAN bitrate in bps")
    add_can_backend_args(parser)
    args = parser.parse_args()

    # Build the lighting mask
    if args.raw is not None:
        mask = args.raw & 0xFF
    elif args.all_on:
        mask = 0x3F
    elif args.all_off:
        mask = 0x00
    else:
        mask = 0
        flag_map = {
            "hazard":          BIT_HAZARD,
            "beam":            BIT_BEAM,
            "position":        BIT_POSITION,
            "right_indicator": BIT_RIGHT_INDICATOR,
            "left_indicator":  BIT_LEFT_INDICATOR,
            "brake":           BIT_BRAKE_LIGHT,
        }
        any_set = False
        for name, bit in flag_map.items():
            val = getattr(args, name)
            if val == "on":
                mask |= (1 << bit)
                any_set = True
            elif val == "off":
                any_set = True

        if not any_set:
            parser.print_help()
            print("\nError: specify at least one light, --all-on, --all-off, or --raw")
            sys.exit(1)

    print(f"LCU lighting control  ({backend_label(args)})")
    print(f"  Adapter index : {args.index}")
    print(f"  Bitrate       : {args.bitrate} bps")
    print()
    _print_mask(mask)
    print()

    try:
        with open_bus(args) as bus:
            frame = can.Message(
                arbitration_id=SWU_LCU_INPUTS_ID,
                is_extended_id=True,
                data=bytes([mask]),
            )

            if args.repeat > 0:
                print(f"Sending SWU_LCU_INPUTS every {args.repeat:.1f} s  (Ctrl+C to stop)")
                try:
                    while True:
                        bus.send(frame)
                        print(f"  sent 0x{mask:02X}", flush=True)
                        time.sleep(args.repeat)
                except KeyboardInterrupt:
                    print("\nStopped.")
            else:
                bus.send(frame)
                print("Sent SWU_LCU_INPUTS frame.")

                if args.listen > 0:
                    print(f"\nListening for LCU_STATUS (0x{LCU_STATUS_ID:03X}) for {args.listen:.0f} s ...")
                    deadline = time.monotonic() + args.listen
                    seen = 0
                    while time.monotonic() < deadline:
                        remaining = deadline - time.monotonic()
                        msg = bus.recv(timeout=min(remaining, 0.2))
                        if msg is None:
                            continue
                        if msg.arbitration_id == LCU_STATUS_ID and msg.is_extended_id:
                            decoded = _decode_lcu_status(msg.data[:msg.dlc])
                            print(f"  LCU_STATUS: {decoded}")
                            seen += 1
                    if seen == 0:
                        print("  No LCU_STATUS received.")

    except can.CanError as e:
        print(f"\nCAN bus error: {e}")
        if "NoBackendError" in str(e) or "No backend" in str(e):
            print("Hint: pip install libusb")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(1)
    except Exception as e:
        if "NoBackendError" in type(e).__name__ or "No backend" in str(e):
            print(f"\nError: libusb backend not found. Fix: pip install libusb")
            sys.exit(1)
        raise


if __name__ == "__main__":
    main()
