#!/usr/bin/env python3
"""
Generate a deterministic, well-formed synthetic NASDAQ ITCH 5.0 binary file.

Each generated order_ref follows a valid lifecycle:
  Add -> (optional partial Cancel/Execute) -> Delete or full Execute.
No orphaned references. No double-deletes.

Usage:
  python scripts/gen_mock_itch.py \
      --output data/mock.NASDAQ_ITCH50 \
      --num-events 1000000 \
      --seed 42

Run with --validate to re-parse and verify the file after writing.
"""

import argparse
import random
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional


# ITCH 5.0 message packing
# All numeric fields are big-endian ('>').
# Each message is preceded by a 2-byte big-endian length field that does NOT
# include the 2 bytes of the length field itself.

def pack_msg(msg_type: bytes, payload: bytes) -> bytes:
    body = msg_type + payload
    return struct.pack('>H', len(body)) + body


def encode_timestamp(ns: int) -> bytes:
    """Encode nanoseconds-from-midnight as 6-byte big-endian."""
    b = []
    for _ in range(6):
        b.append(ns & 0xFF)
        ns >>= 8
    return bytes(reversed(b))


def msg_add_order(stock_locate: int, ts_ns: int, order_ref: int,
                  side: str, shares: int, stock: str, price: int) -> bytes:
    stock_padded = stock.encode('ascii').ljust(8)[:8]
    payload = struct.pack('>HH', stock_locate, 0)  # locate, tracking
    payload += encode_timestamp(ts_ns)
    payload += struct.pack('>Q', order_ref)         # order_reference
    payload += side.encode('ascii')                  # side B/S
    payload += struct.pack('>I', shares)            # shares
    payload += stock_padded                         # stock[8]
    payload += struct.pack('>I', price)             # price
    return pack_msg(b'A', payload)


def msg_execute_order(stock_locate: int, ts_ns: int, order_ref: int,
                      executed_shares: int) -> bytes:
    payload = struct.pack('>HH', stock_locate, 0)
    payload += encode_timestamp(ts_ns)
    payload += struct.pack('>Q', order_ref)
    payload += struct.pack('>I', executed_shares)
    payload += struct.pack('>Q', 0)  # match_number
    return pack_msg(b'E', payload)


def msg_cancel_order(stock_locate: int, ts_ns: int, order_ref: int,
                     cancelled_shares: int) -> bytes:
    payload = struct.pack('>HH', stock_locate, 0)
    payload += encode_timestamp(ts_ns)
    payload += struct.pack('>Q', order_ref)
    payload += struct.pack('>I', cancelled_shares)
    return pack_msg(b'X', payload)


def msg_delete_order(stock_locate: int, ts_ns: int, order_ref: int) -> bytes:
    payload = struct.pack('>HH', stock_locate, 0)
    payload += encode_timestamp(ts_ns)
    payload += struct.pack('>Q', order_ref)
    return pack_msg(b'D', payload)


def msg_replace_order(stock_locate: int, ts_ns: int, orig_ref: int,
                      new_ref: int, shares: int, price: int) -> bytes:
    payload = struct.pack('>HH', stock_locate, 0)
    payload += encode_timestamp(ts_ns)
    payload += struct.pack('>Q', orig_ref)
    payload += struct.pack('>Q', new_ref)
    payload += struct.pack('>I', shares)
    payload += struct.pack('>I', price)
    return pack_msg(b'U', payload)


# Generator

@dataclass
class LiveOrder:
    ref: int
    stock_locate: int
    side: str
    shares: int
    price: int


