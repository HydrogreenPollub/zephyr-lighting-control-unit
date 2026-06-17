"""Helpers for opening CAN adapters used by the local tools."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

import can


GS_USB = "gs_usb"
PCAN = "pcan"
AUTO = "auto"


@dataclass(frozen=True)
class BusCandidate:
    label: str
    kwargs: dict


def add_can_backend_args(parser) -> None:
    parser.add_argument(
        "--interface",
        "-i",
        choices=[AUTO, GS_USB, PCAN],
        default=AUTO,
        help="CAN adapter backend. 'auto' tries gs_usb first, then PeakCAN/PCAN.",
    )
    parser.add_argument(
        "--index",
        type=int,
        default=0,
        help="Adapter index (gs_usb) or USB bus number offset (pcan: 0 -> PCAN_USBBUS1).",
    )
    parser.add_argument(
        "--channel",
        help="Explicit python-can channel, e.g. 0 for gs_usb or PCAN_USBBUS1 for PeakCAN.",
    )


def backend_label(args) -> str:
    if args.interface == AUTO:
        return "auto (gs_usb -> PeakCAN/PCAN)"
    if args.interface == PCAN:
        return "PeakCAN / PCAN"
    return "Candlelight / gs_usb"


def _gs_usb_kwargs(args) -> dict:
    channel = int(args.channel, 0) if args.channel is not None else 0
    return {
        "interface": GS_USB,
        "channel": channel,
        "bitrate": args.bitrate,
        "index": args.index,
    }


def _pcan_channel(args) -> str:
    if args.channel is not None:
        return args.channel
    return f"PCAN_USBBUS{args.index + 1}"


def _pcan_kwargs(args) -> dict:
    return {
        "interface": PCAN,
        "channel": _pcan_channel(args),
        "bitrate": args.bitrate,
    }


def _candidates(args) -> Iterable[BusCandidate]:
    if args.interface in (AUTO, GS_USB):
        yield BusCandidate("gs_usb", _gs_usb_kwargs(args))
    if args.interface in (AUTO, PCAN):
        yield BusCandidate("PeakCAN/PCAN", _pcan_kwargs(args))


def open_bus(args):
    errors = []
    for candidate in _candidates(args):
        try:
            bus = can.Bus(**candidate.kwargs)
            print(f"  CAN backend   : {candidate.label}")
            print(f"  CAN channel   : {candidate.kwargs.get('channel')}")
            return bus
        except Exception as exc:
            errors.append((candidate.label, exc))
            if args.interface != AUTO:
                raise

    lines = ["Could not open any CAN backend:"]
    for label, exc in errors:
        lines.append(f"  {label}: {exc}")
        if label == "PeakCAN/PCAN":
            try:
                configs = can.detect_available_configs([PCAN])
            except Exception as detect_exc:
                lines.append(f"    PCAN detection failed: {detect_exc}")
            else:
                if configs:
                    lines.append(f"    Detected PCAN channels: {configs}")
                else:
                    lines.append("    No PCAN channels detected.")
    raise can.CanError("\n".join(lines))
