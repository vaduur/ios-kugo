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
- CPUs: the Cortex-A53 cluster (MPIDR 0..3); cpu0 runs, SweetCPU starts the rest with PSCI CPU_ON
  (--cpus, default 1). The A72 pair (0x100, 0x101) is left out: LK leaves that cluster on XO.
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


def d22ap_product(path):
    """/product properties of the real iPhone X device tree (MobileGestalt reads them)."""
    if not path or not os.path.exists(path):
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if c.prop('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    return None


# Which iPhone's userland runs here (the root image and --product-from must match).
IDENTITIES = {
    # iPhone 8, the default: its 375x667 @2x UI fits the 720x1280 panel; Home = the camera key
    'iphone8': ('D20AP', 'iPhone10,1', 1),
    'iphonex': ('D22AP', 'iPhone10,3', 2),
}


def build_kugo_adt(seed=None, product_from=None, ncpus=1, identity='iphone8'):
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
        # MobileGestalt's screen scale; with the 750x1334 canvas below iOS sees an iPhone 8 panel (375x667 @2x),
        # as ios-sailfish does: far less to render on the CPU, same aspect as the 720x1280 panel.
        ('display-scale', u32(2)),
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
        ('canvas-width', u32(750)),     # SweetFramebuffer.cpp SWEET_FB_LOGICAL_W/H (KUGO)
        ('canvas-height', u32(1334)),
    ])

    # No 'interrupts': vince_mmc.c polls, and without an interrupt-parent IOKit walks up to the root
    # looking for #interrupt-cells and loops on "unexpected IODeviceTreeSupport.cpp:579".
    sdhci_node = node('sdhci', [
        ph(),
        ('compatible', cstr('generic-sdhci') + cstr('qcom,sdhci-msm-v4') + b'\0'),
        ('reg', u64(SDHCI, SDHCI_SIZE)),
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
        ('home-button-type', u32(IDENTITIES[identity][2])),   # MobileGestalt HomeButtonType; 2 = none (iPhone X)
        ('opposed-power-vol-buttons', u32(1)),
    ])

    cpu_nodes = []
    for core in range(ncpus):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,cortex-a53') + cstr('ARM,v8') + b'\0'),
            ('reg', u32(core)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(1401600000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(933000000)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
            ('cpu-model', cstr('cortex-a53')),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    product_props = d22ap_product(product_from)
    if product_props and identity == 'iphone8':
        # As ios-sailfish: the panel is sRGB, and with "P3" iOS picks deep-color snapshot surfaces that
        # CoreAnimation's CPU renderer cannot draw into (white dock / App Library backgrounds).
        product_props = [(k, cstr('sRGB') if k in ('artwork-display-gamut', 'display-gamut') else v)
                         for k, v in product_props]
    if product_props:
        product = node('product', [ph()] + product_props)
    else:
        product = node('product', [
            ph(),
            ('product-id', u32(0x1003)),
            ('product-name', cstr('iPhone X')),
            ('model-number', cstr('D22AP')),
            ('device-class', cstr('iPhone')),
        ])

    pram = node('pram', [ph(), ('reg', u64(panic_base, PANIC_LOG_SIZE))])

    # 'xiaomi,vince' is what the vince kernel's SweetPlatformExpert matches (KernelConfigTables.cpp);
    # without it: panic "Unable to find driver for this platform".
    root = node('device-tree', [
        ph(),
        ('compatible', cstr(IDENTITIES[identity][0]) + cstr(IDENTITIES[identity][1]) + cstr('sony,kugo') +
         cstr('xiaomi,vince') + cstr('qcom,msm8956') + cstr('AppleARM') + b'\0'),
        ('model', cstr(IDENTITIES[identity][1])),
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
    parser.add_argument('--product-from', help="the iPhone's device tree (d20ap.adt / d22ap.adt, from the IPSW) for /product")
    parser.add_argument('--identity', choices=sorted(IDENTITIES), default='iphone8',
                        help='userland identity: iphone8 (D20AP, default) or iphonex (D22AP)')
    parser.add_argument('--cpus', type=int, default=1, choices=range(1, 5), help='A53 cores in the ADT (1-4)')
    args = parser.parse_args()
    data = adt.serialize(build_kugo_adt(product_from=args.product_from, ncpus=args.cpus, identity=args.identity))
    with open(args.output, 'wb') as f:
        f.write(data)
    print(f'kugo ADT: {args.output} ({len(data)} bytes)'
          f'{", /product from " + args.product_from if d22ap_product(args.product_from) else ""}')


if __name__ == '__main__':
    main()
