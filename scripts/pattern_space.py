#!/usr/bin/env python3
"""Count the walks a PatternLockActivity gate can produce.

The numbers in src/components/lock/PatternLock.h are this script's output, not an
estimate. Re-run it after any change to the grid size or the step rule, because a
comment that states a measured number and is no longer true is worse than one that
states nothing.

A walk is a start cell plus orthogonal single-cell steps. Revisits are allowed --
right-then-left comes back, and the gate does not forbid it -- so the count is a
plain breadth-first product over the adjacency, not a self-avoiding walk count.

  python3 scripts/pattern_space.py              # the table in the header
  python3 scripts/pattern_space.py --min 8 --max 12
"""

import argparse
import math

COLS = 4
ROWS = 4
CELLS = COLS * ROWS

# Cell 0 is a corner, 1 is an edge, 5 is one of the interior four. The three
# classes are all a 4x4 grid has, and the header quotes one column per class.
CLASSES = (("corner", 0), ("edge", 1), ("interior", 5))


def neighbours(cell):
    row, col = divmod(cell, COLS)
    out = []
    if col > 0:
        out.append(cell - 1)
    if col + 1 < COLS:
        out.append(cell + 1)
    if row > 0:
        out.append(cell - COLS)
    if row + 1 < ROWS:
        out.append(cell + COLS)
    return out


def walks(start, min_points, max_points):
    """Distinct walks of min_points..max_points points, start included."""
    # One counter per cell: how many walks of the current length end there. The
    # step is a single pass over the adjacency, so the whole table is linear in
    # max_points rather than exponential in it.
    ending_at = {start: 1}
    total = 0
    per_length = {}
    points = 1
    if min_points <= 1:
        total += 1
        per_length[1] = 1
    while points < max_points:
        nxt = {}
        for cell, count in ending_at.items():
            for m in neighbours(cell):
                nxt[m] = nxt.get(m, 0) + count
        ending_at = nxt
        points += 1
        reached = sum(ending_at.values())
        per_length[points] = reached
        if points >= min_points:
            total += reached
    return total, per_length


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--min", type=int, default=None, help="shortest pattern, points including the start")
    ap.add_argument("--max", type=int, default=None, help="longest pattern, points including the start")
    args = ap.parse_args()

    ranges = [(args.min or 4, args.max or 12)] if (args.min or args.max) else [(4, 8), (4, 12), (8, 12)]

    print(f"{'points':>9} {'corner':>10} {'edge':>10} {'interior':>10} {'bits':>7}")
    for lo, hi in ranges:
        counts = [walks(cell, lo, hi)[0] for _, cell in CLASSES]
        bits = math.log2(counts[2]) if counts[2] else 0.0
        print(f"{lo:>4} to {hi:<2} {counts[0]:>10,} {counts[1]:>10,} {counts[2]:>10,} {bits:>7.1f}")

    lo, hi = ranges[-1]
    _, per = walks(5, 1, hi)
    print(f"\nper length, from an interior cell (the floor is what an attacker searches first):")
    for points in sorted(per):
        if points < 2:
            continue
        print(f"  {points:>2} points ({points - 1:>2} presses): {per[points]:>12,}  {math.log2(per[points]):5.1f} bits")


if __name__ == "__main__":
    main()
