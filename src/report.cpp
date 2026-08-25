#include "report.h"
#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>   // flock(2); must follow fcntl.h, which defines struct flock
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

namespace report {

const std::vector<std::string> FIELDS = {
    "System ID", "Scan Date", "Scan Time", "Manufacturer", "Brand", "Model",
    "Serial Number", "Asset Tag", "Form Factor", "Chassis Type", "Motherboard",
    "Motherboard Serial", "BIOS Version", "BIOS Date", "Boot Mode", "Secure Boot",
    "CPU", "CPU Cores", "CPU Threads", "RAM Total", "RAM Type", "RAM Slots Used",
    "Ram Company", "Ram Company Serial Number", "Ram Model", "Storage Total",
    "Storage Type", "Storage Health", "Storage Health %", "Drives", "GPU", "GPU Type", "VRAM", "Display",
    "Display Panel Model", "Display Diagonal (in)", "Battery Health %",
    "Wi-Fi", "Bluetooth", "Ethernet", "Overall Status", "Detail",
};

// ---------------------------------------------------------------------------
std::string grade(const collect::Result& r, const Thresholds& t,
                  std::vector<std::string>& reasons) {
    int rank = 0;                                  // 0 PASS, 1 WARN, 2 FAIL
    auto worse = [&](int v) { if (v > rank) rank = v; };
    auto get = [&](const char* k) {
        auto it = r.row.find(k);
        return it == r.row.end() ? std::string() : it->second;
    };

    if (r.storageBytes == 0) { worse(2); reasons.push_back("No storage detected"); }

    if (r.smartFail) { worse(2); reasons.push_back("SMART FAIL"); }
    if (r.storageHealth >= 0) {
        long long h = (long long)(r.storageHealth + 0.5);
        if (r.storageHealth < t.storageFail) {
            worse(2);
            reasons.push_back("Drive wear " + util::fmtInt(h) + "% (< " +
                              util::fmtInt((long long)t.storageFail) + "%)");
        } else if (r.storageHealth < t.storageWarn) {
            worse(1);
            reasons.push_back("Drive wear " + util::fmtInt(h) + "%");
        }
    } else if (r.storageBytes > 0) {
        worse(1);
        reasons.push_back("Drive health unknown");
    }

    if (r.batteryPct >= 0) {
        long long b = (long long)(r.batteryPct + 0.5);
        if (r.batteryPct < t.batteryFail) {
            worse(2);
            reasons.push_back("Battery " + util::fmtInt(b) + "% (< " +
                              util::fmtInt((long long)t.batteryFail) + "%)");
        } else if (r.batteryPct < t.batteryWarn) {
            worse(1);
            reasons.push_back("Battery " + util::fmtInt(b) + "%");
        }
    }

    double ramGb = (double)r.ramBytes / (1024.0 * 1024 * 1024);
    if (r.ramBytes == 0) { worse(1); reasons.push_back("RAM size unknown"); }
    else if (t.minRamGb > 0 && ramGb < t.minRamGb - 0.1) {
        worse(1);
        reasons.push_back("RAM " + util::fmtInt((long long)(ramGb + 0.5)) + " GB (< " +
                          util::fmtInt((long long)t.minRamGb) + " GB)");
    }

    if (t.requireSerial && get("Serial Number").empty()) {
        worse(1);
        reasons.push_back("No serial number");
    }
    if (t.requireUefi && !util::startsWith(get("Boot Mode"), "UEFI")) {
        worse(1);
        reasons.push_back("Not booted in UEFI mode");
    }
    if (get("Form Factor") == "Laptop") {
        if (!r.wifiPresent) { worse(1); reasons.push_back("No Wi-Fi adapter"); }
        if (get("Display Diagonal (in)").empty()) reasons.push_back("Panel size unknown");
    }

    return rank == 2 ? "FAIL" : (rank == 1 ? "WARN" : "PASS");
}

// ---------------------------------------------------------------------------
static std::string csvEscape(const std::string& v) {
    bool need = v.find_first_of(",\"\n\r") != std::string::npos;
    if (!need) return v;
    std::string out = "\"";
    for (char c : v) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
}

bool appendCsv(const std::string& path, const std::map<std::string, std::string>& row,
               std::string& err) {
    bool fresh = true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) fresh = false;

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { err = std::string("open: ") + strerror(errno); return false; }
    ::flock(fd, LOCK_EX);                       // advisory; harmless if unsupported

