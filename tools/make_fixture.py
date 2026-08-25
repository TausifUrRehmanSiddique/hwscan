#!/usr/bin/env python3
"""
make_fixture.py -- build a fake /sys + /proc tree for testing hwscan.

Lets you exercise every collector on your workstation, with no laptop and no
root, before you burn an ISO:

    python3 tools/make_fixture.py /tmp/fixture
    python3 scanner/hwscan.py --sysroot /tmp/fixture --output-dir /tmp/out

The tree models a Lenovo ThinkPad T480: UEFI with Secure Boot on, 16 GB of
DDR4 in two slots, an NVMe SSD, a worn battery, TPM 2.0, and a real EDID
blob (checksum and all) for a 14" 1920x1080 BOE panel.
"""

from __future__ import annotations

import os
import struct
import sys


def w(root, path, content, mode=0o644):
    full = os.path.join(root, path.lstrip("/"))
    os.makedirs(os.path.dirname(full), exist_ok=True)
    data = content if isinstance(content, bytes) else str(content).encode()
    with open(full, "wb") as fh:
        fh.write(data)
    os.chmod(full, mode)


def build_edid(pnp="BOE", product=0x08C1, width=1920, height=1080,
               w_mm=309, h_mm=174):
    """Assemble a valid 128-byte EDID 1.4 block with a correct checksum."""
    b = bytearray(128)
    b[0:8] = b"\x00\xff\xff\xff\xff\xff\xff\x00"

    code = 0
    for ch in pnp:
        code = (code << 5) | (ord(ch) - ord("A") + 1)
    b[8] = (code >> 8) & 0xFF
    b[9] = code & 0xFF

    b[10] = product & 0xFF
    b[11] = (product >> 8) & 0xFF
    b[12:16] = (0).to_bytes(4, "little")
    b[16] = 20            # week 20
    b[17] = 2018 - 1990   # year 2018
    b[18], b[19] = 1, 4   # EDID 1.4
    b[20] = 0xA5          # digital input
    b[21] = round(w_mm / 10)
    b[22] = round(h_mm / 10)
    b[23] = 0x78
    b[24] = 0x06

    # Detailed timing descriptor at offset 54
    d = bytearray(18)
    pixclock = 13850
    d[0] = pixclock & 0xFF
    d[1] = (pixclock >> 8) & 0xFF
    d[2] = width & 0xFF
    d[3] = 0x30
    d[4] = ((width >> 8) & 0x0F) << 4
    d[5] = height & 0xFF
    d[6] = 0x10
    d[7] = ((height >> 8) & 0x0F) << 4
    d[12] = w_mm & 0xFF
    d[13] = h_mm & 0xFF
    d[14] = (((w_mm >> 8) & 0x0F) << 4) | ((h_mm >> 8) & 0x0F)
    b[54:72] = d

    # Descriptor 2: monitor name
    name = bytearray(18)
    name[3] = 0xFC
    text = b"NV140FHM-N49\n"
    name[5:5 + len(text)] = text
    b[72:90] = name

    b[127] = (256 - (sum(b[:127]) % 256)) % 256
    return bytes(b)


