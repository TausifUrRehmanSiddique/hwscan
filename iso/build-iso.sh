#!/bin/bash
# build-iso.sh -- build a minimal bootable ISO around the hwscan binary.
#
#   sudo ./iso/build-iso.sh --docker         writable .img  (RECOMMENDED)
#   sudo ./iso/build-iso.sh --docker --iso    read-only .iso (for Ventoy)
#   sudo ./iso/build-iso.sh --native          build without a container
#
# The default output is a .img: ONE bootable FAT32 partition that boots on both
# UEFI and BIOS *and* is writable, so the CSV lands on the same stick you boot
# from. No second stick, no repartitioning.
#
# Output: dist/hwscan-<date>.iso  -- roughly 40-60 MB, boots UEFI and BIOS.
#
# WHY THIS IS DIFFERENT FROM A LIVE-BUILD IMAGE
#
# There is no squashfs and no root filesystem. Everything lives in an initramfs
# that the bootloader loads into RAM before Linux starts. That removes the
# entire "Unable to find a medium containing a live file system" failure mode:
# the USB is read once, by firmware, using the same code path that already
# loads the kernel. Once booted, the stick is only needed to write results.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "${HERE}/.." && pwd)"
WORK="${PROJECT}/build"
DIST="${PROJECT}/dist"
# Bump this whenever the script changes. It is printed on every run, so a CI
# log always states which revision produced it -- otherwise an old copy sitting
# in a repo looks exactly like a fix that did not work.
SCRIPT_REV="2026-08-25.18"

# CI overrides this so an image can be traced back to the commit that made it.
NAME="${HWSCAN_NAME:-hwscan-$(date +%Y%m%d)}"

log()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[!] %s\033[0m\n' "$*"; }
die()  { printf '\033[31m[x] %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# --iso selects the read-only hybrid image instead of the writable disk image.
MAKE_IMG=1
_args=()
for _a in "$@"; do
    case "$_a" in
        --iso) MAKE_IMG=0 ;;
        --img) MAKE_IMG=1 ;;
        *)     _args+=("$_a") ;;
    esac
done
set -- ${_args[@]+"${_args[@]}"}
export HWSCAN_MAKE_IMG="${MAKE_IMG}"

if [ "${1:-}" = "--docker" ]; then
    command -v docker >/dev/null || die "docker is not installed"
    log "building inside debian:bookworm"
    # --privileged alone is not enough. Docker builds the container's /dev from
    # a snapshot of the host's devices taken at start-up, so a partition node
    # the kernel creates later (/dev/loopXp1) never appears inside. Bind-mounting
    # the host's /dev makes it live, which is what lets losetup -P work at all.
    docker run --rm --privileged -v /dev:/dev -v "${PROJECT}:/work" -w /work \
        -e HWSCAN_MAKE_IMG="${MAKE_IMG}" -e HWSCAN_IMG_MB="${HWSCAN_IMG_MB:-}" \
        -e HWSCAN_PART_TYPE="${HWSCAN_PART_TYPE:-}" -e HWSCAN_BOOT_MB="${HWSCAN_BOOT_MB:-}" -e HWSCAN_NAME="${HWSCAN_NAME:-}" \
        -e HWSCAN_COMMIT="${HWSCAN_COMMIT:-}" debian:bookworm \
        bash -c '
            set -e
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            # grub2-common is the package that ships /usr/sbin/grub-install.
            # grub-common does NOT provide it, and neither grub-pc-bin nor
            # grub-efi-amd64-bin depends on it -- they depend on grub-common
            # only. With --no-install-recommends it is therefore never pulled
            # in, and the build dies at the tool check.
            apt-get install -y --no-install-recommends \
                g++ make xorriso grub-pc-bin grub-efi-amd64-bin \
                grub-common grub2-common \
                mtools dosfstools cpio gzip xz-utils zstd kmod pciutils \
                busybox-static linux-image-amd64 ca-certificates \
                fdisk util-linux parted
            command -v grub-install >/dev/null || {
                echo "[x] apt finished but grub-install is still absent." >&2
                echo "    Expected it from grub2-common. Check the apt log above." >&2
                exit 1
            }
            ./iso/build-iso.sh --native $( [ "$HWSCAN_MAKE_IMG" = "0" ] && echo --iso )
        '
    exit $?
fi

[ "$(id -u)" -eq 0 ] || die "run as root (or use --docker)"

# Say where we are. The same tool-check failure reads very differently
# depending on whether it happened on your machine or inside the container,
# and the message used to be identical in both cases.
# Self-check: a stray line inside a heredoc body silently becomes data rather
# than code, which is how a shell command ended up being fed to sfdisk as a
# partition-table entry. Catch it here instead of 200 lines later.
for _tag in INITEOF GRUBEOF BUILDEOF; do
    _n_open=$(grep -c "<<'\?${_tag}" "$0" || true)
    _n_close=$(grep -c "^${_tag}\$" "$0" || true)
    [ "${_n_open}" = "${_n_close}" ] \
        || die "corrupt script: ${_tag} has ${_n_open} opener(s) and ${_n_close} terminator(s)"
done
# CRLF makes every generated file (init, grub.cfg) unusable in ways that only
# show up at boot. Windows editors and some browsers introduce it silently.
case "$(head -c 200 "$0" | tr -d '\n')" in
    *$'\r'*) die "this script has CRLF line endings. Convert with: sed -i 's/\r$//' $0" ;;
esac

if [ -f /.dockerenv ]; then
    log "build-iso.sh rev ${SCRIPT_REV} -- running inside the build container"
else
    log "build-iso.sh rev ${SCRIPT_REV} -- running natively on this host"
fi

