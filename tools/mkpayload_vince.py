#!/usr/bin/env python3
"""Pack an XNU Mach-O kernel + ADT into a bootable payload for Vince Boot Shim.

  mkpayload_vince.py KERNEL ADT OUT [--cmdline "..."] [--ramdisk IMAGE]

Adapted from upstream mkpayload.py for Xiaomi Redmi 5 Plus (vince / MSM8953):
- Magic: VINCEXNU (or SWEETXNU)
- Resolves LC_DYLD_CHAINED_FIXUPS so the kernel is pre-rebased at unslid VAs
- Physical RAM base: 0xa0000000 (1.5 GB contiguous upper DRAM on MSM8953)
- Places ADT and boot_args behind the highest kernel segment
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

LC_SEGMENT_64, LC_UNIXTHREAD, LC_DYLD_CHAINED_FIXUPS = 0x19, 0x5, 0x80000034
PAGE = 0x1000  # 4KB granule alignment
BOOT_LINE_LENGTH = 608
CMDLINE = 0x48
SEG_TABLE = CMDLINE + BOOT_LINE_LENGTH  # 0x2a8


def parse_macho(buf):
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', buf, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    segs, entry, fixups = [], None, None
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', buf, off)
        if cmd == LC_SEGMENT_64:
            name = buf[off + 8:off + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', buf, off + 24)
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
        elif cmd == LC_UNIXTHREAD:
            flavor, count = struct.unpack_from('<II', buf, off + 8)
            # arm_thread_state64: x0..x28, fp, lr, sp, pc, cpsr
            entry, = struct.unpack_from('<Q', buf, off + 16 + 32 * 8)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            fixups = struct.unpack_from('<II', buf, off + 8)
        off += size
    return dict(cputype=cputype, cpusub=cpusub & 0xff, segs=segs, entry=entry, fixups=fixups)


def decode(raw, fmt, image_base):
    if fmt in (2, 6):
        assert not raw >> 63, 'bind in kernel'
        target = raw & ((1 << 36) - 1)
        high8 = (raw >> 36) & 0xff
        nxt = (raw >> 51) & 0xfff
        if fmt == 6:
            target += image_base
        return target | (high8 << 56), nxt, 4
    if fmt == 8:
        target = raw & ((1 << 30) - 1)
        nxt = (raw >> 51) & 0xfff
        return image_base + target, nxt, 4
    if fmt in (1, 7, 9, 12):
        auth = raw >> 63
        nxt = (raw >> 51) & 0x7ff
        stride = 4 if fmt in (7, 12) else 8
        if auth:
            target = raw & 0xffffffff
        else:
            target = raw & ((1 << 43) - 1)
            target |= ((raw >> 43) & 0xff) << 56
        if fmt in (7, 9, 12) or auth:
            target += image_base
        return target, nxt, stride
    raise ValueError(f'unsupported chained pointer format {fmt}')


def apply_chained_fixups(buf, m):
    if not m['fixups']:
        return 0
    data_off, _ = m['fixups']
    _, starts_off, _, _, imports_count, _, _ = struct.unpack_from('<7I', buf, data_off)
    assert imports_count == 0, 'kernel has binds; only rebases are supported'
    image_base = m['segs'][0]['vmaddr']
    seg_count, = struct.unpack_from('<I', buf, data_off + starts_off)
    nfix = 0
    for i in range(seg_count):
        seg_info, = struct.unpack_from('<I', buf, data_off + starts_off + 4 + 4 * i)
        if not seg_info:
            continue
        base = data_off + starts_off + seg_info
        size, page_size, fmt, seg_offset, max_valid, page_count = struct.unpack_from('<IHHQIH', buf, base)
        starts = struct.unpack_from('<%dH' % page_count, buf, base + 22)
        seg = m['segs'][i]
        for p, start in enumerate(starts):
            if start == 0xffff:
                continue
            assert not start & 0x8000, 'DYLD_CHAINED_PTR_START_MULTI not supported'
            loc = seg['fileoff'] + p * page_size + start
            while True:
                raw, = struct.unpack_from('<Q', buf, loc)
                target, nxt, stride = decode(raw, fmt, image_base)
                struct.pack_into('<Q', buf, loc, target)
                nfix += 1
                if not nxt:
                    break
                loc += nxt * stride
    return nfix


def add_memory_map_entry(adt_blob, name, phys, size):
    """/chosen/memory-map NAME = (phys, size): md0's RAMDisk, or a blob a driver reads (GPU microcode)."""
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    mmap = next((c for c in chosen.children if c.name == 'memory-map'), None)
    if mmap is None:
        mmap = adt.Node()
        mmap.props = [('name', 0, b'memory-map\0')]
        chosen.children.append(mmap)
    assert mmap.prop(name) is None, f'ADT already has {name}'
    mmap.props.append((name, 0, struct.pack('<2Q', phys, size)))
    return adt.serialize(root)


