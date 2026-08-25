# hwscan (C++)

Hardware inventory scanner as a single **static binary with no dependencies**.
Replaces the Python version's `python3 + openpyxl + dmidecode + smartmontools
+ pciutils` stack — roughly **63 MB of packages** — with **one 1 MB file**.

Boot a laptop, wait a few seconds, get one CSV row appended to a USB stick.

## What changed from the Python version

| | Python | C++ |
|---|---|---|
| Runtime deps | python3, openpyxl, dmidecode, smartmontools, pciutils | **none** |
| On-image size | ~63 MB | **~1 MB** (+ optional 1.5 MB `pci.ids`) |
| Columns | 45 | **42** |
| XLSX output | built in | use `tools/csv_to_xlsx.py` on your workstation |

**Dropped by request:** `TPM`, `TPM Version`, `SMART Health`. Everything else
is identical, including column order, so old and new CSVs merge cleanly.

`Storage Health` and `Storage Health %` are present and come from SMART read
directly via ioctl — see `src/smart.cpp`. No `smartctl` binary is involved.

Under `--sysroot`, SMART is skipped entirely rather than queried: `/dev` is
real even when `/sys` is a replay tree, and querying it would attribute the
workstation's own drives to the captured machine.

`System ID` uses the same SHA-1 construction as the Python version — verified
against `hashlib` on three test vectors — so the two tools can write into the
same inventory file and de-duplicate against each other.

## Build

```bash
make           # development build
make static    # dependency-free binary for the boot image
make test      # run against a synthetic laptop, no hardware needed
```

Needs only `g++` with C++17. No libraries, no `-dev` packages.

## Run

```bash
sudo ./hwscan                      # find a USB stick, scan, append a row
sudo ./hwscan --output-dir ./out   # write here instead
./hwscan --sysroot /tmp/fixture    # replay a captured machine (testing)
./hwscan --stdout                  # print the CSV row, write nothing
```

Exit codes: `0` PASS, `1` WARN, `2` FAIL, `3` scan/IO error.

## How it gets the data without helper tools

**SMBIOS** (`src/smbios.cpp`) parses `/sys/firmware/dmi/tables/DMI` directly —
the same table `dmidecode` reads. This is what gives us RAM vendor, serial,
part number, speed and form factor per DIMM, plus chassis type and every
identity field. Soldered memory is flagged via form factor `Row Of Chips`,
which matters when grading upgradeability.

**PCI** (`src/collect.cpp`) enumerates `/sys/bus/pci/devices/*` and resolves
names from `pci.ids`. Ship `pci.ids` at `/opt/hwscan/pci.ids` for full product
names; without it, devices still appear as `vendor:device` hex.

**EDID** is parsed from `/sys/class/drm/*/edid` for panel vendor, model and
physical diagonal. Requires a loaded DRM driver — never boot with `nomodeset`.

**Disk serials** come from `/dev/disk/by-id/` symlinks, so no helper binary.

**SMART** (`src/smart.cpp`) talks to drives directly. NVMe uses
`ioctl(NVME_IOCTL_ADMIN_CMD)` with Get Log Page 0x02, where the spec mandates a
Percentage Used counter — that figure is exact. SATA uses `ioctl(SG_IO)` with
ATA PASS-THROUGH(16) to issue SMART READ DATA and SMART READ THRESHOLDS; a
drive fails when any pre-fail attribute drops to its threshold. SSD wear is
read from attributes 231/233/177/202/173/169 in that order, because vendors
disagree about which one carries it. Spinning disks have no wear counter at
all, so their percentage is a triage heuristic from reallocated, pending and
uncorrectable sector counts plus power-on hours — the raw counts go into
`Detail` so the evidence stays visible.

**Storage safety:** internal disks are never mounted or written to, and the
boot USB is excluded from `Storage Total`.

## Output

`HWSCAN/inventory.csv` on the stick — UTF-8 with BOM so Excel opens it
correctly on a double-click, CRLF line endings, appended under `flock` with
`fsync`, so a stick pulled mid-write cannot corrupt earlier rows.

Search order for the results volume: a partition labelled `HWSCANDATA`, then
`Ventoy`, then any other writable removable partition. Partitions under 128 MB
and anything with `/EFI` at its root are skipped, so the EFI system partition
is never used. Falls back to `/tmp/hwscan` if nothing is found.

FAT labels are read straight from the boot sector, so `blkid` isn't needed
either.

## Layout

```
src/util.h|cpp      file IO, string cleaning, SHA-1, size formatting
src/smbios.h|cpp    SMBIOS/DMI parser (replaces dmidecode)
src/collect.h|cpp   PCI, EDID, CPU, memory, storage, GPU, display, battery, net
src/report.h|cpp    40-column schema, grading, CSV writer, USB discovery
src/main.cpp        CLI and on-screen summary
tools/make_fixture.py  synthetic machine for testing without hardware
```

## Testing without hardware

```bash
python3 tools/make_fixture.py /tmp/fixture
./hwscan --sysroot /tmp/fixture --output-dir /tmp/out
```

The fixture models a ThinkPad T480: real SMBIOS binary table with two DDR4
DIMMs, a PCI tree, a valid EDID blob with correct checksum for a 14" BOE
panel, an NVMe disk, and a battery at 68% of design capacity.

## Known limitations

* `VRAM` is exact for AMD (`mem_info_vram_total`) and reads `Shared` for
  integrated. Discrete NVIDIA reports blank without the proprietary driver.
* `Battery Health %` is the firmware's own design-capacity figure, not a load
  test. A pack can report 85% and still collapse under draw.
* Without `pci.ids`, network and GPU names fall back to hex IDs.