# Package names, not just binary names. "missing tool: grub-install" sends you
# looking for a package called grub-install, which does not exist.
need() {
    command -v "$1" >/dev/null && return 0
    die "missing tool: $1  --  Debian/Ubuntu package: $2"
}
need cpio         cpio
need gzip         gzip
need depmod       kmod
need grub-install grub2-common
need sfdisk       fdisk
need losetup      util-linux
need mkfs.vfat    dosfstools
need truncate     coreutils
need find         findutils
need sha256sum    coreutils
# Kernel modules ship compressed and the compression differs by distro:
# Debian uses .ko.xz, Ubuntu 22.04+ uses .ko.zst. busybox insmod can decompress
# neither, so a missing decompressor here becomes "No storage detected" at the
# bench -- with no error message anywhere near the cause.
command -v unxz   >/dev/null || warn "missing unxz (package: xz-utils) -- .ko.xz modules cannot be unpacked"
command -v unzstd >/dev/null || warn "missing unzstd (package: zstd) -- .ko.zst modules cannot be unpacked"

rm -rf "${WORK}"
mkdir -p "${WORK}/initramfs" "${WORK}/iso/boot/grub" "${DIST}"
ROOT="${WORK}/initramfs"

# ---------------------------------------------------------------------------
log "building the scanner (static, no runtime dependencies)"
make -C "${PROJECT}" static >/dev/null
[ -x "${PROJECT}/hwscan" ] || die "scanner build failed"

# ---------------------------------------------------------------------------
log "assembling initramfs"
mkdir -p "${ROOT}"/{bin,sbin,proc,sys,dev,tmp,mnt,lib,opt/hwscan,etc}

BUSYBOX="$(command -v busybox || true)"
[ -n "${BUSYBOX}" ] || BUSYBOX=/bin/busybox
[ -x "${BUSYBOX}" ] || die "busybox not found (apt install busybox-static)"
cp "${BUSYBOX}" "${ROOT}/bin/busybox"
chmod +x "${ROOT}/bin/busybox"

# One binary provides every shell utility we need.
for applet in sh ash mount umount ls cat echo sleep dmesg modprobe insmod \
              lsmod poweroff reboot mkdir sync clear printf grep sed head; do
    ln -sf busybox "${ROOT}/bin/${applet}"
done

# The kernel loads modules on demand through a usermode helper, and that helper
# is /sbin/modprobe unless told otherwise. /sbin was being created empty, so
# request_module() had nothing to call -- which meant a FAT mount could never
# pull in the NLS charset it needs, and failed with EINVAL instead.
for applet in modprobe insmod lsmod; do
    ln -sf ../bin/busybox "${ROOT}/sbin/${applet}"
done

cp "${PROJECT}/hwscan" "${ROOT}/opt/hwscan/hwscan"
chmod +x "${ROOT}/opt/hwscan/hwscan"

# pci.ids turns PCI hex IDs into product names. 1.5 MB uncompressed, and it
# compresses to a few hundred KB inside the initramfs.
for p in /usr/share/misc/pci.ids /usr/share/hwdata/pci.ids; do
    [ -f "$p" ] && { cp "$p" "${ROOT}/opt/hwscan/pci.ids"; break; }
done
[ -f "${ROOT}/opt/hwscan/pci.ids" ] || warn "pci.ids not found: device names will be hex IDs"

# ---------------------------------------------------------------------------
log "selecting kernel and modules"
KVER="$(ls -1 /lib/modules 2>/dev/null | sort -V | tail -1 || true)"
[ -n "${KVER}" ] || die "no kernel modules under /lib/modules (apt install linux-image-amd64)"
KERNEL="/boot/vmlinuz-${KVER}"
[ -f "${KERNEL}" ] || KERNEL="$(ls -1 /boot/vmlinuz-* 2>/dev/null | sort -V | tail -1)"
[ -f "${KERNEL}" ] || die "no kernel image in /boot"
log "kernel ${KVER}"

# Only the drivers a diagnostics pass actually needs. Firmware blobs are
# deliberately excluded: identifying a Wi-Fi card needs PCI enumeration, not a
# working driver, so we save ~250 MB and still report the adapter correctly.
MODDIR="${ROOT}/lib/modules/${KVER}"
mkdir -p "${MODDIR}"
for sub in \
    kernel/drivers/nvme kernel/drivers/ata kernel/drivers/scsi \
    kernel/drivers/usb/storage kernel/drivers/usb/host kernel/drivers/usb/common \
    kernel/drivers/usb/core kernel/drivers/hid kernel/drivers/mmc \
    kernel/drivers/gpu/drm kernel/drivers/media/cec kernel/drivers/media/rc \
    kernel/drivers/i2c kernel/drivers/platform/x86 kernel/block \
    kernel/drivers/video kernel/drivers/acpi kernel/drivers/pci \
    kernel/fs/fat kernel/fs/exfat kernel/fs/ntfs3 kernel/fs/nls kernel/lib
do
    [ -d "/lib/modules/${KVER}/${sub}" ] || continue
    mkdir -p "${MODDIR}/${sub%/*}"
    cp -a "/lib/modules/${KVER}/${sub}" "${MODDIR}/${sub%/*}/" 2>/dev/null || true
done
# Dependency closure.
#
# Copying whole directories silently misses anything a driver needs from
# OUTSIDE them, and the failure mode is brutal: cec needs rc-core from
# drivers/media/rc, i915 needs wmi from drivers/platform/x86. Neither
# directory was copied, so at boot cec died with "Unknown symbol
# rc_allocate_device", drm_display_helper died with it, i915 died after that,
# no EDID was ever read, and the report said "No connected display" on a
# laptop with a perfectly good panel. The same class of gap took out sd_mod,
# which meant a USB stick enumerated at the SCSI layer but never became a
# block device -- so the scan could not be saved to the stick it booted from.
#
# Ask modprobe what each driver actually needs instead of guessing. This runs
# after the directory copy and only adds files, so it cannot break anything
# that already worked.
log "resolving module dependencies"
MODWANT="
    xhci_pci xhci_hcd ehci_pci ehci_hcd ohci_pci ohci_hcd uhci_hcd
    usb_storage uas usbhid hid_generic
    sd_mod sr_mod scsi_mod
    libata libahci ahci ata_piix ata_generic
    nvme nvme_core
    mmc_core mmc_block sdhci sdhci_pci sdhci_acpi
    vfat exfat ntfs3 fat nls_cp437 nls_iso8859_1 nls_utf8 nls_ascii
    battery ac
    i2c_core i2c_algo_bit rc_core cec
    drm drm_kms_helper drm_display_helper
    i915 amdgpu radeon nouveau