def adt_dram(adt_blob):
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    return (struct.unpack('<Q', chosen.prop('dram-base'))[0],
            struct.unpack('<Q', chosen.prop('dram-size'))[0])


def build(kernel_path, adt_path, cmdline, ramdisk_path=None, blobs=()):
    buf = bytearray(open(kernel_path, 'rb').read())
    m = parse_macho(buf)
    nfix = apply_chained_fixups(buf, m)
    segs = [s for s in m['segs'] if s['vmsize']]
    text = m['segs'][0]
    assert text['name'] == '__TEXT' and text['fileoff'] == 0

    virt_base = text['vmaddr'] & ~(0x2000000 - 1)
    kernel_end = max(s['vmaddr'] + s['vmsize'] for s in segs)
    kernel_end = (kernel_end + PAGE - 1) & ~(PAGE - 1)

    adt_blob = open(adt_path, 'rb').read()
    phys_base, mem_size = adt_dram(adt_blob)
    pieces = [(s['vmaddr'], s['vmsize'], buf[s['fileoff']:s['fileoff'] + s['filesize']]) for s in segs]
    ramdisk = None

    extra = ([('RAMDisk', ramdisk_path)] if ramdisk_path else []) + list(blobs)
    placed = []
    for name, path in extra:
        image = open(path, 'rb').read()
        size = (len(image) + PAGE - 1) & ~(PAGE - 1)
        va = kernel_end
        kernel_end = va + size
        pa = va - virt_base + phys_base
        pieces.append((va, size, image))
        # md0 wants whole pages; a blob's exact length matters (microcode word count)
        adt_blob = add_memory_map_entry(adt_blob, name, pa, size if name == 'RAMDisk' else len(image))
        placed.append((name, hex(pa), hex(len(image))))
        if name == 'RAMDisk':
            ramdisk = (hex(pa), hex(size))

    hdr_size = SEG_TABLE + 32 * len(pieces)
    out = bytearray(hdr_size)
    seg_table = []
    for vmaddr, vmsize, data in pieces:
        off = len(out)
        out += data
        out += b'\0' * (-len(out) & 0xf)
        seg_table.append((vmaddr, vmsize, off, len(data)))
    adt_off = len(out)
    out += adt_blob
    out += b'\0' * (-len(out) & 0xf)

    # Magic: VINCEXNU
    struct.pack_into('<8sII', out, 0, b'VINCEXNU', 1, len(pieces))
    struct.pack_into('<7Q', out, 0x10, virt_base, m['entry'], adt_off, len(adt_blob), kernel_end,
                     phys_base, mem_size)
    cl = cmdline.encode()[:BOOT_LINE_LENGTH - 1]
    out[CMDLINE:CMDLINE + len(cl)] = cl
    for i, t in enumerate(seg_table):
        struct.pack_into('<4Q', out, SEG_TABLE + 32 * i, *t)

    info = dict(cputype=hex(m['cputype']), cpusub=m['cpusub'], fixups=nfix, virt_base=hex(virt_base),
                entry=hex(m['entry']), kernel_end=hex(kernel_end), phys_base=hex(phys_base),
                mem_size=hex(mem_size), ramdisk=ramdisk, blobs=placed, size=len(out),
                segs=[(s['name'], hex(s['vmaddr']), hex(s['vmsize'])) for s in segs])
    return bytes(out), info


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Package Vince XNU kernel + ADT into payload')
    ap.add_argument('kernel', help='Path to kernel.development.vince Mach-O')
    ap.add_argument('adt', help='Path to vince.adt')
    ap.add_argument('out', help='Output payload path (e.g. artifacts/vince.payload)')
    ap.add_argument('--cmdline', default='debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 wdt_test=no_user_checkin sweettouch=0 sweetkeys=0')
    ap.add_argument('--ramdisk', help='Optional root image for md0')
    ap.add_argument('--blob', action='append', default=[], metavar='NAME=FILE',
                    help='a file under /chosen/memory-map NAME (e.g. GPU-PM4=a530_pm4.bin)')
    a = ap.parse_args()
    blob, info = build(a.kernel, a.adt, a.cmdline, a.ramdisk, [b.split('=', 1) for b in a.blob])
    open(a.out, 'wb').write(blob)
    print(f"[+] Vince XNU Payload written to {a.out} ({len(blob):,} bytes)")
    for k, v in info.items():
        if k != 'segs':
            print(f"  {k}: {v}")
    for s in info['segs']:
        print(f"  seg {s[0]}: {s[1]} (size: {s[2]})")