def generate(
    output: Path,
    num_events: int,
    num_symbols: int,
    seed: int,
    price_center: int,
    price_range: int,
    validate: bool,
) -> None:
    rng = random.Random(seed)

    symbols = [f"SYM{i:<5}"[:6].strip() for i in range(num_symbols)]
    stock_locates = list(range(1, num_symbols + 1))

    # Message mix weights (Add, Cancel, Execute, Delete, Replace)
    MIX = {'A': 0.60, 'X': 0.15, 'E': 0.15, 'D': 0.08, 'U': 0.02}
    choices = []
    weights = []
    for k, v in MIX.items():
        choices.append(k)
        weights.append(v)

    live: Dict[int, LiveOrder] = {}  # order_ref -> LiveOrder
    next_ref = 1
    ts_ns = int(9.5 * 3600 * 1e9)   # start at 9:30 AM in nanoseconds
    events_written = 0
    messages: List[bytes] = []

    def flush():
        nonlocal events_written
        with open(output, 'wb') as f:
            for m in messages:
                f.write(m)
        events_written = len(messages)

    max_iter = num_events * 10  # guard against infinite loop

    for _ in range(max_iter):
        if events_written >= num_events:
            break

        ts_ns += rng.randint(100, 10_000)  # monotone nanosecond increment
        sym_idx = rng.randrange(num_symbols)
        stock_locate = stock_locates[sym_idx]
        symbol = symbols[sym_idx]

        msg_type = rng.choices(choices, weights)[0]

        # Fall back to Add if no live orders to manipulate.
        if msg_type in ('X', 'E', 'D', 'U') and not live:
            msg_type = 'A'

        if msg_type == 'A':
            ref = next_ref; next_ref += 1
            side = rng.choice(('B', 'S'))
            shares = rng.randint(1, 1000)
            # Keep bids strictly below price_center and asks strictly above to
            # prevent synthetic crossed markets in the generated book.
            if side == 'B':
                price = price_center - rng.randint(1, price_range)
            else:
                price = price_center + rng.randint(1, price_range)
            price = max(1, price)
            messages.append(msg_add_order(stock_locate, ts_ns, ref, side, shares, symbol, price))
            live[ref] = LiveOrder(ref, stock_locate, side, shares, price)

        elif msg_type == 'X':  # partial cancel
            if not live: continue
            o = rng.choice(list(live.values()))
            if o.shares <= 1: continue  # can't partially cancel a 1-share order
            cancelled = rng.randint(1, o.shares - 1)
            messages.append(msg_cancel_order(o.stock_locate, ts_ns, o.ref, cancelled))
            o.shares -= cancelled

        elif msg_type == 'E':  # partial or full execute
            if not live: continue
            o = rng.choice(list(live.values()))
            exec_qty = rng.randint(1, o.shares)
            messages.append(msg_execute_order(o.stock_locate, ts_ns, o.ref, exec_qty))
            o.shares -= exec_qty
            if o.shares == 0:
                del live[o.ref]

        elif msg_type == 'D':  # delete
            if not live: continue
            o = rng.choice(list(live.values()))
            messages.append(msg_delete_order(o.stock_locate, ts_ns, o.ref))
            del live[o.ref]

        elif msg_type == 'U':  # replace
            if not live: continue
            o = rng.choice(list(live.values()))
            new_ref = next_ref; next_ref += 1
            if o.side == 'B':
                new_price = price_center - rng.randint(1, price_range)
            else:
                new_price = price_center + rng.randint(1, price_range)
            new_price = max(1, new_price)
            new_shares = rng.randint(1, 1000)
            messages.append(msg_replace_order(
                o.stock_locate, ts_ns, o.ref, new_ref, new_shares, new_price))
            del live[o.ref]
            live[new_ref] = LiveOrder(new_ref, o.stock_locate, o.side, new_shares, new_price)

        else:
            continue

        events_written += 1
        if events_written % 100_000 == 0:
            print(f"  {events_written}/{num_events} events ...", flush=True)

    # Delete all remaining live orders at end-of-day.
    for o in list(live.values()):
        ts_ns += 1000
        messages.append(msg_delete_order(o.stock_locate, ts_ns, o.ref))

    with open(output, 'wb') as f:
        for m in messages:
            f.write(m)

    total = len(messages)
    size_mb = output.stat().st_size / (1024 * 1024)
    print(f"Wrote {total} messages ({size_mb:.1f} MB) -> {output}")

    if validate:
        _validate(output)