"
MODADDED=0
for m in ${MODWANT}; do
    # Captured first, not piped: under pipefail a failing modprobe would abort
    # the whole build just because one optional driver is not in this kernel.
    DEPS="$(modprobe --dry-run --show-depends -S "${KVER}" "${m}" 2>/dev/null || true)"
    [ -n "${DEPS}" ] || continue
    printf '%s\n' "${DEPS}" | while read -r verb path _rest; do
        [ "${verb}" = "insmod" ] || continue      # "builtin" has no file to copy
        case "${path}" in /*) ;; *) continue ;; esac
        rel="${path#/lib/modules/${KVER}/}"
        mkdir -p "${MODDIR}/$(dirname "${rel}")"
        [ -e "${MODDIR}/${rel}" ] || cp -a "${path}" "${MODDIR}/${rel}" 2>/dev/null || true
    done
    MODADDED=$((MODADDED + 1))
done
log "resolved ${MODADDED} driver groups"

for f in modules.builtin modules.builtin.modinfo modules.order; do
    [ -f "/lib/modules/${KVER}/${f}" ] && cp "/lib/modules/${KVER}/${f}" "${MODDIR}/" || true
done

# busybox insmod cannot decompress modules. A compressed .ko fails with
# "Invalid ELF header magic", which is silent enough to look like a missing
# driver. Decompress everything up front so the format never matters.
DECOMP=0
for ext in xz zst gz; do
    find "${MODDIR}" -name "*.ko.${ext}" -print0 2>/dev/null | while IFS= read -r -d '' f; do
        case "$ext" in
            xz)  unxz  -f "$f" 2>/dev/null || true ;;
            zst) unzstd -q -f --rm "$f" 2>/dev/null || true ;;
            gz)  gunzip -f "$f" 2>/dev/null || true ;;
        esac
    done
    # Command substitution rather than `find | grep -q`: with pipefail set, a
    # pipeline whose producer is killed by SIGPIPE reports failure even though
    # the consumer matched. -quit makes that unlikely, not impossible.
    if [ -n "$(find "${MODDIR}" -name "*.ko.${ext}" -print -quit 2>/dev/null)" ]; then
        warn "some *.ko.${ext} could not be decompressed (is the '${ext}' tool installed?)"
    else
        DECOMP=1
    fi
done
[ "$DECOMP" = "1" ] || true

depmod -b "${ROOT}" "${KVER}" 2>/dev/null \
    || warn "depmod reported problems (module loading may be partial)"

# Fail loudly here rather than at the bench: a missing nvme module shows up as
# "No storage detected" on every machine with an NVMe SSD.
log "verifying critical modules are present"
MISSING=""
for m in nvme usb_storage sd_mod ahci vfat; do
    # modprobe treats - and _ as equivalent, but filenames do not. The USB mass
    # storage driver is usb-storage.ko with a hyphen, so looking only for
    # usb_storage.ko reported it missing on every single build. It was always
    # there -- modules are copied by directory, not by name.
    alt="$(printf '%s' "${m}" | tr '_' '-')"
    [ -n "$(find "${MODDIR}" \( -name "${m}.ko" -o -name "${alt}.ko" \) -print -quit 2>/dev/null)" ] \
        || MISSING="${MISSING} ${m}"
done
if [ -n "${MISSING}" ]; then
    warn "not found as loadable modules:${MISSING}"
    warn "they may be built into this kernel (fine), or genuinely absent (not fine)."
    warn "If storage comes back empty, check the verbose boot entry."
else
    log "all critical modules present"
fi

# ---------------------------------------------------------------------------
log "writing init"
cat > "${ROOT}/init" <<'INITEOF'
#!/bin/sh
# PID 1 inside the initramfs. There is no root filesystem to switch to; this
# is the whole operating system.

mount -t proc     none /proc  2>/dev/null
mount -t sysfs    none /sys   2>/dev/null
mount -t devtmpfs none /dev   2>/dev/null
mount -t tmpfs    none /tmp   2>/dev/null

RESET=$(printf '\033[0m'); BOLD=$(printf '\033[1m'); DIM=$(printf '\033[2m')
CYAN=$(printf '\033[36m'); GREEN=$(printf '\033[32m')
YELLOW=$(printf '\033[33m'); RED=$(printf '\033[31m')

banner() {
    clear 2>/dev/null
    printf '%s%s\n' "$CYAN$BOLD" "  hwscan -- hardware inventory"
    printf '%s\n\n' "$DIM  results are appended to a USB stick$RESET"
}

load_drivers() {
    printf '%s  loading drivers...%s\n' "$DIM" "$RESET"
    FAILED=""

    # Order matters when modules.dep is incomplete: every module here is
    # listed after the things it depends on, so plain insmod-style loading
    # still works as a fallback.
    #
    #   cec + i2c_algo_bit come BEFORE drm, or drm_display_helper aborts with
    #   "Unknown symbol cec_*" and no EDID is ever read.
    #   nvme_core comes BEFORE nvme.
    for m in \
        i2c_core i2c_algo_bit cec \
        ehci_pci ehci-hcd ohci_pci ohci-hcd uhci_hcd xhci_pci xhci_hcd \
        scsi_mod sd_mod usb_storage uas \
        libata libahci ahci ata_piix \
        nvme_core nvme_common nvme \
        mmc_core mmc_block sdhci sdhci_pci sdhci_acpi \
        battery ac \
        nls_cp437 nls_iso8859_1 nls_iso8859_15 nls_utf8 nls_ascii nls_cp850 \
        fat vfat exfat ntfs3 \
        drm drm_kms_helper drm_display_helper \
        i915 amdgpu radeon nouveau
    do
        modprobe "$m" >/dev/null 2>&1 || FAILED="$FAILED $m"
    done

    # NVMe and USB mass storage both enumerate asynchronously, and NVMe is
    # almost always first. Breaking out as soon as *anything* appeared meant
    # the USB stick -- the slow one, and the one we need to write to -- never
    # got the extra time.
    sleep 4

    # Some USB bridges answer the first TEST UNIT READY with "medium not
    # present". sd_mod logs "Media removed, stopped polling", attaches the disk
    # with size 0, and then never retries: the stick we booted from is present
    # but has no capacity and no partition table. Re-triggering the scan makes
    # the kernel re-read both, which normally brings it straight back.
    # Step 1 -- SCSI level. Ask the disk to re-read its capacity and partition
    # table. Cheap, and enough for a device that was merely slow.
    kick_storage() {
        for h in /sys/class/scsi_host/host*; do
            [ -e "$h/scan" ] && echo '- - -' > "$h/scan" 2>/dev/null
        done
        for d in /sys/block/sd*; do
            [ -e "$d/size" ] || continue
            [ "$(cat "$d/size" 2>/dev/null)" = "0" ] || continue
            [ -e "$d/device/rescan" ] && echo 1 > "$d/device/rescan" 2>/dev/null
        done
    }

    # Step 2 -- driver level. Make usb-storage drop the device and probe it
    # from scratch, which re-runs INQUIRY and READ CAPACITY. A SCSI rescan
    # cannot do this: it reuses the existing, already-wedged SCSI device.
    rebind_usb_storage() {
        for i in /sys/bus/usb/drivers/usb-storage/*:*; do
            [ -e "$i" ] || continue
            n=$(basename "$i")
            printf '%s  re-binding usb-storage on %s%s\n' "$DIM" "$n" "$RESET"
            echo "$n" > /sys/bus/usb/drivers/usb-storage/unbind 2>/dev/null
            sleep 1
            echo "$n" > /sys/bus/usb/drivers/usb-storage/bind 2>/dev/null
        done
    }

    # Step 3 -- bus level. Take the device off the bus and put it back. This is
    # a full USB re-enumeration: the software equivalent of unplugging it and
    # plugging it in again, which is what usually revives a bridge that came up
    # in a bad state. Safe here because we are running entirely from RAM.
    reauthorize_usb() {
        for d in /sys/bus/usb/devices/*-*; do
            case "$(basename "$d")" in *:*) continue ;; esac   # skip interfaces
            [ -w "$d/authorized" ] || continue
            ms=0
            for ifc in "$d"/*:*; do
                [ -r "$ifc/bInterfaceClass" ] || continue
                [ "$(cat "$ifc/bInterfaceClass" 2>/dev/null)" = "08" ] && ms=1
            done
            [ "$ms" = "1" ] || continue
            printf '%s  re-enumerating USB device %s%s\n' "$DIM" "$(basename "$d")" "$RESET"
            echo 0 > "$d/authorized" 2>/dev/null
            sleep 1
            echo 1 > "$d/authorized" 2>/dev/null
            sleep 2
        done
    }

    # A port that keeps announcing "new high-speed USB device number N" without
    # ever producing a device is stuck in an enumeration loop. It floods the
    # console, burns through the bus's 127 device addresses, and can starve a
    # genuine device trying to attach. A working device enumerates once or
    # twice, so anything past 20 attempts is broken hardware, not slowness.
    disable_runaway_ports() {
        dmesg 2>/dev/null \
          | sed -n 's/.*usb \([0-9][0-9]*-[0-9][0-9]*\): new .*USB device number.*/\1/p' \
          | sort | uniq -c \
          | while read -r n port; do
                [ "$n" -gt 20 ] 2>/dev/null || continue
                b="${port%%-*}"; pn="${port##*-}"
                d="/sys/bus/usb/devices/usb${b}/${b}-0:1.0/port${pn}/disable"
                if [ -w "$d" ]; then
                    echo 1 > "$d" 2>/dev/null \
                      && printf '%s  disabled runaway USB port %s (%s failed attempts)%s\n' \
                         "$YELLOW" "$port" "$n" "$RESET"
                fi
            done
    }
    disable_runaway_ports

    # Step 4 -- controller level. Unbind and rebind the xHCI host controller
    # itself. Device-level resets cannot help when the *controller* was left in
    # a bad state by the firmware handoff, which is the remaining explanation
    # for a stick that a UEFI just booted from and Linux then sees as empty.
    # A laptop's built-in keyboard is on i8042, not USB, so input survives.
    reset_usb_controller() {
        for drv in xhci_hcd ehci-pci ehci_hcd; do
            dd_="/sys/bus/pci/drivers/$drv"
            [ -d "$dd_" ] || continue
            for slot in "$dd_"/0000:*; do
                [ -e "$slot" ] || continue
                n=$(basename "$slot")
                printf '%s  resetting USB controller %s (%s)%s\n' "$DIM" "$n" "$drv" "$RESET"
                echo "$n" > "$dd_/unbind" 2>/dev/null
                sleep 2
                echo "$n" > "$dd_/bind" 2>/dev/null
                sleep 4
            done
        done
    }

    for i in 1 2 3 4 5 6 7 8 9 10; do
        empty=0
        any=0
        for d in /sys/block/sd* /sys/block/mmcblk*; do
            [ -e "$d/size" ] || continue
            any=1
            [ "$(cat "$d/size" 2>/dev/null)" = "0" ] && empty=1
        done
        # No removable disk at all yet, or one with a real size: stop waiting.
        [ "$any" = "1" ] && [ "$empty" = "0" ] && break
        # Escalate. A disk that is still 0 bytes after three SCSI rescans will
        # not be fixed by a fourth; it needs a harder reset.
        case $i in
            1|2)   kick_storage ;;
            3|4)   rebind_usb_storage ;;
            5|6)   reauthorize_usb ;;
            7)     reset_usb_controller ;;
            *)     kick_storage ;;
        esac
        sleep 2
    done

    # Belt and braces: even with /sbin/modprobe in place, say explicitly where
    # the helper lives.
    echo /sbin/modprobe > /proc/sys/kernel/modprobe 2>/dev/null || true

    # FAT mounts fail with a bare "Invalid argument" when a charset is missing,
    # which is indistinguishable from a corrupt filesystem. Show what is loaded.
    nlsl=$(lsmod 2>/dev/null | sed -n 's/^\(nls_[^ ]*\).*/\1/p' | tr '\n' ' ')
    printf '%s  charsets loaded: %s%s\n' "$DIM" "${nlsl:-none (may be built in)}" "$RESET"

    if [ -n "$FAILED" ]; then
        # Most of these are simply built into the kernel, which is fine. This
        # line exists so a genuinely missing driver is visible, not silent.
        printf '%s  modules not loaded (usually built-in):%s%s\n' "$DIM" "$FAILED" "$RESET"
    fi

    # Report size and partition count, not just names. "sda" alone looked
    # healthy while being a 0-byte disk the scanner could never use.
    devs=""
    for d in /sys/block/nvme* /sys/block/sd* /sys/block/mmcblk*; do
        [ -e "$d/size" ] || continue
        n=$(basename "$d")
        mb=$(( $(cat "$d/size" 2>/dev/null || echo 0) / 2048 ))
        np=0
        for pp in "$d"/"$n"*; do
            [ -e "$pp/partition" ] && np=$((np + 1))
        done
        devs="$devs ${n}:${mb}MB/${np}p"
    done
    printf '%s  block devices:%s%s\n' "$DIM" "$devs" "$RESET"
}

