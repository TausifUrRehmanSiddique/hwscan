# Building the image and a bootable USB

**One stick.** The default build produces a `.img` with a single bootable
FAT32 partition that boots on UEFI and BIOS *and* is writable — so the CSV
lands on the same stick you boot from. No second stick, no repartitioning.

---

## How one partition can do both jobs

A USB stick boots on UEFI by having `EFI/BOOT/BOOTX64.EFI` on a FAT
partition. FAT is writable. So the same partition can be the EFI system
partition, the BIOS boot volume, and the results volume at once:

```
/dev/sdX1   FAT32, label HWSCAN, bootable
  EFI/BOOT/BOOTX64.EFI     <- UEFI firmware boots this
  boot/vmlinuz             <- kernel
  boot/initrd.img          <- everything else, loaded into RAM
  boot/grub/               <- GRUB config + BIOS modules
  HWSCAN/inventory.csv     <- results appended here
```

GRUB's BIOS `core.img` goes in the 1 MiB gap ahead of the partition, so legacy
machines boot from the same image.

The scanner recognises the `HWSCAN` label and writes to it. It still refuses
to write to a *bare* EFI system partition — one with `/EFI` and nothing else,
which is what an internal disk's ESP looks like — because filling one of those
with a CSV is how you make a machine stop booting.

---

## Part 1 — Build the image

### Requirements

A Linux machine (or WSL2) with Docker. Nothing else — the toolchain lives in
the container.

On Windows, install WSL2 first (`wsl --install` in an elevated PowerShell,
then reboot), install Docker Desktop with the WSL2 backend enabled, and run
everything below from inside the WSL2 shell.

### Step 1. Extract and enter

```bash
tar xzf hwscan-cpp.tar.gz
cd hwscan-cpp
chmod +x iso/build-iso.sh
```

### Step 2. Test the scanner first

Sixty seconds here saves a wasted build:

```bash
make test
```

You should see a WARN result for the synthetic ThinkPad and all 42 columns
printed. `exit code: 1` is correct — that's WARN, not an error.

### Step 3. Build

```bash
sudo ./iso/build-iso.sh --docker
```

The first run downloads ~400 MB of Debian packages and takes 10–15 minutes.
Later runs take 2–3.

Result:

```
dist/hwscan-20260823.img          1 GB by default
dist/hwscan-20260823.img.sha256
```

The image is 1 GB regardless of your stick's size. Change it with
`HWSCAN_IMG_MB=2048 sudo ./iso/build-iso.sh --docker` if you want more room —
though 1 GB holds well over a million CSV rows.

Want the old read-only ISO instead (for Ventoy, or to boot several tools from
one stick)? Add `--iso`. You then need a second stick for results.

### What the script does

1. Builds `hwscan` as a **static binary** — no libc, no libstdc++, nothing to
   install alongside it.
2. Assembles an initramfs: busybox (one binary providing every shell utility),
   the scanner, `pci.ids` for device names, and an `init` script.
3. Copies in **only** the kernel modules a diagnostics pass needs — NVMe, AHCI,
   USB storage, HID, MMC, DRM, and the FAT/exFAT/NTFS filesystems.
   **Firmware blobs are deliberately excluded**: identifying a Wi-Fi card needs
   PCI enumeration, not a working driver, so this saves ~250 MB and the adapter
   is still reported correctly.
4. Packs it with `cpio` + `gzip`.
5. Creates the disk image: one MBR partition at 1 MiB, formatted FAT32 with
   label `HWSCAN`, then installs GRUB twice into it -- once for legacy BIOS
   (`core.img` in the 1 MiB gap) and once for UEFI
   (`EFI/BOOT/BOOTX64.EFI`).
6. Verifies that the boot files actually landed. If neither bootloader
   installed, the build **fails loudly** rather than handing you an image
   that looks fine and does not boot.

With `--iso` it calls `grub-mkrescue` instead, producing a read-only hybrid
ISO for Ventoy.

### Native build instead of Docker

On Debian or Ubuntu:

```bash
sudo apt install -y g++ make xorriso grub-pc-bin grub-efi-amd64-bin \
     grub-common grub2-common mtools dosfstools cpio gzip xz-utils zstd \
     kmod pciutils busybox-static linux-image-amd64 fdisk util-linux parted
sudo ./iso/build-iso.sh
```

`xz-utils` and `zstd` are not optional. Kernel modules ship compressed and the
format differs by distro -- Debian uses `.ko.xz`, Ubuntu 22.04+ uses `.ko.zst`
-- and busybox `insmod` can decompress neither. Without the matching
decompressor the modules stay packed, no storage driver loads, and every
machine reports `No storage detected`. The build now warns if either is
missing.

---

## Part 2 — Make the USB bootable

### On Linux

```bash
lsblk -d -o NAME,PATH,SIZE,TRAN                 # find the stick
sudo dd if=dist/hwscan-20260823.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

That is the whole procedure. **Do not repartition it afterwards** — that is
what broke the earlier attempts, and there is now no reason to.

### On Windows

Rufus → SELECT the `.img` → **START** → **"Write in DD Image mode"** → OK.

Unlike the old ISO, Windows can now read this stick: it shows up as a normal
FAT32 drive. After a scan you can plug it into any PC and open
`HWSCAN\inventory.csv` directly.

### Reclaiming the rest of the stick

`dd` writes a 1 GB image, so an 8 GB stick will show as 1 GB. That is cosmetic
— it does not affect scanning. To recover the space:

The image has **two** partitions, so grow the second one (the results volume):

* **Windows:** Disk Management → right-click the **HWSCANDATA** partition → Extend Volume
* **Linux:** `sudo parted /dev/sdX resizepart 2 100%` then
  `sudo fatresize -s max /dev/sdX2`

Because the results partition is last on the disk it grows into free space
without moving anything else. Never touch partition 1 — that is the bootloader.

### Partition layout

| # | Type | Label | Size | Holds | Shown in Windows |
|---|---|---|---|---|---|
| 1 | `ef` EFI System | `HWSCAN` | 512 MB | GRUB, kernel, initramfs | No — Windows hides ESPs |
| 2 | `0c` FAT32 LBA | `HWSCANDATA` | rest | `inventory.csv` | **Yes** |

This is why the stick shows exactly one drive letter in Explorer containing
nothing but results. There is no way to delete the bootloader while copying a
CSV off it, and a single `ef` partition (which made the whole stick vanish from
Explorer) is no longer a trade-off you have to make.

The scanner prefers a `HWSCANDATA` volume over a `HWSCAN` one, so it writes to
partition 2 automatically and leaves partition 1 alone. If partition 2 is ever
unmountable it falls back to partition 1.

Override the split with `HWSCAN_BOOT_MB` (default 512) and `HWSCAN_IMG_MB`
(default 1024). The build refuses to run if that leaves under 256 MB for
results.

The partition is **FAT32**, so `resize2fs` does not apply — that is for
ext2/3/4 and will refuse to touch it.

### Search order

The scanner looks for a volume labelled `HWSCANDATA`, then `HWSCAN` (our own
image), then `Ventoy`, then any other writable removable partition. If you
prefer a separate results stick, format one FAT32 with the label `HWSCANDATA`
and it wins over the boot volume.

### Verify before walking to the bench

A counterfeit or failing stick accepts the write, reports success, and returns
garbage. The only reliable way to catch that is to read the image back off the
flash and compare hashes:

```bash
IMG=dist/hwscan-YYYYMMDD.img
BYTES=$(stat -c %s "$IMG")
sudo dd if=/dev/sdX bs=1M count=$(( (BYTES + 1048575) / 1048576 )) status=none \
  | head -c "$BYTES" | sha256sum
