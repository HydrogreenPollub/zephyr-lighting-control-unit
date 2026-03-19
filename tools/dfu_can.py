"""
Firmware updater via CAN DFU protocol — Candlelight USB adapter.

Sends a signed firmware binary to the device using the DFU protocol over CAN.
The device must be running and the DFU module must be active.

Protocol:
  1. Send REQUEST  (0x7E0): byte[0]=0x01, byte[1-4]=image_size (LE)
  2. Device replies (0x7E2): byte[0]=0x00 (OK)
  3. Send DATA     (0x7E1): byte[0-1]=seq (LE), byte[2-7]=up to 6 bytes of firmware
  4. Send COMMIT   (0x7E0): byte[0]=0xFF, byte[1-4]=CRC32 (LE)
  5. Device replies (0x7E2): byte[0]=0x00 (OK), then reboots after 1 s

Usage:
    python dfu_can.py build/zephyr/zephyr.signed.bin
    python dfu_can.py build/zephyr/zephyr.signed.bin --bitrate 250000
    python dfu_can.py build/zephyr/zephyr.signed.bin --index 1   # second adapter

Requirements:
    pip install python-can[gs_usb] libusb
    Windows: WinUSB driver (already installed with candleLight) + libusb DLL via `pip install libusb`
    No driver change needed — SavvyCAN continues to work alongside this script.
"""

import argparse
import struct
import time
import zlib
import sys
from pathlib import Path

try:
    import can
except ImportError:
    print("Error: python-can not installed. Run: pip install python-can[gs_usb] libusb")
    sys.exit(1)

def _patch_libusb() -> None:
    """
    Point pyusb at the libusb-1.0.dll bundled by the `libusb` PyPI package.

    pyusb's get_backend() searches PATH and system dirs, which misses the DLL
    installed inside site-packages.  Patching it once here fixes gs_usb.scan().
    """
    try:
        import libusb as _libusb_pkg
        import usb.backend.libusb1 as _lb1
        dll_path = _libusb_pkg.dll._name          # absolute path to the DLL
        _orig = _lb1.get_backend
        _lb1.get_backend = lambda find_library=None: _orig(
            find_library=lambda _: dll_path
        )
    except Exception:
        pass  # will surface as a clearer error when the bus is opened

_patch_libusb()

# ── CAN IDs (extended frames) ──────────────────────────────────────────────────
DFU_CMD_ID  = 0x7E4   # host → device: REQUEST or COMMIT
DFU_DATA_ID = 0x7E5   # host → device: firmware chunks
DFU_RSP_ID  = 0x7E6   # device → host: status

# ── Command bytes ──────────────────────────────────────────────────────────────
CMD_REQUEST = 0x01
CMD_COMMIT  = 0xFF

# ── Status bytes ───────────────────────────────────────────────────────────────
STATUS_OK         = 0x00
STATUS_CRC_FAIL   = 0x01
STATUS_WRITE_FAIL = 0x02
STATUS_SEQ_FAIL   = 0x03

_STATUS_NAMES = {
    STATUS_OK:         "OK",
    STATUS_CRC_FAIL:   "CRC_FAIL",
    STATUS_WRITE_FAIL: "WRITE_FAIL",
    STATUS_SEQ_FAIL:   "SEQ_FAIL",
}

DATA_PAYLOAD = 6  # bytes per data frame (DFU_DATA_PAYLOAD_LEN on device)

# Must match CAN_DFU_BATCH_SIZE in can_dfu.h.
# Device sends STATUS_OK every this many DATA frames; host waits for it so
# the gs_usb TX-echo pool (64 slots on candlelight) never exhausts.
DFU_BATCH_SIZE = 32


def _status_str(code: int) -> str:
    return _STATUS_NAMES.get(code, f"UNKNOWN(0x{code:02X})")