scan() {
    banner
    load_drivers
    /opt/hwscan/hwscan
    return $?
}

# The stick is not in use once we are running: the kernel and initramfs were
# copied into RAM by the bootloader and nothing reads the device afterwards.
# So an operator can physically unplug and replug it, and that is the one reset
# a wedged USB bridge always honours -- software cannot make a controller
# re-read a capacity it has decided is absent.
save_to_usb() {
    _have=0
    for _d in /sys/block/sd*; do
        [ -e "$_d/size" ] || continue
        [ "$(cat "$_d/size" 2>/dev/null)" != "0" ] && _have=1
    done

    if [ "$_have" = "0" ]; then
        printf '\n%s  The scan is safe in RAM. Nothing is reading the USB stick,%s\n' "$CYAN" "$RESET"
        printf '%s  so it is safe to unplug it right now.%s\n\n' "$CYAN" "$RESET"
        printf '    1. Unplug the USB stick\n'
        printf '    2. Wait three seconds\n'
        printf '    3. Plug it back in\n'
        printf '    4. Press %s[Enter]%s\n\n' "$BOLD" "$RESET"

        # Drop the wedged SCSI device first. Without this the replug can
        # reattach to the same device that already reported no media, and
        # nothing changes.
        for _d in /sys/block/sd*; do
            [ -e "$_d/size" ] || continue
            [ "$(cat "$_d/size" 2>/dev/null)" = "0" ] || continue
            [ -e "$_d/device/delete" ] && echo 1 > "$_d/device/delete" 2>/dev/null
        done

        printf '  > '
        read -r _dummy || true
        printf '%s  waiting for the stick...%s\n' "$DIM" "$RESET"
        for _i in 1 2 3 4 5 6 7 8 9 10 11 12; do
            _ok=0
            for _d in /sys/block/sd*; do
                [ -e "$_d/size" ] || continue
                [ "$(cat "$_d/size" 2>/dev/null)" != "0" ] && _ok=1
            done
            [ "$_ok" = "1" ] && break
            kick_storage
            sleep 1
        done
    fi

    devs=""
    for _d in /sys/block/sd*; do
        [ -e "$_d/size" ] || continue
        devs="$devs $(basename "$_d"):$(( $(cat "$_d/size") / 2048 ))MB"
    done
    printf '%s  block devices now:%s%s\n' "$DIM" "${devs:- none}" "$RESET"

    /opt/hwscan/hwscan --flush-pending
}

