"""Rewrite the .tip fixtures this test reads, with the tile generator's writer.

The format has two implementations in two languages, and a fixture written by
one and read by the other is the only thing that keeps them agreeing. So these
bytes are never hand-built here: they are `mapbuilder/tilegen/point_file.py`'s
own output, and this script is how the next session reproduces them instead of
guessing what the numbers were.

    python3 make_fixtures.py --tilegen ~/Development/xteink/mapbuilder/tilegen

`shard_v1.tip` is deliberately NOT rewritten. It is the bytes a released
version-1 writer produced, and its whole job is to prove this reader still
walks a shard that is already on a card and on the CDN. Regenerating it from a
current writer would quietly redefine what version 1 was, and the test would
then only prove the writer agrees with itself.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The six safety points the tests assert on, in Sološnica and around: two water
# (one unverified), a seasonal+fee hut, an unnamed hospital, a restricted
# pharmacy and a transport point. Chosen so the flag mask, the name pool and the
# sort order all have something to prove. They are the same six the version-1
# fixture holds, so the two files differ only in layout.
SAFETY = [
    (48.600, 17.300, "water", ["unverified"], "Spring"),
    (48.610, 17.310, "water", [], "Drinking water"),
    (48.620, 17.320, "hut", ["seasonal", "fee"], "Chata Vrátna"),
    (48.590, 17.290, "hospital", [], ""),
    (48.615, 17.305, "pharmacy", ["restricted"], "Lekáreň"),
    (48.605, 17.295, "transport", [], "Sološnica, obec"),
]

# Two landmarks in a different z10 shard, carrying the version-2 fields. A peak
# with a height is the record `ele` exists for, and the second one has none --
# so the test sees both a real height and the unknown sentinel in one file, and
# sees kindsPresent report landmarks rather than safety.
LANDMARKS = [
    (49.164, 20.134, 2655, 0, "Gerlachovský štít"),
    (49.180, 20.090, None, 3, "Sedlo"),
]

BUILD_EPOCH = 1755800000


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tilegen", required=True,
                        help="path to the explorink-tilegen checkout (mapbuilder/tilegen)")
    args = parser.parse_args()
    sys.path.insert(0, os.path.abspath(args.tilegen))
    import point_enum
    import point_file
    import tiles

    def merc(lat, lon):
        x, y = tiles.lonlat_to_merc(lat, lon)
        return round(x), round(y)

    safety = []
    for lat, lon, category, reliability, name in SAFETY:
        x, y = merc(lat, lon)
        safety.append({
            "kind": point_enum.KIND_SAFETY,
            "category": point_enum.SAFETY_CATEGORY_ID[category],
            "flags": point_file.flags_from_reliability(reliability),
            "x": x, "y": y, "name": name, "ele": None, "rank": 0,
        })

    landmarks = []
    for lat, lon, ele, rank, name in LANDMARKS:
        x, y = merc(lat, lon)
        landmarks.append({
            "kind": point_enum.KIND_LANDMARK,
            "category": point_enum.LANDMARK_CATEGORY_ID["unknown"],
            "flags": 0, "x": x, "y": y, "name": name, "ele": ele, "rank": rank,
        })

    def write(relpath, points):
        path = os.path.join(HERE, relpath)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        data = point_file.pack(points, BUILD_EPOCH)
        with open(path, "wb") as f:
            f.write(data)
        header, back = point_file.unpack(data)
        print(f"{relpath}: {len(data)} bytes, version {header['version']}, "
              f"{header['count']} points, names_len {header['names_len']}")
        for p in back:
            print(f"    {{{p['x']}, {p['y']}, {p['kind']}, {p['category']}, "
                  f"0x{p['flags']:02x}, \"{p['name']}\"}}, ele={p['ele']} rank={p['rank']}")

    write("shard.tip", safety)
    # The same bytes again at the path MapPointShards::buildPath() produces, so
    # the radius query can run against this directory as its root.
    col, row = point_file.shard_of(safety[0]["x"], safety[0]["y"])
    write(os.path.join("points", "10", str(col), f"{row}.tip"), safety)
    write("landmarks.tip", landmarks)


if __name__ == "__main__":
    sys.exit(main())