def _wait_response(bus: "can.BusABC", timeout: float = 5.0) -> int:
    """Block until a DFU_RSP_ID frame arrives and return the status byte."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        msg = bus.recv(timeout=min(remaining, 0.1))
        if msg is None:
            continue
        if msg.arbitration_id == DFU_RSP_ID and msg.is_extended_id and msg.dlc >= 1:
            return msg.data[0]
    raise TimeoutError(f"No response from device within {timeout:.1f} s")


def _send_request(bus: "can.BusABC", image_size: int) -> None:
    data = bytes([CMD_REQUEST]) + struct.pack("<I", image_size)
    bus.send(can.Message(arbitration_id=DFU_CMD_ID, is_extended_id=True, data=data))


def _send_data(bus: "can.BusABC", seq: int, payload: bytes) -> None:
    data = struct.pack("<H", seq) + payload
    bus.send(can.Message(arbitration_id=DFU_DATA_ID, is_extended_id=True, data=data))


def _send_commit(bus: "can.BusABC", crc32: int) -> None:
    data = bytes([CMD_COMMIT]) + struct.pack("<I", crc32)
    bus.send(can.Message(arbitration_id=DFU_CMD_ID, is_extended_id=True, data=data))


def run_dfu(bus: "can.BusABC", firmware: bytes, frame_delay_ms: float) -> None:
    image_size   = len(firmware)
    total_frames = (image_size + DATA_PAYLOAD - 1) // DATA_PAYLOAD
    frame_delay  = frame_delay_ms / 1000.0

    print(f"  Image size   : {image_size} bytes ({image_size / 1024:.1f} KB)")
    print(f"  Data frames  : {total_frames}")
    print(f"  Frame delay  : {frame_delay_ms:.1f} ms")
    print()

    t_total = time.monotonic()

    # ── Step 1: REQUEST ────────────────────────────────────────────────────────
    print("→ Sending REQUEST ...", end=" ", flush=True)
    print("(device will pre-erase ~3 s before ACKing)", flush=True)
    _send_request(bus, image_size)
    status = _wait_response(bus, timeout=15.0)
    if status != STATUS_OK:
        raise RuntimeError(f"REQUEST rejected with status {_status_str(status)}")
    print("OK")

    # ── Step 2: DATA frames ────────────────────────────────────────────────────
    print("→ Uploading firmware ...")
    crc  = 0
    seq  = 0
    sent = 0
    t0   = time.monotonic()

    while sent < image_size:
        chunk = firmware[sent : sent + DATA_PAYLOAD]

        # CRC matches Zephyr's crc32_ieee_update() accumulation
        crc = zlib.crc32(chunk, crc) & 0xFFFFFFFF

        _send_data(bus, seq, chunk)
        sent += len(chunk)
        seq  += 1

        elapsed = time.monotonic() - t0
        pct     = sent / image_size * 100
        kbps    = (sent / 1024) / elapsed if elapsed > 0 else 0
        print(f"\r     {pct:5.1f}%  {sent:>6}/{image_size} B  {kbps:5.1f} KB/s  frame {seq - 1}", end="", flush=True)

        # Wait for the device's intermediate batch ACK every DFU_BATCH_SIZE
        # frames.  The device sends STATUS_OK on the response ID to confirm
        # receipt; receiving it forces a USB-IN read that drains pending TX-echo
        # notifications in the gs_usb driver, preventing TX pool exhaustion.
        if seq % DFU_BATCH_SIZE == 0:
            status = _wait_response(bus, timeout=5.0)
            if status != STATUS_OK:
                raise RuntimeError(
                    f"Batch ACK at frame {seq} failed: {_status_str(status)}"
                )

        if frame_delay > 0:
            time.sleep(frame_delay)

    elapsed = time.monotonic() - t0
    print(f"\r     100.0%  {image_size}/{image_size} B  "
          f"{image_size / 1024 / elapsed:.1f} KB/s  {total_frames} frames sent  ({elapsed:.1f} s)")

    # ── Step 3: COMMIT ─────────────────────────────────────────────────────────
    print(f"→ Sending COMMIT (CRC32=0x{crc:08X}) ...", end=" ", flush=True)
    _send_commit(bus, crc)
    status = _wait_response(bus, timeout=10.0)
    if status != STATUS_OK:
        raise RuntimeError(f"COMMIT failed with status {_status_str(status)}")
    print("OK")

    t_total = time.monotonic() - t_total
    print()
    print(f"DFU complete in {t_total:.1f} s — device will reboot in ~1 s.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Firmware updater via CAN DFU (Candlelight USB adapter)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "firmware",
        help="Signed firmware binary (build/zephyr/zephyr.signed.bin)",
    )
    parser.add_argument(
        "--bitrate", "-b", type=int, default=500000,
        help="CAN bitrate in bps",
    )
    parser.add_argument(
        "--index", type=int, default=0,
        help="Adapter index (0 = first Candlelight adapter)",
    )
    parser.add_argument(
        "--delay", "-d", type=float, default=5.0,
        help="Inter-frame delay in ms. Increase if the device reports SEQ_FAIL.",
    )
    args = parser.parse_args()

    fw_path = Path(args.firmware)
    if not fw_path.exists():
        print(f"Error: firmware file not found: {fw_path}")
        sys.exit(1)

    firmware = fw_path.read_bytes()
    if not firmware:
        print("Error: firmware file is empty")
        sys.exit(1)

    print("DFU over CAN  (Candlelight / gs_usb)")
    print(f"  Adapter index : {args.index}")
    print(f"  Bitrate       : {args.bitrate} bps")
    print(f"  Firmware      : {fw_path}")
    print()

    try:
        with can.Bus(interface="gs_usb", channel=0, bitrate=args.bitrate, index=args.index) as bus:
            run_dfu(bus, firmware, frame_delay_ms=args.delay)
    except TimeoutError as e:
        print(f"\nError: {e}")
        sys.exit(1)
    except RuntimeError as e:
        print(f"\nError: {e}")
        sys.exit(1)
    except can.CanError as e:
        print(f"\nCAN bus error: {e}")
        if "NoBackendError" in str(e) or "No backend" in str(e):
            print("Hint: install the libusb DLL with: pip install libusb")
        sys.exit(1)
    except Exception as e:
        if "NoBackendError" in type(e).__name__ or "No backend" in str(e):
            print(f"\nError: libusb backend not found.")
            print("Fix: pip install libusb   (no driver change needed, WinUSB stays as-is)")
            sys.exit(1)
        raise
    except KeyboardInterrupt:
        print("\nAborted.")
        sys.exit(1)


if __name__ == "__main__":
    main()