show_result() {
    scan
    rc=$?
    case $rc in
        0) printf '%s  PASS%s\n' "$GREEN$BOLD" "$RESET" ;;
        1) printf '%s  WARN%s\n' "$YELLOW$BOLD" "$RESET" ;;
        2) printf '%s  FAIL%s\n' "$RED$BOLD" "$RESET" ;;
        *) printf '%s  SCAN ERROR%s\n' "$RED$BOLD" "$RESET" ;;
    esac
}

# Scan once up front. The menu below must NOT re-enter this: pressing [u] used
# to fall through to the top of the loop and probe the machine all over again,
# which looked like the tool had hung in a loop and appended a duplicate row to
# the pending file every time.
show_result

while true; do
    if [ -s /tmp/hwscan/inventory.csv ]; then
        printf '\n  %s[u]%s SAVE TO USB   %s[Enter]%s rescan   %s[s]%s shell   %s[p]%s power off   %s[r]%s reboot\n\n  > ' \
            "$GREEN" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET"
    else
        printf '\n  %s[Enter]%s rescan   %s[s]%s shell   %s[p]%s power off   %s[r]%s reboot\n\n  > ' \
            "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET"
    fi
    # Without the EOF guard, a console that returns EOF (serial console, or a
    # detached tty) turns this into a 100% CPU rescan loop.
    if ! read -r key; then
        printf '\n  no console input -- idling. Power off with the button.\n'
        while true; do sleep 3600; done
    fi
    case "$key" in
        u|U) save_to_usb ;;
        s|S) printf '  type "exit" to return\n'; /bin/sh ;;
        p|P) printf '  powering off...\n'; sync; sleep 1; poweroff -f ;;
        r|R) printf '  rebooting...\n';     sync; sleep 1; reboot -f ;;
        *)   show_result ;;
    esac
