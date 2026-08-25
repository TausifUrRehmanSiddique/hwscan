# Review fixes

Everything changed during the review, with the reasoning. Nothing here alters
the CSV schema, the column order, or the `System ID` construction, so rows
written by the patched build still merge with rows written by the old one.

---

## Critical

### 1. SATA SMART could never have worked — `src/smart.cpp`

`ataSmart()` set `cdb[2] = 0x2E`, which turns on **CK_COND**. A SCSI-to-ATA
translation layer asked for the ATA output registers answers with **CHECK
CONDITION even on success**, carrying descriptor `09h` ("ATA Status Return") in
the sense data. The very next line was:

```c
if (io.host_status != 0 || io.driver_status != 0) return false;
```

The kernel sets `driver_status = DRIVER_SENSE (0x08)` whenever sense data comes
back, so this discarded the successful transfer on *every healthy SATA drive*.
Every SATA SSD and every spinning disk would have reported
`Storage Health: UNKNOWN`, and grading would have added a spurious
"Drive health unknown" WARN to each of those machines.

Fixed by clearing CK_COND (`cdb[2] = 0x0E`) and replacing the blanket status
test with real sense-data inspection: fixed and descriptor sense formats are
both decoded, sense keys `NO SENSE (0)` and `RECOVERED ERROR (1)` are accepted
as success, anything higher is a genuine failure. A short-transfer check was
added, the sense buffer was widened from 32 to 64 bytes (descriptor-format
sense with an ATA return descriptor does not fit in 32), and the timeout was
raised from 10 s to 15 s for drives that are slow to spin up.

**Only the NVMe path worked before this. Any inventory taken with the previous
build has blank storage health for every non-NVMe machine.**

---

## Serious

### 2. `--sysroot` reached out to real hardware — `src/collect.cpp`

```c
smart::Info si = smart::query("/dev/" + name, isNvme, rot == 1);
```

Literal `/dev/`, with no `util::P()` wrapper. `--sysroot` is documented as
"replay a captured machine", but `/dev` stays real even when `/sys` is a replay
tree — so replaying a fixture on a workstation issued `SG_IO` and NVMe admin
ioctls to *that workstation's* drives and stapled the answers onto someone
else's inventory row. Silent, and wrong in a way that is very hard to notice.

SMART is now skipped when `util::ROOT` is non-empty, with
`"<dev>: SMART skipped (--sysroot replay)"` recorded in `Detail` so the gap is
visible rather than implied. The duplicate `"SMART not available"` note is
suppressed in that mode.

### 3. Partitionless USB sticks were invisible — `src/report.cpp`

`candidates()` only considered entries under `/sys/block/$disk/` that had a
`partition` file. A stick formatted straight to FAT32 with no partition table —
which is what Windows "Format…" and most factory-fresh sticks produce — has no
partition children at all, so it was skipped entirely. The operator would plug
in a perfectly good results stick, see `NOT SAVED TO USB`, and lose the scan to
tmpfs on power-off.

Whole-disk devices are now added as a fallback when a removable disk exposes no
partitions. The label/size ranking was factored into `rankOfLabel()` and is
applied identically to both paths.

### 4. The builder could report success on an unbootable image — `iso/build-iso.sh`

Both `grub-install` calls ended in `|| warn`, so a build in which *neither*
bootloader installed still printed `done` and produced a `.img`. Worse,
`grub-install --target=i386-pc` against a loop device commonly fails with
*"cannot find a GRUB drive for /dev/loopXp1. Check your device.map"* — meaning
BIOS boot was probably broken on every image the script had ever produced, and
the only symptom was a warning line scrolling past.

Three changes:

* An explicit `device.map` mapping the loop device to `(hd0)` is written and
  passed to `grub-install`, which is what makes the legacy path work at all on
  a loop device.
* Both runs log to `build/grub-bios.log` / `build/grub-uefi.log` instead of
  `/dev/null`.
* The finished filesystem is inspected before unmounting.
  `EFI/BOOT/BOOTX64.EFI`, `boot/grub/i386-pc`, `boot/vmlinuz`,
  `boot/initrd.img` and `boot/grub/grub.cfg` must all be present. Missing
  kernel or config is fatal; losing *one* bootloader is a loud warning naming
  which firmware type is now unsupported; losing *both* is fatal.

---

## Moderate

### 5. `unzstd` was not checked for — `iso/build-iso.sh`

The precheck required `unxz` but not `unzstd`. Debian ships `.ko.xz`, but
**Ubuntu 22.04+ ships `.ko.zst`**. Building natively on Ubuntu without `zstd`
installed left every module compressed; busybox `insmod` cannot decompress and
fails with `Invalid ELF header magic`, which surfaces at the bench as
`No storage detected` on every machine — with nothing near the actual cause.
Both decompressors are now checked and warn by name. (The `--docker` path
already installed both.)

### 6. Init spun at 100% CPU on console EOF — `iso/build-iso.sh`

`read -r key` with no EOF guard. On a serial console or a detached tty, `read`
returns immediately and forever, turning the menu into a busy rescan loop that
would hammer the drives of the machine under test. Now falls through to an idle
sleep with an explanatory line.

### 7. Phantom PCI device `0000:0000` — `src/collect.cpp`

`hexField()` zero-padded an empty string up to the requested width, so a
missing `vendor` attribute became `"0000"`. The guard immediately below it,
`if (!d.vendorId.empty())`, could therefore never reject anything. It now
returns empty for empty input, so the guard does its job. It also stopped using
`clean()` — that function blanks strings made only of zeros, which is exactly
what a legitimate PCI ID like `0x0000` looks like — and now accepts an
uppercase `0X` prefix.