def build(root):
    # ---- DMI -------------------------------------------------------------
    dmi = {
        "sys_vendor": "LENOVO",
        "product_name": "20L5S1P600",
        "product_version": "ThinkPad T480",
        "product_family": "ThinkPad T480",
        "product_serial": "PF1KQ8ZT",
        "chassis_type": "10",
        "chassis_asset_tag": "ASSET-00417",
        "board_vendor": "LENOVO",
        "board_name": "20L5S1P600",
        "board_version": "SDK0J40697 WIN",
        "board_serial": "L1HF04M00ZW",
        "bios_vendor": "LENOVO",
        "bios_version": "N24ET66W (1.41 )",
        "bios_date": "06/14/2022",
    }
    for k, v in dmi.items():
        w(root, "/sys/class/dmi/id/" + k, v + "\n")

    # ---- Firmware / Secure Boot -----------------------------------------
    os.makedirs(os.path.join(root, "sys/firmware/efi/efivars"), exist_ok=True)
    w(root, "/sys/firmware/efi/fw_platform_size", "64\n")
    w(root, "/sys/firmware/efi/efivars/"
            "SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c",
      bytes([0x06, 0x00, 0x00, 0x00, 0x01]))
    w(root, "/sys/firmware/acpi/tables/TPM2", b"TPM2fixture")

    # ---- CPU -------------------------------------------------------------
    cpuinfo = []
    for cpu in range(8):
        cpuinfo.append(
            "processor\t: %d\n"
            "vendor_id\t: GenuineIntel\n"
            "model name\t: Intel(R) Core(TM) i5-8350U CPU @ 1.70GHz\n"
            "physical id\t: 0\n"
            "core id\t\t: %d\n"
            "cpu MHz\t\t: 1900.000\n" % (cpu, cpu // 2)
        )
    w(root, "/proc/cpuinfo", "\n".join(cpuinfo) + "\n")
    w(root, "/proc/meminfo", "MemTotal:       16187352 kB\nMemFree: 9000000 kB\n")

    # ---- Storage: one 512 GB NVMe ---------------------------------------
    w(root, "/sys/block/nvme0n1/size", str(512110190592 // 512) + "\n")
    w(root, "/sys/block/nvme0n1/removable", "0\n")
    w(root, "/sys/block/nvme0n1/queue/rotational", "0\n")
    w(root, "/sys/block/nvme0n1/device/model", "SAMSUNG MZVLB512HAJQ-000L7\n")
    w(root, "/sys/block/nvme0n1/device/serial", "S3TNNX0M123456\n")
    w(root, "/sys/class/nvme/nvme0/model", "SAMSUNG MZVLB512HAJQ-000L7\n")
    w(root, "/sys/class/nvme/nvme0/serial", "S3TNNX0M123456\n")

    # ---- Display ---------------------------------------------------------
    w(root, "/sys/class/drm/card0-eDP-1/status", "connected\n")
    w(root, "/sys/class/drm/card0-eDP-1/enabled", "enabled\n")
    w(root, "/sys/class/drm/card0-eDP-1/modes", "1920x1080\n1600x900\n1280x720\n")
    w(root, "/sys/class/drm/card0-eDP-1/edid", build_edid())
    w(root, "/sys/class/drm/card0-HDMI-A-1/status", "disconnected\n")
    w(root, "/sys/class/drm/card0-HDMI-A-1/modes", "")
    w(root, "/sys/class/drm/card0-HDMI-A-1/edid", b"")

    # ---- Battery: 68 % of design capacity -------------------------------
    w(root, "/sys/class/power_supply/BAT0/type", "Battery\n")
    w(root, "/sys/class/power_supply/BAT0/energy_full", "16210000\n")
    w(root, "/sys/class/power_supply/BAT0/energy_full_design", "24000000\n")
    w(root, "/sys/class/power_supply/BAT0/cycle_count", "412\n")
    w(root, "/sys/class/power_supply/AC/type", "Mains\n")

    # ---- TPM -------------------------------------------------------------
    w(root, "/sys/class/tpm/tpm0/tpm_version_major", "2\n")

    # ---- Network ---------------------------------------------------------
    w(root, "/sys/class/net/lo/type", "772\n")
    w(root, "/sys/class/net/enp0s31f6/type", "1\n")
    w(root, "/sys/class/net/enp0s31f6/address", "8c:16:45:aa:bb:cc\n")
    w(root, "/sys/class/net/wlp3s0/type", "1\n")
    w(root, "/sys/class/net/wlp3s0/address", "34:13:e8:11:22:33\n")
    os.makedirs(os.path.join(root, "sys/class/net/wlp3s0/phy80211"), exist_ok=True)
    w(root, "/sys/class/bluetooth/hci0/name", "hci0\n")

    print("fixture written to %s" % root)
    print("now run:")
    print("  python3 scanner/hwscan.py --sysroot %s --output-dir /tmp/hwscan-out" % root)




def build_dmi():
    """Raw SMBIOS table -- what the C++ scanner parses instead of dmidecode."""
    def st(*s):
        b = b"".join(x.encode() + b"\0" for x in s)
        return (b if b else b"\0") + b"\0"
    out = b""
    t1 = bytearray(0x1B); t1[0]=1; t1[1]=0x1B; t1[2:4]=struct.pack('<H',1)
    t1[4]=1; t1[5]=2; t1[6]=3; t1[7]=4
    out += bytes(t1) + st("LENOVO","20L5S1P600","ThinkPad T480","PF1KQ8ZT")
    t2 = bytearray(0x0F); t2[0]=2; t2[1]=0x0F; t2[2:4]=struct.pack('<H',2)
    t2[4]=1; t2[5]=2; t2[6]=3; t2[7]=4
    out += bytes(t2) + st("LENOVO","20L5S1P600","SDK0J40697 WIN","L1HF04M00ZW")
    t3 = bytearray(0x15); t3[0]=3; t3[1]=0x15; t3[2:4]=struct.pack('<H',3)
    t3[4]=1; t3[5]=10; t3[7]=2; t3[8]=3
    out += bytes(t3) + st("LENOVO","CHASSIS-SN","ASSET-00417")
    t16 = bytearray(0x17); t16[0]=16; t16[1]=0x17; t16[2:4]=struct.pack('<H',0x1000)
    t16[0x0D:0x0F]=struct.pack('<H',2)
    out += bytes(t16) + st()
    for i,(mb,man,ser,part) in enumerate([
            (8192,"Samsung","3B4C1D22","M471A1K43CB1-CTD"),
            (8192,"SK Hynix","7A2E9F01","HMA81GS6JJR8N-VK")]):
        t = bytearray(0x22); t[0]=17; t[1]=0x22; t[2:4]=struct.pack('<H',0x10+i)
        t[0x0C:0x0E]=struct.pack('<H',mb); t[0x0E]=0x0D; t[0x10]=1; t[0x12]=0x1A
        t[0x15:0x17]=struct.pack('<H',2400); t[0x17]=2; t[0x18]=3; t[0x1A]=4
        t[0x20:0x22]=struct.pack('<H',2400)
        out += bytes(t) + st(f"ChannelA-DIMM{i}",man,ser,part)
    t127 = bytearray(4); t127[0]=127; t127[1]=4
    return out + bytes(t127) + b"\0\0"


def build_extra(root):
    w(root, "/sys/firmware/dmi/tables/DMI", build_dmi())

    # PCI devices the C++ scanner enumerates directly from sysfs
    def pci(slot, cls, ven, dev):
        base = "/sys/bus/pci/devices/" + slot
        w(root, base + "/class",  cls + "\n")
        w(root, base + "/vendor", ven + "\n")
        w(root, base + "/device", dev + "\n")
    pci("0000:00:02.0", "0x030000", "0x8086", "0x5917")   # UHD Graphics 620
    pci("0000:02:00.0", "0x028000", "0x8086", "0x24fd")   # Wireless-AC 8265
    pci("0000:00:1f.6", "0x020000", "0x8086", "0x15d7")   # Ethernet I219-V

    # minimal pci.ids so names resolve without the full 1.5 MB file
    ids = ("8086  Intel Corporation\n"
           "\t5917  UHD Graphics 620\n"
           "\t24fd  Wireless 8265 / 8275\n"
           "\t15d7  Ethernet Connection (4) I219-V\n")
    w(root, "/usr/share/misc/pci.ids", ids)

    # The base fixture creates netdevs with no PCI link; drop the stale wifi one
    # and wire ethernet up so names resolve the way they do on real hardware.
    import shutil
    shutil.rmtree(os.path.join(root, "sys/class/net/wlp3s0"), ignore_errors=True)
    try:
        os.symlink("../../../bus/pci/devices/0000:00:1f.6",
                   os.path.join(root, "sys/class/net/enp0s31f6/device"))
    except (FileExistsError, FileNotFoundError):
        pass

    # link the wifi netdev to its PCI slot
    os.makedirs(os.path.join(root, "sys/class/net/wlp2s0"), exist_ok=True)
    os.makedirs(os.path.join(root, "sys/class/net/wlp2s0/phy80211"), exist_ok=True)
    w(root, "/sys/class/net/wlp2s0/type", "1\n")
    w(root, "/sys/class/net/wlp2s0/address", "34:13:e8:11:22:33\n")
    try:
        os.symlink("../../../bus/pci/devices/0000:02:00.0",
                   os.path.join(root, "sys/class/net/wlp2s0/device"))
    except FileExistsError:
        pass

if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hwscan-fixture"
    os.makedirs(target, exist_ok=True)
    build(target)
    build_extra(target)