done
INITEOF
chmod +x "${ROOT}/init"

# ---------------------------------------------------------------------------
log "packing initramfs"
( cd "${ROOT}" && find . | cpio -o -H newc --quiet ) | gzip -9 > "${WORK}/iso/boot/initrd.img"
cp "${KERNEL}" "${WORK}/iso/boot/vmlinuz"

# Provenance. A stick found in a drawer six weeks later should be able to say
# what it is, which kernel it carries, and which commit built the scanner.
cat > "${WORK}/buildinfo.txt" <<BUILDEOF
name:    ${NAME}
built:   $(date -u +%Y-%m-%dT%H:%M:%SZ)
kernel:  ${KVER}
commit:  ${HWSCAN_COMMIT:-unknown (built outside CI)}
BUILDEOF
cp "${WORK}/buildinfo.txt" "${WORK}/iso/hwscan-build.txt"

cat > "${WORK}/iso/boot/grub/grub.cfg" <<'GRUBEOF'
set default=0
set timeout=3

# Load the video drivers before anything needs them. Without this, GRUB's
# linux loader has no video driver registered, grub_video_set_mode() fails
# with "error: no suitable video mode found." and it continues "in blind
# mode" -- the kernel starts but is handed no framebuffer, so the screen
# stays dark and the machine looks hung.
#
# all_video is a meta-module: efi_gop and efi_uga on UEFI, vbe and vga on
# legacy BIOS. Loading it covers both firmware types from one config.
insmod all_video
insmod video

# keep = hand GRUB's current framebuffer straight through to the kernel.
# That is what makes efifb work on UEFI machines, which frequently have no
# VGA text mode to fall back on at all.
set gfxmode=auto
set gfxpayload=keep

menuentry "hwscan -- scan this machine" {
    linux /boot/vmlinuz console=tty0 quiet loglevel=3 usbcore.autosuspend=-1 \
          usb-storage.delay_use=5 scsi_mod.scan=sync
    initrd /boot/initrd.img
}
menuentry "hwscan (USB compatibility -- if the stick is not found)" {
    linux /boot/vmlinuz console=tty0 quiet loglevel=3 usbcore.autosuspend=-1 \
          usbcore.old_scheme_first=1 usbcore.initial_descriptor_timeout=10000 \
          usbcore.use_both_schemes=1 scsi_mod.scan=sync usb-storage.delay_use=15 \
          usb-storage.quirks=1f75:091b:fso
    initrd /boot/initrd.img
}
menuentry "hwscan (text mode -- try this if the screen stays blank)" {
    set gfxpayload=text
    linux /boot/vmlinuz console=tty0 quiet loglevel=3
    initrd /boot/initrd.img
}
menuentry "hwscan (verbose, for troubleshooting)" {
    linux /boot/vmlinuz console=tty0 debug ignore_loglevel
    initrd /boot/initrd.img
}
menuentry "hwscan (safe graphics -- no panel model or size)" {
    linux /boot/vmlinuz console=tty0 nomodeset
    initrd /boot/initrd.img
}
GRUBEOF

# ---------------------------------------------------------------------------
# Writable disk image: ONE FAT32 partition that is simultaneously the EFI
# system partition, the BIOS boot volume, and the results volume. This is what
# lets the CSV land on the same stick you boot from.
# ---------------------------------------------------------------------------
# Release the mount and both loop devices, in the right order, tolerating any
# of them being absent. Safe to call more than once.
cleanup_img() {
    if [ -n "${MNT:-}" ] && grep -q " ${MNT} " /proc/mounts 2>/dev/null; then
        sync
        umount "${MNT}" 2>/dev/null || umount -l "${MNT}" 2>/dev/null || true
    fi
    if [ -n "${PART_LOOPS:-}" ]; then
        for _pl in ${PART_LOOPS}; do losetup -d "${_pl}" 2>/dev/null || true; done
        PART_LOOPS=""
    fi
    if [ -n "${LOOP:-}" ]; then
        partx -d "${LOOP}" 2>/dev/null || true
        losetup -d "${LOOP}" 2>/dev/null || true
        LOOP=""
    fi
    return 0
}