def _validate(path: Path) -> None:
    """Re-parse the generated file and verify all lifecycle rules."""
    print(f"Validating {path} ...")
    live: Dict[int, dict] = {}
    deleted = set()
    errors = 0
    n = 0

    with open(path, 'rb') as f:
        data = f.read()

    pos = 0
    while pos + 2 <= len(data):
        msg_len = struct.unpack('>H', data[pos:pos+2])[0]
        if pos + 2 + msg_len > len(data):
            print(f"  WARN: truncated message at offset {pos}")
            break
        msg = data[pos+2 : pos+2+msg_len]
        pos += 2 + msg_len
        n += 1

        if not msg:
            continue
        mtype = chr(msg[0])

        # Message body layout (msg[0] = type):
        #   msg[1:3]   stock_locate (2)
        #   msg[3:5]   tracking_number (2)
        #   msg[5:11]  timestamp (6)
        #   msg[11:19] order_reference (8)
        #   ...rest depends on message type

        if mtype == 'A':
            ref    = struct.unpack('>Q', msg[11:19])[0]
            shares = struct.unpack('>I', msg[20:24])[0]   # after side(1) at msg[19]
            if ref in live:
                print(f"  ERROR: double-add of ref {ref}"); errors += 1
            if ref in deleted:
                print(f"  ERROR: add of previously deleted ref {ref}"); errors += 1
            live[ref] = {'shares': shares}

        elif mtype == 'E':
            ref      = struct.unpack('>Q', msg[11:19])[0]
            exec_qty = struct.unpack('>I', msg[19:23])[0]
            if ref not in live:
                print(f"  ERROR: execute unknown ref {ref}"); errors += 1; continue
            live[ref]['shares'] -= exec_qty
            if live[ref]['shares'] < 0:
                print(f"  ERROR: execute > remaining shares at ref {ref}"); errors += 1
            if live[ref]['shares'] == 0:
                del live[ref]; deleted.add(ref)

        elif mtype == 'X':
            ref       = struct.unpack('>Q', msg[11:19])[0]
            cancelled = struct.unpack('>I', msg[19:23])[0]
            if ref not in live:
                print(f"  ERROR: cancel unknown ref {ref}"); errors += 1; continue
            live[ref]['shares'] -= cancelled
            if live[ref]['shares'] <= 0:
                print(f"  ERROR: partial cancel leaves ≤0 shares at ref {ref}"); errors += 1

        elif mtype == 'D':
            ref = struct.unpack('>Q', msg[11:19])[0]
            if ref not in live:
                print(f"  ERROR: delete unknown ref {ref}"); errors += 1; continue
            del live[ref]; deleted.add(ref)

        elif mtype == 'U':
            orig_ref = struct.unpack('>Q', msg[11:19])[0]
            new_ref  = struct.unpack('>Q', msg[19:27])[0]
            shares   = struct.unpack('>I', msg[27:31])[0]
            if orig_ref not in live:
                print(f"  ERROR: replace unknown orig_ref {orig_ref}"); errors += 1
            else:
                del live[orig_ref]; deleted.add(orig_ref)
            live[new_ref] = {'shares': shares}

    print(f"  Parsed {n} messages. Live at end: {len(live)}. Errors: {errors}.")
    if errors:
        print("  VALIDATION FAILED")
        sys.exit(1)
    else:
        print("  VALIDATION PASSED ✓")


# CLI

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output',       required=True,   help='Output file path')
    parser.add_argument('--num-events',   type=int, default=1_000_000)
    parser.add_argument('--num-symbols',  type=int, default=10)
    parser.add_argument('--seed',         type=int, default=42)
    parser.add_argument('--price-center', type=int, default=500_000,
                        help='Center price in $0.0001 units (default: 500000 = $50)')
    parser.add_argument('--price-range',  type=int, default=50_000,
                        help='Half-range of price variation in $0.0001 units')
    parser.add_argument('--validate', action='store_true',
                        help='Re-parse and validate the output file after writing')
    args = parser.parse_args()

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    generate(
        output=output,
        num_events=args.num_events,
        num_symbols=args.num_symbols,
        seed=args.seed,
        price_center=args.price_center,
        price_range=args.price_range,
        validate=args.validate,
    )


if __name__ == '__main__':
    main()