sha256sum "$IMG"
```

The two hashes must match. If they differ the stick is bad — replace it rather
than writing again.

### Or just use Ventoy

Install Ventoy on one stick, copy the ISO onto it, done. Ventoy's exFAT
partition is both the boot medium and the writable results volume, so there's
nothing to partition and no second stick.

---

## Part 3 — Using it

### Firmware settings on each laptop

- **Fast Boot → Disabled.** It skips USB enumeration, so the stick never
  appears in the boot menu. This is the most common reason a good stick
  "doesn't boot".
- **USB Boot → Enabled**
- **SATA Mode → AHCI**, not RAID/Intel RST. In RAID mode Linux may not see the
  drive and the storage columns come back empty.
- **Secure Boot → Disabled.** This image is not signed, and there is no way
  around that short of a shim and a Microsoft-signed chain.

If a machine still refuses to list the stick after all four of the above, its
firmware may insist on MBR partition type `ef` for removable UEFI media.
Rebuild with `HWSCAN_PART_TYPE=ef sudo ./iso/build-iso.sh --docker`. The cost
is that Windows then hides the volume in Explorer, so you would read the CSV
from Linux instead.

### Boot menu keys

| Vendor | Key |
|---|---|
| Dell, Lenovo ThinkPad | F12 |
| HP | F9 (or Esc, then F9) |
| Acer, Toshiba | F12 |
| ASUS | Esc or F8 |
| MSI | F11 |
| Samsung | F10 or Esc |
| Lenovo IdeaPad | Novo pinhole button |

### The loop

1. Boot from stick 1 with stick 2 also plugged in.
2. The scanner runs automatically — no login.
3. Read the verdict: green PASS, amber WARN, red FAIL.
4. Press `p` to power off, swap the machine, repeat.

Other keys: `Enter` rescans, `s` opens a shell, `r` reboots.

Results append to `HWSCANDATA:/HWSCAN/inventory.csv` — one row per machine,
UTF-8 with a BOM so Excel opens it correctly on a double-click.

### Boot menu entries

The GRUB menu offers three:

* **hwscan** — normal
* **verbose** — adds `debug ignore_loglevel`; use this if something fails and
  you need to see what the kernel is doing
* **safe graphics** — adds `nomodeset`. It will boot on stubborn hardware, but
  it disables the DRM driver and with it EDID, so `Display`, `Display Panel
  Model` and `Display Diagonal (in)` come back blank. Last resort only.

---

## If storage comes back empty

Symptom: `Storage Total` is blank and `Detail` says `No storage detected`, on a
machine you know has an SSD.

Cause is almost always a driver that didn't load. Boot the **verbose** GRUB
entry and look for these two lines:

```
Invalid ELF header magic: != ELF
```
The module was compressed (`.ko.xz` / `.ko.zst`) and busybox `insmod` cannot
decompress it. The build script now decompresses every module before packing,
so this should not recur — but if it does, check that `xz-utils` and `zstd`
were installed in the build environment.

```
drm_display_helper: Unknown symbol cec_transmit_attempt_done_ts (err -2)
```
The `cec` module is missing. It lives in `kernel/drivers/media/cec`, which is
now in the copy list. Without it DRM never initialises, so the three `Display`
columns come back blank too.

The init script now prints, on every boot:

```
  block devices: nvme0n1 sda
  modules not loaded (usually built-in): scsi_mod ata_piix
```

The first line is the one that matters. If it lists nothing, no storage driver
loaded. The second line is usually harmless — most of those are compiled into
the kernel rather than built as modules.

---

## If the CSV says "NOT SAVED TO USB"

```
  NOT SAVED TO USB
  Written to /tmp/hwscan/inventory.csv, which is RAM. It will be LOST on reboot.