    std::string out;
    if (fresh) {
        out += "\xEF\xBB\xBF";                // BOM so Excel opens it as UTF-8
        for (size_t i = 0; i < FIELDS.size(); ++i) {
            if (i) out += ',';
            out += csvEscape(FIELDS[i]);
        }
        out += "\r\n";
    }
    for (size_t i = 0; i < FIELDS.size(); ++i) {
        if (i) out += ',';
        auto it = row.find(FIELDS[i]);
        out += csvEscape(it == row.end() ? "" : it->second);
    }
    out += "\r\n";

    bool ok = write(fd, out.data(), out.size()) == (ssize_t)out.size();
    if (!ok) err = std::string("write: ") + strerror(errno);
    fsync(fd);
    ::flock(fd, LOCK_UN);
    close(fd);
    return ok;
}

// ---------------------------------------------------------------------------
// USB discovery. Internal disks are never touched: candidates must be
// removable, and the EFI partition is excluded explicitly.
// ---------------------------------------------------------------------------
static const char* MOUNT_POINT = "/mnt/hwscan-out";

namespace {

struct Part {
    std::string dev;       // /dev/sdb3
    std::string label;
    unsigned long long bytes = 0;
    int rank = 99;
};

// FAT label straight out of the boot sector -- no blkid, no libblkid.
std::string fatLabel(const std::string& devPath) {
    int fd = open(devPath.c_str(), O_RDONLY);
    if (fd < 0) return "";
    unsigned char bs[512];
    ssize_t n = read(fd, bs, sizeof bs);
    close(fd);
    if (n < 512) return "";
    auto grab = [&](int off) {
        std::string s(reinterpret_cast<char*>(bs + off), 11);
        return util::trim(s);
    };
    if (memcmp(bs + 0x52, "FAT32", 5) == 0) return grab(0x47);
    if (memcmp(bs + 0x36, "FAT", 3) == 0)   return grab(0x2B);
    return "";
}

// Why each disk and partition was accepted or rejected. Printed when no
// output volume could be found -- guessing at this from a photograph of a
// screen costs a rebuild-and-reboot cycle every time.
std::vector<std::string> g_scanNotes;

// "HWSCAN" is the label of our own boot image: a single writable FAT32
// partition that both boots the machine and receives the CSV.
static int rankOfLabel(const std::string& label) {
    std::string l = util::lower(label);
    if (l == "hwscandata") return 0;
    if (l == "hwscan")     return 1;
    if (l == "ventoy")     return 2;
    return 3;
}

std::vector<Part> candidates() {
    std::vector<Part> out;
    for (const auto& disk : util::listDir("/sys/block")) {
        static const char* skip[] = {"loop","ram","zram","sr","fd","md","dm-","nbd"};
        bool bad = false;
        for (const char* s : skip) if (util::startsWith(disk, s)) bad = true;
        if (bad) continue;

        bool removable = util::readLL("/sys/block/" + disk + "/removable", 0) == 1 ||
                         util::contains(util::readLink("/sys/block/" + disk), "/usb");
        if (!removable) {
            g_scanNotes.push_back("    " + disk + ": not removable and not on USB, skipped");
            continue;
        }

        int found = 0, usable = 0;
        for (const auto& p : util::listDir("/sys/block/" + disk)) {
            if (!util::startsWith(p, disk)) continue;
            if (!util::exists("/sys/block/" + disk + "/" + p + "/partition")) continue;
            long long sectors = util::readLL("/sys/block/" + disk + "/" + p + "/size", 0);
            unsigned long long bytes = (unsigned long long)sectors * 512ULL;
            ++found;
            if (bytes < 128ULL * 1024 * 1024) {          // ESP / boot helper
                g_scanNotes.push_back("    /dev/" + p + ": " +
                    std::to_string(bytes / (1024 * 1024)) + " MB, under the 128 MB minimum");
                continue;
            }

            Part x;
            x.dev = "/dev/" + p;
            x.bytes = bytes;
            x.label = fatLabel(x.dev);
            x.rank = rankOfLabel(x.label);
            // An unreadable label is worth saying out loud: it demotes the
            // partition to rank 3, which then makes the /EFI guard apply and
            // silently rejects our own boot volume.
            g_scanNotes.push_back("    /dev/" + p + ": " +
                std::to_string(bytes / (1024 * 1024)) + " MB, label=\"" + x.label +
                "\"" + (x.label.empty() ? " (unreadable -- not FAT?)" : "") +
                ", rank=" + std::to_string(x.rank));
            ++usable;
            out.push_back(x);
        }

        // A stick formatted straight to FAT32 with no partition table -- what
        // Windows "Format..." and most factory-fresh sticks produce -- has no
        // partition children at all. Skipping those made a perfectly good
        // results stick invisible, and the row landed in RAM instead.
        if (usable == 0) {
            long long sectors = util::readLL("/sys/block/" + disk + "/size", 0);
            unsigned long long bytes = (unsigned long long)sectors * 512ULL;
            if (bytes >= 128ULL * 1024 * 1024) {
                Part x;
                x.dev = "/dev/" + disk;
                x.bytes = bytes;
                x.label = fatLabel(x.dev);
                x.rank = rankOfLabel(x.label);
                g_scanNotes.push_back("    /dev/" + disk + ": whole disk, " +
                    std::to_string(bytes / (1024 * 1024)) + " MB, label=\"" + x.label +
                    "\", rank=" + std::to_string(x.rank) + " (no partition table)");
                out.push_back(x);
            } else {
                // Without this the disk vanished from the report entirely: it
                // passed the removable test, produced no usable partition, and
                // then failed the whole-disk size test in silence.
                g_scanNotes.push_back("    /dev/" + disk + ": whole disk is " +
                    std::to_string(bytes / (1024 * 1024)) +
                    " MB, under the 128 MB minimum -- nothing to write to");
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Part& a, const Part& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.bytes > b.bytes;
    });
    return out;
}

// umount() can return EBUSY. If we move on without checking, the next
// candidate's mount() stacks on top of the old one and we write the CSV into
// a filesystem nobody will ever look at.
void forceUnmount() {
    if (umount(MOUNT_POINT) == 0) return;
    umount2(MOUNT_POINT, MNT_DETACH);
}

bool tryMount(const std::string& dev) {
    mkdir(MOUNT_POINT, 0755);
    // msdos is a genuine fallback, not decoration: vfat refuses a volume whose
    // codepage module is missing, and msdos needs no NLS at all. The explicit
    // charset retry covers the same failure from the other side.
    struct Attempt { const char* fs; const char* opts; };
    // A FAT mount needs both a codepage and an iocharset, and the kernel's
    // built-in default for the latter differs between distributions. Relying
    // on it produced EINVAL that looked exactly like a corrupt filesystem, so
    // name every charset explicitly and let one of them succeed. msdos needs
    // no iocharset at all, which makes it the most likely to work of the lot.
    static const Attempt tries[] = {
        {"vfat",  nullptr},
        {"vfat",  "iocharset=utf8,codepage=437"},
        {"vfat",  "iocharset=iso8859-1,codepage=437"},
        {"vfat",  "iocharset=ascii,codepage=437"},
        {"msdos", "codepage=437"},
        {"msdos", nullptr},
        {"exfat", nullptr},
        {"ntfs3", nullptr},
        {"ext4",  nullptr}, {"ext3", nullptr}, {"ext2", nullptr},
    };
    std::string tried;
    for (const auto& a : tries) {
        if (mount(dev.c_str(), MOUNT_POINT, a.fs, 0, a.opts) == 0) return true;
        if (!tried.empty()) tried += ", ";
        tried += std::string(a.fs) + (a.opts ? "(opts)" : "") + "=" + strerror(errno);
    }
    g_scanNotes.push_back("    " + dev + ": every mount failed -- " + tried);
    return false;
}

bool writable(const std::string& dir) {
    std::string probe = dir + "/.hwscan-write-test";
    int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    close(fd);
    unlink(probe.c_str());
    return true;
}

}  // namespace

std::string findOutputDir(const std::string& subdir, int waitSeconds,
                          bool& mountedByUs, std::string& device,
                          bool& usedFallback) {
    mountedByUs = false;
    usedFallback = false;
    bool announced = false;
    for (int elapsed = 0; ; elapsed += 3) {
        for (const auto& p : candidates()) {
            if (!tryMount(p.dev)) continue;
            // A bare EFI system partition has /EFI at the root and nothing
            // else; filling one with inventory.csv is how you make a stick
            // stop booting. But our own boot image ALSO has /EFI, because the
            // one FAT32 partition is both the ESP and the results volume --
            // so a recognised label overrides the check.
            bool ours = (p.rank <= 2);
            if (!ours && util::isDir(std::string(MOUNT_POINT) + "/EFI")) {
                forceUnmount();
                continue;
            }
            std::string dir = std::string(MOUNT_POINT) + "/" + subdir;
            mkdir(dir.c_str(), 0777);
            if (writable(dir)) {
                mountedByUs = true;
                device = p.dev;
                return dir;
            }
            forceUnmount();
        }
        if (elapsed >= waitSeconds) break;
        if (!announced) {
            printf("  no USB storage found -- insert a stick now "
                   "(waiting up to %ds)\n", waitSeconds);
            // Say what we actually saw. "No USB storage" reads as "no stick
            // plugged in", but a stick whose bridge reported no media attaches
            // as a 0 MB disk with no partitions -- present, and unusable. The
            // two need to look different.
            for (const auto& disk : util::listDir("/sys/block")) {
                if (!util::startsWith(disk, "sd") &&
                    !util::startsWith(disk, "mmcblk")) continue;
                std::string base = "/sys/block/" + disk;
                long long mb = util::readLL(base + "/size", 0) / 2048;
                bool rem = util::readLL(base + "/removable", 0) == 1;
                int parts = 0;
                for (const auto& p : util::listDir(base))
                    if (util::startsWith(p, disk) &&
                        util::exists(base + "/" + p + "/partition")) ++parts;
                printf("    saw %s: %lld MB, %d partition(s), removable=%s%s\n",
                       disk.c_str(), mb, parts, rem ? "yes" : "no",
                       mb == 0 ? "  <- no media; bridge did not report capacity" : "");
            }
            // Per-partition detail: which were considered, what label was read,
            // and exactly which errno each mount attempt returned.
            for (const auto& n : g_scanNotes) printf("%s\n", n.c_str());
            fflush(stdout);
            announced = true;
        }
        sleep(3);
    }
    usedFallback = true;
    mkdir("/tmp/hwscan", 0777);
    return "/tmp/hwscan";
}

int flushPending(const std::string& pendingPath, const std::string& subdir,
                 int waitSeconds, std::string& device, std::string& err) {
    FILE* f = fopen(pendingPath.c_str(), "rb");
    if (!f) { err = "no pending scans at " + pendingPath; return -1; }
    std::vector<std::string> lines;
    char buf[8192];
    while (fgets(buf, sizeof buf, f)) {
        std::string l(buf);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        // The pending file carries its own BOM. Keeping it would put a second
        // one in the middle of the merged file, and Excel then shows the first
        // column header with a stray glyph in front of it.
        if (l.size() >= 3 && (unsigned char)l[0] == 0xEF
                          && (unsigned char)l[1] == 0xBB
                          && (unsigned char)l[2] == 0xBF)
            l = l.substr(3);
        if (!l.empty()) lines.push_back(l);
    }
    fclose(f);
    if (lines.size() < 2) { err = "no rows to save"; return -1; }

    bool mounted = false, fallback = false;
    std::string dir = findOutputDir(subdir, waitSeconds, mounted, device, fallback);
    if (fallback) {
        // Falling back to tmpfs here would mean copying the file onto itself.
        releaseOutput(mounted);
        err = "still no USB volume";
        return -1;
    }

    std::string out = dir + "/inventory.csv";
    struct stat st;
    bool needHeader = (stat(out.c_str(), &st) != 0 || st.st_size == 0);
    FILE* o = fopen(out.c_str(), "ab");
    if (!o) { releaseOutput(mounted); err = "cannot open " + out; return -1; }
    int fd = fileno(o);
    flock(fd, LOCK_EX);
    if (needHeader) {
        fputs("\xEF\xBB\xBF", o);              // BOM, so Excel reads UTF-8
        fputs(lines[0].c_str(), o); fputs("\r\n", o);
    }
    // De-duplicate by System ID, keeping the newest scan of each machine.
    // Anything that re-runs the scan -- an accidental keypress, a retry -- adds
    // another row for the same laptop, and the operator should not have to
    // clean that up in Excel afterwards.
    std::vector<std::string> keep;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].compare(0, 9, "System ID") == 0) continue;   // repeated header
        std::string id = lines[i].substr(0, lines[i].find(','));
        bool replaced = false;
        for (auto& k : keep) {
            if (k.compare(0, id.size() + 1, id + ",") == 0) { k = lines[i]; replaced = true; break; }
        }
        if (!replaced) keep.push_back(lines[i]);
    }
    int n = 0;
    for (const auto& l : keep) {
        fputs(l.c_str(), o); fputs("\r\n", o);
        ++n;
    }
    fflush(o);
    fsync(fd);
    flock(fd, LOCK_UN);
    fclose(o);
    sync();
    releaseOutput(mounted);

    // Only now is it safe to drop the RAM copy.
    if (n > 0) unlink(pendingPath.c_str());
    return n;
}

void releaseOutput(bool mountedByUs) {
    sync();
    if (!mountedByUs) return;
    for (int i = 0; i < 3; ++i) {
        if (umount(MOUNT_POINT) == 0) return;
        sleep(1);
    }
    umount2(MOUNT_POINT, MNT_DETACH);
}

}  // namespace report
