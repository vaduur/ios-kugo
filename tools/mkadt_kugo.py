#!/usr/bin/env python3
"""Build the Apple Device Tree (ADT) for Sony Xperia X Compact (kugo, MSM8956).

Adapted from mkadt_vince.py (Redmi 5 Plus, MSM8953). Values below come from
the phone's live Linux DT (/sys/firmware/fdt) and the iSony shim probes:
- GICv2 (qcom,msm-qgic2) Distributor @ 0x0b000000, CPU Interface @ 0x0b002000
- arch timer 19.2 MHz, virtual timer PPI 4 = INTID 20
- LK splash framebuffer @ 0x83200000, 720x1280, 24-bpp (B, G, R), stride 2160,
  command-mode panel (the kernel kicks MDP CTL0 START per frame)
- XNU DRAM 0x90000000..0xd3c00000: above splash/SMEM/modem/reloc/tz_app,
  below the CMA pools Linux puts at the top
- SDHCI eMMC @ 0x07824900, hc_irq SPI 123
- first boot: cpu0 (Cortex-A53, MPIDR 0) only
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

PHYS_BASE = 0x90000000
MEM_SIZE = 0x43c00000

PANIC_LOG_SIZE = 0x100000

# Non-zero SOC base for arm-io so pe_arm_get_soc_base_phys() returns non-zero
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x0b000000, 0x1000
GICC, GICC_SIZE = 0x0b002000, 0x1000

FB_W, FB_H, FB_STRIDE = 720, 1280, 720 * 3
FB, FB_SIZE = 0x83200000, FB_STRIDE * FB_H

SDHCI, SDHCI_SIZE = 0x07824900, 0x1000

TIMEBASE = 19200000
VTIMER_INTID = 20

KUGO_ECID = 0x4b55474f00000001  # 'KUGO' + 1
BOOT_MANIFEST_HASH = hashlib.sha384(b'kugo boot manifest').digest()

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build_kugo_adt(seed=None):
    phys_base = PHYS_BASE
    mem_size = MEM_SIZE

    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE

    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('display-scale', u32(3)),
        ('display-rotation', u32(0)),
        ('unique-chip-id', u64(KUGO_ECID)),
        ('chip-id', u32(0x8015)),  # Apple A11 (iPhone X), the userland we run
        ('board-id', u32(0x16)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('isony-shim')),
        ('system-firmware-version', cstr('isony-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [
        node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])
    ])

    gic_props = lambda: [
        ph(),
        ('compatible', cstr('arm,cortex-a15-gic') + cstr('arm,gic-400') + b'\0'),
        ('reg', u64(GICD, GICD_SIZE, GICC, GICC_SIZE)),
        ('interrupt-controller', b''),
        ('#interrupt-cells', u32(3)),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
    ]
    gic = node('interrupt-controller', gic_props())
    gic_alias = node('gic', gic_props())

    wdt = node('wdt', [ph(), ('no-pmu', u32(1))])

    disp0 = node('disp0', [
        ph(),
        ('device_type', cstr('display-subsystem')),
        ('compatible', cstr('kugo,display') + cstr('vince,display') + cstr('sweet,display') + b'\0'),
        ('canvas-width', u32(1125)),
        ('canvas-height', u32(2436)),
    ])

    sdhci_node = node('sdhci', [
        ph(),
        ('compatible', cstr('generic-sdhci') + cstr('qcom,sdhci-msm-v4') + b'\0'),
        ('reg', u64(SDHCI, SDHCI_SIZE)),
        ('interrupts', u32(0, 123, 4)),
    ])

    # Stub nubs so MobileGestalt finds WifiAddress/BluetoothAddress (see mkadt_vince.py).
    wlan = node('wlan', [
        ph(),
        ('device_type', cstr('wlan')),
        ('compatible', cstr('kugo,wlan-stub')),
        ('local-mac-address', bytes([0x02, 0x4b, 0x55, 0x47, 0x4f, 0x01])),
    ])
    bluetooth = node('bluetooth', [
        ph(),
        ('device_type', cstr('bluetooth')),
        ('compatible', cstr('kugo,bluetooth-stub')),
        ('local-mac-address', bytes([0x02, 0x4b, 0x55, 0x47, 0x4f, 0x02])),
    ])

    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('vince-io')),
        ('compatible', cstr('vince-io') + cstr('arm-io') + b'\0'),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic, gic_alias, wdt, disp0, sdhci_node, wlan, bluetooth])

    # Framebuffer console node, the name the vince kernel looks up.
    fbcon = node('vince-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    cpu0 = node('cpu0', [
        ph(),
        ('device_type', cstr('cpu')),
        ('compatible', cstr('qcom,cortex-a53') + cstr('ARM,v8') + b'\0'),
        ('reg', u32(0)),
        ('cpu-id', u32(0)),
        ('cluster-id', u32(0)),
        ('cluster-type', u32(0)),
        ('die-cluster-id', u32(0)),
        ('cluster-core-id', u32(0)),
        ('state', cstr('running')),
        ('timebase-frequency', u32(TIMEBASE)),
        ('clock-frequency', u32(1401600000)),
        ('bus-frequency', u32(100000000)),
        ('memory-frequency', u32(933000000)),
        ('peripheral-frequency', u32(TIMEBASE)),
        ('fixed-frequency', u32(TIMEBASE)),
        ('cpu-model', cstr('cortex-a53')),
    ])
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], [cpu0])

    product = node('product', [
        ph(),
        ('product-id', u32(0x1003)),
        ('product-name', cstr('iPhone X')),
        ('model-number', cstr('D22AP')),
        ('device-class', cstr('iPhone')),
    ])

    pram = node('pram', [ph(), ('reg', u64(panic_base, PANIC_LOG_SIZE))])

    root = node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('sony,kugo') + cstr('qcom,msm8956') + cstr('AppleARM') + b'\0'),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('kugo')),
        ('target-sub-type', cstr('kugo')),
        ('manufacturer', cstr('Apple Inc.')),
        ('device-id', u64(KUGO_ECID)),
        ('timebase-frequency', u32(TIMEBASE)),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram, product])

    return root


def main():
    parser = argparse.ArgumentParser(description='Generate the kugo Apple Device Tree')
    parser.add_argument('output', help='output .adt file')
    args = parser.parse_args()
    data = adt.serialize(build_kugo_adt())
    with open(args.output, 'wb') as f:
        f.write(data)
    print(f'kugo ADT: {args.output} ({len(data)} bytes)')


if __name__ == '__main__':
    main()
