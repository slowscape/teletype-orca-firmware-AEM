"""Inspect module flash backups and carry saved scenes into a new firmware image.

A backup is the output of `dfu-programmer at32uc3b0512 read`. Scenes live in the
.flash_nvram block at 0x8004E000 and are erased with the firmware, so they have
to be merged into the hex that gets flashed.

    python scripts/scenes.py inspect backup/module-flash.hex
    python scripts/scenes.py merge module/orca.hex backup/module-flash.hex out.hex

`merge` writes the current layout (64x64). It reads both the 32x32 layout of
firmware 0.1-build 2 (FIRSTRUN 0x55; old grids go in the top-left corner) and the
64x64 layout (FIRSTRUN 0x64), and carries CV calibration over when present.
"""
import struct
import sys

NVRAM = 0x8004E000
NAME_LEN = 22
SLOTS = 32
CAL_MAGIC = 0x43414C31
CAL_SIZE = 20  # 4x u16 code_1v, 4x u16 code_4v, u32 magic


class Layout:
    def __init__(self, key, w, h):
        self.key, self.w, self.h = key, w, h
        self.scene_size = w * h + NAME_LEN + 1 + 9
        scenes_end = 2 + SLOTS * self.scene_size
        self.cal_offset = (scenes_end + 3) & ~3
        self.size = self.cal_offset + CAL_SIZE

    def scene_offset(self, slot):
        return 2 + slot * self.scene_size


V1 = Layout(0x55, 32, 32)
V2 = Layout(0x64, 64, 64)
CURRENT = V2


def load_hex(path):
    data, base = {}, 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(':'):
            continue
        n = int(line[1:3], 16)
        addr = int(line[3:7], 16)
        rtype = int(line[7:9], 16)
        payload = bytes.fromhex(line[9:9 + 2 * n])
        if rtype == 0:
            for i, b in enumerate(payload):
                data[base + addr + i] = b
        elif rtype == 2:
            base = int.from_bytes(payload, 'big') << 4
        elif rtype == 4:
            base = int.from_bytes(payload, 'big') << 16
    return data


def record(addr, rtype, payload):
    body = bytes([len(payload)]) + addr.to_bytes(2, 'big') + bytes([rtype]) + payload
    return ':' + (body + bytes([(-sum(body)) & 0xFF])).hex().upper()


def write_hex(path, data):
    out, upper = [], None
    addrs = sorted(data)
    i = 0
    while i < len(addrs):
        a = addrs[i]
        if a >> 16 != upper:
            upper = a >> 16
            out.append(record(0, 4, upper.to_bytes(2, 'big')))
        n = 1
        while (i + n < len(addrs) and n < 16 and addrs[i + n] == a + n
               and (a + n) >> 16 == upper):
            n += 1
        out.append(record(a & 0xFFFF, 0, bytes(data[a + k] for k in range(n))))
        i += n
    out.append(':00000001FF')
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


def read_block(data, layout):
    return bytes(data.get(NVRAM + i, 0xFF) for i in range(layout.size))


def detect(data):
    key = data.get(NVRAM, 0xFF)
    for layout in (V1, V2):
        if key == layout.key:
            return layout
    return None


def parse(block, layout):
    """-> last_scene, [(grid rows, name bytes, tempo, extra)], cal bytes or None

    extra is the 9 bytes after the tempo: swing (since build 5), then reserved.
    """
    scenes = []
    for s in range(SLOTS):
        o = layout.scene_offset(s)
        grid = block[o:o + layout.w * layout.h]
        rows = [grid[r * layout.w:(r + 1) * layout.w] for r in range(layout.h)]
        name = block[o + layout.w * layout.h:o + layout.w * layout.h + NAME_LEN]
        tempo = block[o + layout.w * layout.h + NAME_LEN]
        extra = block[o + layout.w * layout.h + NAME_LEN + 1:
                      o + layout.w * layout.h + NAME_LEN + 10]
        scenes.append((rows, name, tempo, extra))
    cal = block[layout.cal_offset:layout.cal_offset + CAL_SIZE]
    has_cal = struct.unpack_from('<I', cal, 16)[0] == CAL_MAGIC or \
        struct.unpack_from('>I', cal, 16)[0] == CAL_MAGIC
    return block[1], scenes, (cal if has_cal else None)


def build(layout, last_scene, scenes, cal):
    block = bytearray(b'\xFF' * layout.size)
    block[0] = layout.key
    block[1] = last_scene
    for s, (rows, name, tempo, extra) in enumerate(scenes):
        o = layout.scene_offset(s)
        grid = bytearray(b'.' * (layout.w * layout.h))
        for r, row in enumerate(rows[:layout.h]):
            row = row[:layout.w]
            grid[r * layout.w:r * layout.w + len(row)] = row
        block[o:o + len(grid)] = grid
        o += len(grid)
        block[o:o + NAME_LEN] = name
        block[o + NAME_LEN] = tempo
        block[o + NAME_LEN + 1:o + NAME_LEN + 10] = extra
    if cal is not None:
        block[layout.cal_offset:layout.cal_offset + CAL_SIZE] = cal
    return bytes(block)


def describe(layout, last_scene, scenes, cal):
    print(f'layout {layout.w}x{layout.h} (key 0x{layout.key:02x}), last scene {last_scene}, '
          f'calibration {"yes" if cal else "no"}')
    for s, (rows, name, tempo, extra) in enumerate(scenes):
        cells = sum(c != ord('.') for row in rows for c in row)
        label = name.split(b'\0')[0].decode('ascii', 'replace')
        swing = extra[0] if 50 <= extra[0] <= 75 else 50
        if cells or label:
            print(f'  slot {s:02d}: {label!r:24} tempo {tempo:3d} ms, swing {swing}%, {cells} cells')


def cmd_inspect(dump_path):
    data = load_hex(dump_path)
    layout = detect(data)
    if layout is None:
        sys.exit(f'no scene block found (first byte 0x{data.get(NVRAM, 0xFF):02x})')
    describe(layout, *parse(read_block(data, layout), layout))


def cmd_merge(fw_path, dump_path, out_path):
    fw = load_hex(fw_path)
    if any(NVRAM <= a < NVRAM + CURRENT.size for a in fw):
        sys.exit('firmware hex already contains scene data')
    data = load_hex(dump_path)
    layout = detect(data)
    if layout is None:
        sys.exit('no scene block found in the backup')
    last_scene, scenes, cal = parse(read_block(data, layout), layout)
    block = build(CURRENT, last_scene, scenes, cal)

    merged = dict(fw)
    for i, b in enumerate(block):
        if b != 0xFF:  # erased flash is 0xFF anyway
            merged[NVRAM + i] = b
    write_hex(out_path, merged)

    # read back and check: firmware unchanged, scenes as converted
    back = load_hex(out_path)
    assert all(back[a] == fw[a] for a in fw), 'firmware bytes changed'
    assert read_block(back, CURRENT) == block, 'scene block mismatch'
    print(f'converted {layout.w}x{layout.h} -> {CURRENT.w}x{CURRENT.h}, wrote {out_path}')
    describe(CURRENT, *parse(block, CURRENT))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'inspect':
        cmd_inspect(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == 'merge':
        cmd_merge(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)