```

No writable removable volume was found. `/tmp` is a tmpfs — it exists only in
memory and disappears at power-off.

Two causes:

1. **No results stick plugged in.** Insert one and press Enter to rescan.
2. **The stick is faulty.** If the boot log shows
   `sd 0:0:0:0: [sda] Media removed, stopped polling`, that drive is telling
   the kernel it contains no media. It has no partitions, so there is nothing
   to mount. Replace it.

Note that a broken *boot* stick no longer stops the scan from running — the
initramfs is already in RAM by then. But you still need a working *results*
volume, and it can be any second USB stick with a FAT32 partition labelled
`HWSCANDATA`.

To recover a scan that landed in RAM: press `s` for a shell, plug in a good
stick, and copy it off:

```sh
mkdir -p /mnt/usb
mount /dev/sdb1 /mnt/usb
cp /tmp/hwscan/inventory.csv /mnt/usb/
sync
```

---

## Turning the CSV into a spreadsheet

The C++ scanner writes CSV only — an XLSX writer in C++ means a ZIP writer plus
OOXML for little benefit.

`inventory.csv` is UTF-8 with a BOM and CRLF line endings, so Excel and
LibreOffice both open it correctly on a double-click. Save as `.xlsx` from
there if you need a workbook.

To merge several benches, `System ID` is the de-duplication key — a SHA-1 over
manufacturer, model, system serial and board serial, so the same machine
scanned twice produces the same ID. The Python version of this tool used an
identical construction, so rows from both merge cleanly.

> **Note:** the `tools/csv_to_xlsx.py` and `tools/merge_csv.py` helpers
> referenced by earlier revisions of this document are not part of this
> tarball. Only `tools/make_fixture.py` ships here.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Stick not in the boot menu | Disable Fast Boot; try a rear USB 2.0 port |
| "Secure Boot violation" | Disable Secure Boot — this image is unsigned |
| Boots, but no storage listed | SATA is in RAID mode; switch to AHCI |
| `Display*` columns blank | You booted the `nomodeset` entry, or the GPU has no in-tree driver |
| `Storage Health` says UNKNOWN | Drive didn't answer SMART. Normal for eMMC and some USB-bridged devices |
| Wi-Fi shows a hex ID | `pci.ids` wasn't found at build time; check the build log for that warning |
| Results land in `/tmp` | No writable removable volume found — check stick 2's label is exactly `HWSCANDATA` |
| `grub-mkrescue` fails | Missing `mtools` or `grub-efi-amd64-bin`; the `--docker` path installs both |
| `missing tool: grub-install` | Install **`grub2-common`**. `grub-common` does not provide it, and neither `grub-pc-bin` nor `grub-efi-amd64-bin` depends on it |

---

## A caveat worth stating

The **scanner** is tested: builds clean with `-Wall -Wextra`, produces all 42
columns against a synthetic machine, and degrades safely on hardware that
answers nothing. The FAT label offsets it reads (`0x47` for FAT32, `0x2B` for
FAT16) are verified against the spec.

The **image builder cannot be fully exercised** in the environment it was
written in — `cpio`, `depmod`, `sfdisk`, `mkfs.vfat`, `grub-install` and
`docker` were all absent there. What *is* checked: shell syntax under
`bash -n`, the `init` script under `dash -n` (the right proxy for busybox
`ash`), and the flag dispatch across every documented invocation. The
`build_img` path still runs for the first time on your machine.

It does now verify its own output before declaring success. `BOOTX64.EFI`,
`boot/grub/i386-pc`, `vmlinuz`, `initrd.img` and `grub.cfg` must all be present
on the finished filesystem, and if *neither* bootloader installed, the build
fails rather than handing you an image that looks fine and does not boot. Both
GRUB runs are logged to `build/grub-bios.log` and `build/grub-uefi.log`.

| Failure | Meaning |
|---|---|
| `losetup failed (need --privileged)` | Docker wasn't given `--privileged`; the `--docker` path does pass it |
| `no partition device /dev/loopXp1` | Two causes stack here. The loop driver is usually loaded with `max_part=0`, so `losetup -P` creates no partitions at all; and inside a container `/dev` is a tmpfs built from a snapshot of the host at start-up, so a node the kernel creates later never appears. The script now tries `partx`/`partprobe`, then `mknod` from sysfs, then an offset loop. The `--docker` path also bind-mounts `/dev`, which prevents the problem outright |
| `falling back to an offset loop -- UEFI-only` | Every recovery step failed. The image still boots on UEFI. For BIOS too, build natively rather than in Docker |
| `neither UEFI nor BIOS boot files were installed` | Both GRUB runs failed. Read `build/grub-bios.log` and `build/grub-uefi.log`. This is the case that used to pass silently |
| `BIOS GRUB install failed` | Warning only — the image still boots on UEFI, which covers essentially every machine you are scanning |
| `missing from image: boot/vmlinuz` | The kernel copy failed; check `/boot` in the build environment |
| `cannot mount /dev/loopXp1` | `mkfs.vfat` didn't run; check `dosfstools` is installed |