### 8. An all-zero SMART page graded as PASS — `src/smart.cpp`

`parseAttrs()` returned `void`. A drive that answered the ioctl but returned a
zeroed page produced no attributes, tripped no thresholds, and was therefore
reported `SMART PASS` on the strength of having said nothing at all. It now
returns a count of populated attributes and `query()` bails out when that count
is zero, recording `"SMART page returned no attributes"` instead of a false
clean bill of health.

The dead `id > 255` test was also removed — `id` is read from an
`unsigned char`, so it cannot exceed 255.

### 9. `umount()` failure was ignored — `src/report.cpp`

`findOutputDir()` unmounted and moved to the next candidate without checking
the result. `umount()` returns `EBUSY` readily, and the next candidate's
`mount()` would then stack on top of the still-mounted old one — so the CSV
would be written into a filesystem that nobody would ever look at again. Both
call sites now use `forceUnmount()`, which falls back to `MNT_DETACH`.

### 10. Unsigned underflow in the result banner — `src/main.cpp`

```c
(label + std::string(64 - pad - label.size(), ' '))
```

`label.size()` is `std::string::size_type`, so the whole expression was
evaluated unsigned. A status string long enough to make `label` exceed 64
characters wrapped to a value near `SIZE_MAX`, and `std::string(huge, ' ')`
would then **abort** — the static build is compiled `-fno-exceptions`, so there
is nothing to catch `bad_alloc`. Not reachable with the current three status
values, but it is one added status away from crashing after the scan has
already run. All three terms are now `int`, both results are clamped, and
`label` is truncated.

### 11. Missing includes — `src/util.cpp`, `src/collect.cpp`

`util.cpp` used `errno` and `strtoll` without `<cerrno>` or `<cstdlib>`;
`collect.cpp` used `atoll` without `<cstdlib>`. These compile on glibc through
transitive includes, which is luck rather than correctness — a musl or
different-libstdc++ toolchain would fail. Added.

### 12. Misleading operator instruction — `src/main.cpp`

The `NOT SAVED TO USB` message ended `"then press Enter to rescan"`, but
`hwscan` returns immediately after printing it. True in the boot image, because
`init` happens to prompt afterwards; false for anyone running the binary
directly. Reworded to describe the outcome rather than a keypress the program
does not wait for.

---

## Documentation

* `src/collect.h` opened with "No TPM, no SMART (dropped by request)" while the
  file's whole storage path depends on SMART. Corrected.
* `src/report.h` said "The 40 columns"; there are 42, and it claimed the SMART
  columns were absent when they are present and populated. Corrected.
* `BUILD-ISO.md` described the build as calling `grub-mkrescue`, which is the
  `--iso` path, not the default `.img` path. Rewritten to describe what the
  default actually does, including the new verification step.
* `BUILD-ISO.md` referenced `tools/csv_to_xlsx.py`, `tools/merge_csv.py`,
  `tools/prepare-usb.sh`, `Verify-UsbImage.ps1` and `docs/BUILD-WINDOWS.md` —
  **none of which exist in this tarball**. Replaced with instructions that use
  only what ships here.
* `BUILD-ISO.md` suggested `resize2fs` for reclaiming space on the stick. That
  partition is FAT32; `resize2fs` is an ext2/3/4 tool and will refuse. Removed;
  `fatresize` was already named two lines later.
* The native-build `apt install` line omitted `xz-utils`, `zstd`, `fdisk`,
  `util-linux` and `parted`, all of which the script requires. Added, with the
  reason the decompressors matter.
* Added the `HWSCAN_PART_TYPE=ef` escape hatch for firmware that insists on MBR
  type `ef` for removable UEFI media, and documented the Windows-visibility
  trade-off.

---

## Enhancement

`HWSCAN_PART_TYPE` (default `0c`) now controls the MBR partition type.
`0c` (FAT32 LBA) keeps the stick visible in Windows Explorer so the CSV can be
read by plugging it in. A minority of strict UEFI implementations only boot
type `ef` (EFI System) from MBR media. Previously this was hard-coded with no
way out short of editing the script.

`HWSCAN_IMG_MB` and `HWSCAN_PART_TYPE` are both forwarded into the Docker
container; previously `HWSCAN_IMG_MB` was documented as working with
`--docker` but was **not** passed through, so it silently had no effect.

---

## Not changed, but worth knowing

* **Secure Boot must be disabled.** `grub-install --removable` produces an
  unsigned `BOOTX64.EFI`. Signing needs shim and a Microsoft-signed chain.
* **`Battery Health %` is the firmware's own design-capacity figure**, not a
  load test. A pack can report 85% and still collapse under draw.
* **Discrete NVIDIA VRAM comes back blank** without the proprietary driver.
  Only `amdgpu` exports a usable figure via `mem_info_vram_total`.
* **The RTC supplies `Scan Date` / `Scan Time`.** There is no NTP in the
  initramfs and no `/etc/localtime`, so timestamps are whatever the machine's
  clock says, interpreted as UTC. A dead CMOS battery yields a wrong date, and
  that is a firmware fault the scanner cannot correct.
* **Spinning-disk `Storage Health %` is a triage heuristic**, not a
  remaining-life figure — ATA defines no wear counter for them. The raw
  reallocated / pending / uncorrectable counts go into `Detail` so the evidence
  behind the number stays visible.