build_img() {
    IMG="${DIST}/${NAME}.img"
    SIZE_MB="${HWSCAN_IMG_MB:-1024}"
    MNT="${WORK}/imgmnt"
    rm -f "${IMG}"
    mkdir -p "${MNT}"

    log "creating ${SIZE_MB} MB disk image"
    truncate -s "${SIZE_MB}M" "${IMG}"

    # Two partitions, which separates the two jobs the stick has to do:
    #
    #   p1  boot   type ef (EFI System), label HWSCAN     -- GRUB, kernel, initramfs
    #   p2  data   type 0c (FAT32 LBA),  label HWSCANDATA -- inventory.csv
    #
    # Windows deliberately hides EFI System partitions, which is exactly what
    # made the whole stick vanish from Explorer when the image was one ef
    # partition. With this layout Windows hides p1 and shows p2, so the stick
    # gets one drive letter containing nothing but the results -- and there is
    # no way to accidentally delete the bootloader while copying a CSV off it.
    #
    # p2 is last, so it can be grown to fill a larger stick afterwards without
    # moving anything. The scanner already prefers a HWSCANDATA volume over a
    # HWSCAN one, so it writes to p2 and leaves the boot partition untouched.
    BOOT_MB="${HWSCAN_BOOT_MB:-512}"
    PART_TYPE="${HWSCAN_PART_TYPE:-ef}"
    DATA_MB=$((SIZE_MB - BOOT_MB - 1))
    if [ "${DATA_MB}" -lt 256 ]; then
        die "image too small: ${SIZE_MB} MB leaves only ${DATA_MB} MB for results. Raise HWSCAN_IMG_MB (boot needs ${BOOT_MB} MB, results need at least 256 MB)."
    fi

    BOOT_START=2048
    BOOT_SECTORS=$((BOOT_MB * 2048))
    DATA_START=$((BOOT_START + BOOT_SECTORS))

    log "partitions: p1 boot ${BOOT_MB} MB type ${PART_TYPE}, p2 data ${DATA_MB} MB type 0c"
    # Built with printf rather than a heredoc, deliberately. A heredoc body is
    # ordinary text in this file, so any edit that lands a stray line inside it
    # gets fed to sfdisk as a partition-table command -- which is exactly how
    # "line 1: unsupported command" happened. printf takes its newlines from
    # the format string, so the table cannot be corrupted by surrounding edits
    # and does not care what line endings this file was saved with.
    SFOUT="$(printf 'label: dos\nstart=%s, size=%s, type=%s, bootable\nstart=%s, type=0c\n' \
        "${BOOT_START}" "${BOOT_SECTORS}" "${PART_TYPE}" "${DATA_START}" \
        | sfdisk "${IMG}" 2>&1)" || {
            printf '%s\n' "${SFOUT}" >&2
            die "sfdisk failed to write the partition table (PART_TYPE=${PART_TYPE}, BOOT_MB=${BOOT_MB}, SIZE_MB=${SIZE_MB})"
        }

    # Confirm both partitions actually exist before going any further.
    sfdisk -l "${IMG}" 2>/dev/null | grep -q "2 " \
        || { printf '%s\n' "${SFOUT}" >&2; die "partitioning produced no second partition"; }

    # Getting usable partition devices is the single most fragile step, and it
    # fails for two independent reasons that stack:
    #
    #   1. The loop driver is almost always loaded with max_part=0, so
    #      `losetup -P` succeeds but the kernel creates no partitions at all.
    #   2. Inside a container, /dev is a tmpfs that Docker populated from a
    #      snapshot of the host at start-up. Even once the kernel does create
    #      the partition, the node appears in the *host's* devtmpfs and never
    #      shows up in here.
    #
    # resolve_part tries, in order: the node itself, a forced partition rescan,
    # creating the node by hand from sysfs, and finally an offset loop that
    # needs no partition device at all.
    modprobe loop max_part=8 2>/dev/null || true

    LOOP="$(losetup --show -f -P "${IMG}")" || die "losetup failed (need --privileged)"
    LOOPNAME="$(basename "${LOOP}")"
    PART_LOOPS=""         # offset loops we created, for cleanup

    command -v udevadm >/dev/null && udevadm settle --timeout=5 2>/dev/null || true
    for _ in 1 2 3 4 5; do [ -b "${LOOP}p1" ] && break; sleep 1; done
    if [ ! -b "${LOOP}p1" ]; then
        partx -a "${LOOP}" >/dev/null 2>&1 || true
        partprobe "${LOOP}" >/dev/null 2>&1 || true
        for _ in 1 2 3; do [ -b "${LOOP}p1" ] && break; sleep 1; done
    fi

    # resolve_part <index> <start_sector> <size_sectors|0>  -> echoes a device
    resolve_part() {
        _n="$1"; _off="$2"; _sz="$3"
        _dev="${LOOP}p${_n}"
        [ -b "${_dev}" ] && { echo "${_dev}"; return 0; }

        # sysfs is shared with the host even when /dev is not.
        if [ -r "/sys/class/block/${LOOPNAME}p${_n}/dev" ]; then
            _num="$(cat "/sys/class/block/${LOOPNAME}p${_n}/dev")"
            mknod "${_dev}" b "${_num%:*}" "${_num#*:}" 2>/dev/null || true
            [ -b "${_dev}" ] && { echo "${_dev}"; return 0; }
        fi

        # Offset loop. Works for mkfs and mount, but grub-install cannot tell
        # which partition of which disk a bare offset loop belongs to.
        if [ "${_sz}" -gt 0 ]; then
            _l="$(losetup --show -f -o $((_off * 512)) --sizelimit $((_sz * 512)) "${IMG}" 2>/dev/null)" || return 1
        else
            _l="$(losetup --show -f -o $((_off * 512)) "${IMG}" 2>/dev/null)" || return 1
        fi
        PART_LOOPS="${PART_LOOPS} ${_l}"
        echo "${_l}"
    }

    trap cleanup_img EXIT

    BOOTPART="$(resolve_part 1 "${BOOT_START}" "${BOOT_SECTORS}")" \
        || die "could not map the boot partition. Run with --privileged and -v /dev:/dev, or build natively."
    DATAPART="$(resolve_part 2 "${DATA_START}" 0)" \
        || die "could not map the results partition."

    case " ${PART_LOOPS} " in
        *" ${BOOTPART} "*)
            warn "boot partition is a bare offset loop -- BIOS boot cannot be installed"
            warn "the image will be UEFI-only"
            SKIP_BIOS=1 ;;
    esac
    log "boot partition: ${BOOTPART}   results partition: ${DATAPART}"

    # The results volume. HWSCANDATA is rank 0 in the scanner's candidate
    # ordering, so it is preferred over the boot volume automatically.
    mkfs.vfat -F 32 -n HWSCANDATA "${DATAPART}" >/dev/null \
        || die "mkfs.vfat failed on the results partition ${DATAPART}"

    PART="${BOOTPART}"
    mkfs.vfat -F 32 -n HWSCAN "${PART}" >/dev/null || die "mkfs.vfat failed on ${PART}"
    mount "${PART}" "${MNT}" || die "cannot mount ${PART}"

    mkdir -p "${MNT}/boot/grub" "${MNT}/EFI/BOOT"
    cp "${WORK}/iso/boot/vmlinuz"    "${MNT}/boot/"
    cp "${WORK}/iso/boot/initrd.img" "${MNT}/boot/"
    cp "${WORK}/iso/boot/grub/grub.cfg" "${MNT}/boot/grub/"
    {
        cat "${WORK}/buildinfo.txt"
        printf 'image:   %s MB, MBR type %s\n' "${SIZE_MB}" "${PART_TYPE}"
    } > "${MNT}/hwscan-build.txt"

    # all_video is embedded as well as insmod'd: if module loading from
    # /boot/grub ever fails, the boot still gets a video driver.
    GRUB_MODS="part_msdos fat normal linux echo test configfile search search_fs_uuid all_video video"

    # No --device-map here. grub-install in GRUB 2.12 has no such option and
    # exits with "unrecognized option", installing nothing at all. It resolves
    # a loop device perfectly well on its own -- grub-probe reports
    # (hostdisk//dev/loop0,msdos1) and the install completes.
    BIOS_OK=0; UEFI_OK=0

    if [ "${SKIP_BIOS:-0}" = "1" ]; then
        warn "skipping BIOS GRUB: /boot is on a bare offset loop, so grub-install"
        warn "cannot determine which partition of which disk it lives on"
    else
    log "installing GRUB for BIOS"
    if grub-install --target=i386-pc --boot-directory="${MNT}/boot" \
        --modules="${GRUB_MODS} biosdisk" \
        --recheck "${LOOP}" >"${WORK}/grub-bios.log" 2>&1; then
        BIOS_OK=1
    else
        warn "BIOS GRUB install failed (see ${WORK}/grub-bios.log)"
    fi
    fi

    log "installing GRUB for UEFI"
    # --removable writes EFI/BOOT/BOOTX64.EFI, the path firmware looks for on a
    # USB stick. --no-nvram keeps us from touching the build host's boot entries.
    if grub-install --target=x86_64-efi --efi-directory="${MNT}" \
        --boot-directory="${MNT}/boot" --removable --no-nvram \
        --modules="${GRUB_MODS}" --recheck >"${WORK}/grub-uefi.log" 2>&1; then
        UEFI_OK=1
    else
        warn "UEFI GRUB install failed (see ${WORK}/grub-uefi.log)"
    fi

    # Verify what actually landed on the filesystem rather than trusting exit
    # codes. An image that reports success and then does not boot costs a trip
    # to the bench and back.
    log "verifying image contents"
    [ -f "${MNT}/EFI/BOOT/BOOTX64.EFI" ] || UEFI_OK=0
    [ "${SKIP_BIOS:-0}" = "1" ] || [ -d "${MNT}/boot/grub/i386-pc" ] || BIOS_OK=0
    for f in boot/vmlinuz boot/initrd.img boot/grub/grub.cfg; do
        [ -s "${MNT}/${f}" ] || die "missing from image: ${f}"
    done
    if [ "${UEFI_OK}" = "0" ] && [ "${BIOS_OK}" = "0" ]; then
        die "neither UEFI nor BIOS boot files were installed -- this image would not boot. Check ${WORK}/grub-*.log"
    fi
    [ "${UEFI_OK}" = "1" ] || warn "UEFI boot files ABSENT -- this image boots on legacy BIOS only"
    [ "${BIOS_OK}" = "1" ] || warn "BIOS boot files ABSENT -- this image boots on UEFI only"

    cleanup_img
    trap - EXIT

    [ -s "${IMG}" ] || die "image build produced nothing"
    ( cd "${DIST}" && sha256sum "${NAME}.img" > "${NAME}.img.sha256" )

    log "done"
    printf '\n  IMAGE: %s\n  size:  %s\n\n' "${IMG}" "$(du -h "${IMG}" | cut -f1)"
    printf '  Write it with:\n'
    printf '    sudo dd if=%s of=/dev/sdX bs=4M status=progress conv=fsync\n\n' "${IMG}"
    printf '  or Rufus in DD mode. The stick then has ONE FAT32 partition that\n'
    printf '  boots the machine AND receives HWSCAN/inventory.csv.\n\n'
}

build_iso() {
    need grub-mkrescue grub-common
    need xorriso       xorriso
    command -v mformat >/dev/null || warn "missing mformat (package: mtools) -- grub-mkrescue may fail to build the EFI image"
    log "building hybrid ISO (UEFI + BIOS, read-only)"
    grub-mkrescue -o "${DIST}/${NAME}.iso" "${WORK}/iso" \
        -- -volid HWSCAN >/dev/null 2>&1 \
        || grub-mkrescue -o "${DIST}/${NAME}.iso" "${WORK}/iso" >/dev/null
    [ -f "${DIST}/${NAME}.iso" ] || die "grub-mkrescue produced no image"
    ( cd "${DIST}" && sha256sum "${NAME}.iso" > "${NAME}.iso.sha256" )
    log "done"
    printf '\n  ISO:  %s\n  size: %s\n\n' "${DIST}/${NAME}.iso" \
        "$(du -h "${DIST}/${NAME}.iso" | cut -f1)"
    warn "This image is READ-ONLY. Results need a second stick, or use Ventoy."
}

if [ "${HWSCAN_MAKE_IMG:-1}" = "1" ]; then
    build_img
else
    build_iso
fi
