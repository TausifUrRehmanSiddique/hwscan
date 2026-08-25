#include "collect.h"
#include "smbios.h"
#include "smart.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>    // atoll
#include <cstring>
#include <map>
#include <set>

using util::clean;
using util::readLL;
using util::readText;

namespace collect {
namespace {

// ===========================================================================
// PCI enumeration + pci.ids lookup (replaces lspci / libpci)
// ===========================================================================
struct PciDev { std::string slot, cls, vendorId, deviceId; };

std::string hexField(const std::string& raw, size_t width) {
    std::string v = util::trim(raw);
    if (util::startsWith(v, "0x") || util::startsWith(v, "0X")) v = v.substr(2);
    // An absent attribute must stay absent. Zero-padding "" into "0000" made
    // every unreadable directory look like a real device 0000:0000.
    if (v.empty()) return "";
    while (v.size() < width) v = "0" + v;
    return util::lower(v);
}

std::vector<PciDev> pciDevices() {
    std::vector<PciDev> out;
    const std::string base = "/sys/bus/pci/devices";
    for (const auto& slot : util::listDir(base)) {
        PciDev d;
        d.slot     = slot;
        d.cls      = hexField(readText(base + "/" + slot + "/class"), 6).substr(0, 4);
        d.vendorId = hexField(readText(base + "/" + slot + "/vendor"), 4);
        d.deviceId = hexField(readText(base + "/" + slot + "/device"), 4);
        if (!d.vendorId.empty()) out.push_back(d);
    }
    return out;
}

struct PciIds {
    std::map<std::string, std::string> vendors, devices;
    bool loaded = false;
    void load() {
        if (loaded) return;
        loaded = true;
        for (const char* p : {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids",
                              "/opt/hwscan/pci.ids"}) {
            std::string text = readText(p);
            if (text.empty()) continue;
            std::string cur;
            size_t pos = 0;
            while (pos < text.size()) {
                size_t eol = text.find('\n', pos);
                if (eol == std::string::npos) eol = text.size();
                std::string line = text.substr(pos, eol - pos);
                pos = eol + 1;
                if (line.size() < 6 || line[0] == '#') continue;
                if (line[0] != '\t') {
                    if (line.compare(4, 2, "  ") != 0) continue;
                    cur = util::lower(line.substr(0, 4));
                    vendors[cur] = util::trim(line.substr(6));
                } else if (line[1] != '\t' && !cur.empty()) {
                    std::string b = line.substr(1);
                    if (b.size() < 6 || b.compare(4, 2, "  ") != 0) continue;
                    devices[cur + ":" + util::lower(b.substr(0, 4))] = util::trim(b.substr(6));
                }
            }
            break;
        }
    }
};
PciIds g_ids;

std::string shortenVendor(std::string v) {
    static const char* tails[] = {" Technologies, Inc.", " Technology, Inc.",
                                  " Semiconductor, Inc.", " Corporation", " Corp.",
                                  " Co., Ltd.", " Inc.", " Ltd.", " Corp", " Inc"};
    for (const char* t : tails) {
        size_t n = strlen(t);
        if (v.size() > n && v.compare(v.size() - n, n, t) == 0) {
            v = v.substr(0, v.size() - n);
            break;
        }
    }
    return v;
}

std::string pciName(const PciDev& d) {
    g_ids.load();
    std::string vendor, device;
    auto v = g_ids.vendors.find(d.vendorId);
    if (v != g_ids.vendors.end()) vendor = shortenVendor(v->second);
    auto x = g_ids.devices.find(d.vendorId + ":" + d.deviceId);
    if (x != g_ids.devices.end()) device = x->second;
    if (!vendor.empty() && !device.empty()) return vendor + " " + device;
    if (!vendor.empty()) return vendor + " [" + d.deviceId + "]";
    return d.vendorId + ":" + d.deviceId;
}

// ===========================================================================
// EDID
// ===========================================================================
struct Edid {
    bool ok = false;
    std::string vendor, productCode, name;
    int width = 0, height = 0, widthMm = 0, heightMm = 0;
    double diagonalIn = 0;
};

std::string pnpVendor(const std::string& id) {
    static const std::map<std::string, std::string> m = {
        {"AUO","AU Optronics"},{"BOE","BOE"},{"CMN","Chi Mei / Innolux"},
        {"LGD","LG Display"},{"SDC","Samsung Display"},{"SHP","Sharp"},
        {"IVO","InfoVision"},{"CSO","CSOT"},{"HSD","HannStar"},{"TMX","Tianma"},
        {"LEN","Lenovo"},{"DEL","Dell"},{"HWP","HP"},{"SAM","Samsung"},
        {"GSM","LG"},{"ACR","Acer"},{"AOC","AOC"},{"BNQ","BenQ"},
        {"PHL","Philips"},{"VSC","ViewSonic"},{"APP","Apple"},{"MSI","MSI"},
        {"LPL","LG Philips"},{"SEC","Seiko Epson"},{"CMO","Chi Mei"},
    };
    auto it = m.find(id);
    return it == m.end() ? id : it->second;
}

Edid parseEdid(const std::vector<uint8_t>& b) {
    Edid e;
    if (b.size() < 128) return e;
    static const uint8_t magic[8] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
    if (memcmp(b.data(), magic, 8) != 0) return e;
    unsigned sum = 0;
    for (int i = 0; i < 128; ++i) sum += b[i];
    if (sum % 256 != 0) return e;
    e.ok = true;

    unsigned raw = (unsigned)((b[8] << 8) | b[9]);
    std::string id;
    for (int shift : {10, 5, 0}) id += char(((raw >> shift) & 0x1F) + 'A' - 1);
    bool alpha = true;
    for (char c : id) if (c < 'A' || c > 'Z') alpha = false;
    if (alpha) e.vendor = pnpVendor(id);

    char pc[8];
    snprintf(pc, sizeof pc, "%04X", (unsigned)((b[11] << 8) | b[10]));
    e.productCode = pc;

    e.widthMm  = b[21] * 10;      // coarse centimetre fields
    e.heightMm = b[22] * 10;

    for (int off : {54, 72, 90, 108}) {
        if ((size_t)off + 18 > b.size()) break;
        const uint8_t* d = &b[off];
        if (d[0] == 0 && d[1] == 0 && d[2] == 0) {
            if (d[3] == 0xFC) {
                std::string s;
                for (int i = 5; i < 18 && d[i] != '\n'; ++i) s += char(d[i]);
                e.name = util::trim(s);
            }
            continue;
        }
        if (e.width == 0) {                       // first DTD is the native mode
            e.width  = d[2] | ((d[4] & 0xF0) << 4);
            e.height = d[5] | ((d[7] & 0xF0) << 4);
            int hmm = d[12] | ((d[14] & 0xF0) << 4);   // mm: 10x finer than cm
            int vmm = d[13] | ((d[14] & 0x0F) << 8);
            if (hmm > 0 && vmm > 0) { e.widthMm = hmm; e.heightMm = vmm; }
        }
    }
    if (e.widthMm > 0 && e.heightMm > 0) {
        double d = std::sqrt((double)e.widthMm * e.widthMm +
                             (double)e.heightMm * e.heightMm) / 25.4;
        e.diagonalIn = std::round(d * 10.0) / 10.0;
    }
    return e;
}

std::string panelLabel(const Edid& e) {
    if (!e.ok) return "";
    if (!e.name.empty()) {
        if (!e.vendor.empty() &&
            util::lower(e.name).find(util::lower(e.vendor)) == std::string::npos)
            return e.vendor + " " + e.name;
        return e.name;
    }
    if (!e.vendor.empty()) return util::trim(e.vendor + " " + e.productCode);
    return "";
}

// ===========================================================================
// 1. Identity
// ===========================================================================
const std::vector<std::pair<std::string, std::string>>& brandMap() {
    static const std::vector<std::pair<std::string, std::string>> m = {
        {"lenovo","Lenovo"},{"hewlett","HP"},{"hp inc","HP"},{"compaq","HP"},
        {"dell","Dell"},{"asustek","ASUS"},{"asus","ASUS"},{"acer","Acer"},
        {"apple","Apple"},{"micro-star","MSI"},{"msi","MSI"},{"toshiba","Toshiba"},
        {"dynabook","Dynabook"},{"samsung","Samsung"},{"sony","Sony"},
        {"fujitsu","Fujitsu"},{"panasonic","Panasonic"},{"lg electronics","LG"},
        {"gigabyte","Gigabyte"},{"microsoft","Microsoft"},{"razer","Razer"},
        {"huawei","Huawei"},{"xiaomi","Xiaomi"},{"timi","Xiaomi"},
        {"framework","Framework"},{"google","Google"},{"getac","Getac"},
        {"durabook","Durabook"},{"medion","Medion"},{"clevo","Clevo"},
        {"intel","Intel"},{"supermicro","Supermicro"},{"infinix","Infinix"},
        {"tecno","TECNO"},{"haier","Haier"},{"positivo","Positivo"},
        {"vaio","VAIO"},{"system76","System76"},{"tuxedo","Tuxedo"},
    };
    return m;
}

std::string brandOf(const std::string& vendor) {
    std::string low = util::lower(vendor);
    for (const auto& kv : brandMap())
        if (low.find(kv.first) != std::string::npos) return kv.second;
    return clean(vendor);
}

bool looksLikeMtm(const std::string& s) {   // Lenovo machine-type-model, e.g. 20L5S1P600
    if (s.size() < 6) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return false;
    return true;
}

void identity(Result& r, const std::vector<smbios::Entry>& sm) {
    auto dmi = [](const char* n) { return clean(readText(std::string("/sys/class/dmi/id/") + n)); };

    std::string vendor = dmi("sys_vendor"),   product = dmi("product_name");
    std::string version = dmi("product_version"), family = dmi("product_family");
    std::string serial = dmi("product_serial");
    std::string bVendor = dmi("board_vendor"), bName = dmi("board_name");
    std::string bVersion = dmi("board_version"), bSerial = dmi("board_serial");
    std::string biosVendor = dmi("bios_vendor"), biosVer = dmi("bios_version");
    std::string biosDate = dmi("bios_date"), asset = dmi("chassis_asset_tag");
    int chassisId = (int)readLL("/sys/class/dmi/id/chassis_type", 0);

    // sysfs hides serials from non-root and is absent on some kernels; the raw
    // SMBIOS table we already parsed for RAM fills every gap.
    for (const auto& e : sm) {
        if (e.type == 1) {
            if (vendor.empty())  vendor  = e.str(4);
            if (product.empty()) product = e.str(5);
            if (version.empty()) version = e.str(6);
            if (serial.empty())  serial  = e.str(7);
        } else if (e.type == 2) {
            if (bVendor.empty())  bVendor  = e.str(4);
            if (bName.empty())    bName    = e.str(5);
            if (bVersion.empty()) bVersion = e.str(6);
            if (bSerial.empty())  bSerial  = e.str(7);
        } else if (e.type == 3) {
            if (!chassisId) chassisId = e.u8(5) & 0x7F;
            if (asset.empty()) asset = e.str(8);
        }
    }
    if (sm.empty() && vendor.empty() && product.empty())
        r.notes.push_back("SMBIOS/DMI unreadable (virtual machine or locked firmware?)");
    if (serial.empty()) r.notes.push_back("No system serial in firmware");

    std::string brand = brandOf(vendor);
    std::string model = product;
    // Lenovo puts the MTM in product_name and the marketing name in version.
    if (brand == "Lenovo" && !version.empty() && !looksLikeMtm(version))
        model = product.empty() ? version : version + " (" + product + ")";
    else if (!family.empty() && family.size() < 40 && brand != "Dell" &&
             util::lower(model).find(util::lower(family)) == std::string::npos &&
             // HP sets family to "103C_5336AN HP EliteBook" while product_name
             // is already "HP EliteBook 840 G7"; prepending gives a doubled,
             // unreadable model. If the model already names the brand, the
             // family adds nothing.
             util::lower(model).find(util::lower(brand)) == std::string::npos)
        model = model.empty() ? family : family + " " + model;

    static const std::set<int> laptop = {8,9,10,14,30,31,32};
    static const std::set<int> desktop = {3,4,5,6,7,15,24,34,35,36};
    static const std::set<int> server = {17,23,28,29};
    std::string form = "Unknown";
    if (laptop.count(chassisId)) form = "Laptop";
    else if (desktop.count(chassisId)) form = "Desktop";
    else if (server.count(chassisId)) form = "Server";
    else if (chassisId == 13) form = "All-in-One";

    r.chassisId = chassisId;
    r.row["Manufacturer"]       = vendor;
    r.row["Brand"]              = brand;
    r.row["Model"]              = model;
    r.row["Serial Number"]      = serial;
    r.row["Asset Tag"]          = asset;
    r.row["Form Factor"]        = form;
    r.row["Chassis Type"]       = smbios::chassisTypeName((uint8_t)chassisId);
    r.row["Motherboard"]        = util::join({bVendor, bName, bVersion}, " ");
    r.row["Motherboard Serial"] = bSerial;
    r.row["BIOS Version"]       = util::join({biosVendor, biosVer}, " ");
    r.row["BIOS Date"]          = biosDate;
}

// ===========================================================================
// 2. Boot mode / Secure Boot
// ===========================================================================
void boot(Result& r) {
    if (util::isDir("/sys/firmware/efi")) {
        std::string bits = clean(readText("/sys/firmware/efi/fw_platform_size"));
        r.row["Boot Mode"] = bits.empty() ? "UEFI" : "UEFI (" + bits + "-bit)";
    } else {
        r.row["Boot Mode"] = "Legacy BIOS";
    }

    std::string sb;
    for (const auto& n : util::listDir("/sys/firmware/efi/efivars")) {
        if (!util::startsWith(n, "SecureBoot-")) continue;
        auto v = util::readBytes("/sys/firmware/efi/efivars/" + n);
        if (v.size() >= 5) sb = (v[4] == 1) ? "Enabled" : "Disabled";   // 4 attr bytes, then value
        break;
    }
    if (sb.empty()) sb = (r.row["Boot Mode"] == "Legacy BIOS") ? "N/A" : "Unknown";

    for (const auto& n : util::listDir("/sys/firmware/efi/efivars")) {
        if (!util::startsWith(n, "SetupMode-")) continue;
        auto v = util::readBytes("/sys/firmware/efi/efivars/" + n);
        if (v.size() >= 5 && v[4] == 1) sb += " (Setup Mode)";
        break;
    }
    r.row["Secure Boot"] = sb;
}

// ===========================================================================
// 3. CPU
// ===========================================================================
void cpu(Result& r) {
    std::string text = readText("/proc/cpuinfo");
    std::string model;
    int threads = 0;
    std::set<std::pair<std::string, std::string>> cores;
    std::string pkg, core;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) {
            if (!pkg.empty() && !core.empty()) cores.insert({pkg, core});
            pkg.clear(); core.clear();
            if (eol >= text.size()) break;
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = util::trim(line.substr(0, colon));
        std::string v = util::trim(line.substr(colon + 1));
        if (k == "model name" && model.empty()) model = v;
        else if (k == "Model" && model.empty()) model = v;
        else if (k == "processor") ++threads;
        else if (k == "physical id") pkg = v;
        else if (k == "core id") core = v;
        if (eol >= text.size()) break;
    }
    if (!pkg.empty() && !core.empty()) cores.insert({pkg, core});

    int coreCount = (int)cores.size();
    if (coreCount == 0) coreCount = threads;

    // Tidy "Intel(R) Core(TM) i5-8350U CPU @ 1.70GHz"
    static const char* noise[] = {"(R)", "(TM)", "(r)", "(tm)"};
    for (const char* n : noise) {
        size_t p;
        while ((p = model.find(n)) != std::string::npos) model.erase(p, strlen(n));
    }
    size_t p;
    while ((p = model.find(" CPU ")) != std::string::npos) model.replace(p, 5, " ");
    while ((p = model.find(" Processor ")) != std::string::npos) model.replace(p, 11, " ");
    model = util::collapse(model);
    while (!model.empty() && (model.back() == '@' || model.back() == ' ')) model.pop_back();

    r.row["CPU"]         = clean(model);
    r.row["CPU Cores"]   = coreCount ? util::fmtInt(coreCount) : "";
    r.row["CPU Threads"] = threads ? util::fmtInt(threads) : "";
    if (r.row["CPU"].empty()) r.notes.push_back("CPU model string unavailable");
}

// ===========================================================================
// 4. Memory
// ===========================================================================
void memory(Result& r, const std::vector<smbios::Entry>& sm) {
    auto dimms = smbios::memoryDevices(sm);
    unsigned long long total = 0;
    std::vector<std::string> types, vendors, serials, parts;
    int populated = 0;

    for (const auto& d : dimms) {
        if (!d.populated) continue;
        ++populated;
        total += d.bytes;
        std::string t = d.type;
        if (d.formFactor == "Row Of Chips" && !util::startsWith(t, "LP") && !t.empty())
            t += " (soldered)";       // cannot be upgraded: matters when grading
        if (!d.speed.empty()) t += " " + d.speed;
        types.push_back(t);
        vendors.push_back(d.vendor);
        serials.push_back(d.serial);
        parts.push_back(d.part);
    }

    if (dimms.empty()) {
        // No SMBIOS: /proc/meminfo undercounts by whatever firmware reserved,
        // so round up to the nearest real module size.
        long long kb = 0;
        std::string mi = readText("/proc/meminfo");
        size_t p = mi.find("MemTotal:");
        if (p != std::string::npos) kb = atoll(mi.c_str() + p + 9);
        if (kb > 0) {
            double gib = kb / 1048576.0;
            static const int steps[] = {1,2,3,4,6,8,12,16,20,24,32,40,48,64,96,128,192,256};
            for (int s : steps)
                if (gib <= s * 1.02) { total = (unsigned long long)s << 30; break; }
            if (!total) total = (unsigned long long)(gib + 0.5) * (1ULL << 30);
        }
        r.notes.push_back("RAM module detail unavailable (no SMBIOS type 17)");
    }

    int slots = smbios::memorySlotCount(sm);
    r.ramBytes = total;
    r.row["RAM Total"]  = util::fmtRam(total);
    r.row["RAM Type"]   = util::join(types, " / ");
    // "2 / 2" is silently converted to a date by Excel and LibreOffice the
    // moment the CSV is opened -- the operator sees "02-Feb" in the slots
    // column. "of" is never parsed as a separator, so the value survives.
    if (slots > 0) r.row["RAM Slots Used"] = util::fmtInt(populated) + " of " + util::fmtInt(slots);
    r.row["Ram Company"]               = util::join(vendors);
    r.row["Ram Company Serial Number"] = util::join(serials);
    r.row["Ram Model"]                 = util::join(parts);
    if (!dimms.empty() && populated == 0)
        r.notes.push_back("Firmware reports zero populated RAM slots");
}

// ===========================================================================
// 5. Storage (no SMART -- sysfs and by-id only)
// ===========================================================================
std::map<std::string, std::string> diskSerialsById() {
    // /dev/disk/by-id/ata-Model_SERIAL -> ../../sda. Gives us a serial with no
    // ioctl and no smartctl.
    std::map<std::string, std::string> out;
    const std::string dir = "/dev/disk/by-id";
    for (const auto& name : util::listDir(dir)) {
        if (util::contains(name, "-part")) continue;
        std::string target = util::readLink(dir + "/" + name);
        if (target.empty()) continue;
        std::string dev = util::baseName(target);
        if (dev.empty() || out.count(dev)) continue;
        size_t us = name.rfind('_');
        if (us == std::string::npos || us + 1 >= name.size()) continue;
        std::string serial = name.substr(us + 1);
        if (serial.size() >= 4) out[dev] = serial;
    }
    return out;
}

void storage(Result& r) {
    auto byId = diskSerialsById();
    unsigned long long total = 0;
    std::vector<std::string> kinds, drives, statuses;
    std::vector<double> healths;

    for (const auto& name : util::listDir("/sys/block")) {
        static const char* skip[] = {"loop","ram","zram","sr","fd","md","dm-","nbd"};
        bool bad = false;
        for (const char* s : skip) if (util::startsWith(name, s)) bad = true;
        if (bad) continue;

        long long sectors = readLL("/sys/block/" + name + "/size", 0);
        if (sectors <= 0) continue;
        unsigned long long bytes = (unsigned long long)sectors * 512ULL;
        if (bytes < 1000000000ULL) continue;      // firmware scratch, not storage

        // Never count the boot USB: it would inflate Storage Total on every row.
        long long removable = readLL("/sys/block/" + name + "/removable", 0);
        std::string real = util::readLink("/sys/block/" + name);
        if (removable == 1 || util::contains(real, "/usb")) continue;

        long long rot = readLL("/sys/block/" + name + "/queue/rotational", 1);
        std::string model = clean(readText("/sys/block/" + name + "/device/model"));
        std::string vend  = clean(readText("/sys/block/" + name + "/device/vendor"));
        std::string serial;
        if (byId.count(name)) serial = byId[name];

        std::string kind;
        if (util::startsWith(name, "nvme")) {
            kind = "NVMe SSD";
            std::string ctrl = name.substr(0, name.find('n', 4));
            if (model.empty()) model = clean(readText("/sys/class/nvme/" + ctrl + "/model"));
            if (serial.empty()) serial = clean(readText("/sys/class/nvme/" + ctrl + "/serial"));
        } else if (util::startsWith(name, "mmcblk")) {
            kind = "eMMC";
            if (model.empty()) model = clean(readText("/sys/block/" + name + "/device/name"));
        } else {
            kind = (rot == 1) ? "HDD" : "SATA SSD";
        }

        total += bytes;
        kinds.push_back(kind);

        bool isNvme = util::startsWith(name, "nvme");
        // /dev is real even under --sysroot. Querying it while replaying a
        // captured machine would issue ioctls to *this* workstation's drives
        // and staple the answers onto someone else's inventory row.
        smart::Info si;
        if (util::ROOT.empty())
            si = smart::query("/dev/" + name, isNvme, rot == 1);
        else
            r.notes.push_back(name + ": SMART skipped (--sysroot replay)");

        if (si.queried) {
            if (si.haveStatus) {
                statuses.push_back(si.passed ? "PASS" : "FAIL");
                if (!si.passed) r.smartFail = true;
            }
            if (si.healthPct >= 0) healths.push_back(si.healthPct);
            for (const auto& n : si.notes) r.notes.push_back(name + ": " + n);
        } else if (kind != "eMMC" && util::ROOT.empty()) {
            // eMMC has no SMART at all; anything else should have answered.
            r.notes.push_back(name + ": SMART not available");
        }

        std::vector<std::string> bits = {util::join({vend, model}, " "),
                                         util::fmtStorage(bytes), kind};
        if (!serial.empty()) bits.push_back("S/N " + serial);
        if (si.powerOnHours > 0) bits.push_back(util::fmtInt(si.powerOnHours) + "h");
        if (si.haveStatus) bits.push_back(std::string("SMART ") + (si.passed ? "PASS" : "FAIL"));
        if (si.healthPct >= 0) bits.push_back(util::fmtInt((long long)(si.healthPct + 0.5)) + "%");
        std::string line;
        for (size_t i = 0; i < bits.size(); ++i) {
            if (bits[i].empty()) continue;
            if (!line.empty()) line += " ";
            line += bits[i];
        }
        if (line.empty()) line = name;
        drives.push_back(line);
    }

    r.storageBytes = total;
    r.row["Storage Total"] = util::fmtStorage(total);
    r.row["Storage Type"]  = util::join(kinds, " + ");
    r.row["Drives"]        = util::join(drives);
    if (drives.empty()) { r.notes.push_back("No internal storage detected"); return; }

    bool anyFail = std::find(statuses.begin(), statuses.end(), "FAIL") != statuses.end();
    bool anyPass = std::find(statuses.begin(), statuses.end(), "PASS") != statuses.end();
    std::string overall;
    if (anyFail)                       overall = "FAIL";
    else if (statuses.empty())         overall = "UNKNOWN";
    else if (statuses.size() == drives.size() && anyPass) overall = "PASS";
    else                               overall = "PASS (partial)";
    r.row["Storage Health"] = overall;

    if (!healths.empty()) {
        double worst = *std::min_element(healths.begin(), healths.end());
        r.storageHealth = worst;
        r.row["Storage Health %"] = util::fmtInt((long long)(worst + 0.5));
    }
}

// ===========================================================================
// 6. GPU
// ===========================================================================
void gpu(Result& r, const std::vector<PciDev>& pci) {
    std::vector<std::string> names, types, vrams;
    for (const auto& d : pci) {
        if (d.cls != "0300" && d.cls != "0302" && d.cls != "0380") continue;
        names.push_back(pciName(d));

        bool rootBus = util::startsWith(d.slot, "0000:00:");
        std::string kind;
        if (d.vendorId == "10de") kind = "Discrete";              // NVIDIA
        else if (rootBus && (d.vendorId == "8086" || d.vendorId == "1002" ||
                             d.vendorId == "1022")) kind = "Integrated";
        else kind = "Discrete";
        types.push_back(kind);

        std::string vram;
        // amdgpu exports the real figure; nothing else does without a driver.
        for (const auto& card : util::listDir("/sys/class/drm")) {
            if (!util::startsWith(card, "card") || util::contains(card, "-")) continue;
            std::string link = util::readLink("/sys/class/drm/" + card + "/device");
            if (util::baseName(link) != d.slot) continue;
            long long v = readLL("/sys/class/drm/" + card + "/device/mem_info_vram_total", 0);
            if (v > 0) vram = util::fmtInt((long long)((v + (1LL<<29)) >> 30)) + " GB";
        }
        if (vram.empty() && kind == "Integrated") vram = "Shared";
        vrams.push_back(vram);
    }

    if (names.empty()) { r.notes.push_back("No PCI display controller found"); return; }
    r.row["GPU"] = util::join(names);
    bool disc = std::find(types.begin(), types.end(), "Discrete") != types.end();
    bool integ = std::find(types.begin(), types.end(), "Integrated") != types.end();
    r.row["GPU Type"] = (disc && integ) ? "Hybrid (Integrated + Discrete)"
                                        : util::join(types, " + ");
    r.row["VRAM"] = util::join(vrams);
}

// ===========================================================================
// 7. Display
// ===========================================================================
void display(Result& r) {
    struct Conn { std::string name, native; bool internal; Edid edid; };
    std::vector<Conn> conns;

    for (const auto& n : util::listDir("/sys/class/drm")) {
        if (!util::startsWith(n, "card") || !util::contains(n, "-")) continue;
        std::string path = "/sys/class/drm/" + n;
        if (clean(readText(path + "/status")) != "connected") continue;

        Conn c;
        c.name = n.substr(n.find('-') + 1);
        std::string modes = readText(path + "/modes");
        size_t eol = modes.find('\n');
        c.native = util::trim(eol == std::string::npos ? modes : modes.substr(0, eol));
        std::string up = c.name;
        std::transform(up.begin(), up.end(), up.begin(), ::toupper);
        c.internal = util::startsWith(up, "EDP") || util::startsWith(up, "LVDS") ||
                     util::startsWith(up, "DSI");
        c.edid = parseEdid(util::readBytes(path + "/edid"));
        conns.push_back(c);
    }

    if (conns.empty()) {
        r.notes.push_back("No connected display (headless, or DRM driver not loaded)");
        return;
    }
    std::stable_sort(conns.begin(), conns.end(),
                     [](const Conn& a, const Conn& b) { return a.internal && !b.internal; });

    std::vector<std::string> res;
    for (const auto& c : conns) {
        std::string s = c.native;
        if (s.empty() && c.edid.ok && c.edid.width)
            s = util::fmtInt(c.edid.width) + "x" + util::fmtInt(c.edid.height);
        if (!s.empty()) res.push_back(s + (c.internal ? " (internal)" : " (external)"));
    }
    r.row["Display"] = util::join(res);

    const Edid& e = conns[0].edid;
    if (e.ok) {
        r.row["Display Panel Model"] = panelLabel(e);
        if (e.diagonalIn > 0) r.row["Display Diagonal (in)"] = util::fmtFixed1(e.diagonalIn);
    } else {
        r.notes.push_back("EDID unreadable on " + conns[0].name);
    }
}

// ===========================================================================
// 8. Battery
// ===========================================================================
void battery(Result& r) {
    std::vector<double> health;
    std::vector<long long> cycles;
    bool any = false;

    for (const auto& n : util::listDir("/sys/class/power_supply")) {
        std::string p = "/sys/class/power_supply/" + n;
        if (clean(readText(p + "/type")) != "Battery") continue;
        if (clean(readText(p + "/scope")) == "Device") continue;   // wireless mouse etc.
        any = true;
        long long full   = readLL(p + "/energy_full", 0);
        long long design = readLL(p + "/energy_full_design", 0);
        if (full <= 0 || design <= 0) {
            full   = readLL(p + "/charge_full", 0);
            design = readLL(p + "/charge_full_design", 0);
        }
        long long c = readLL(p + "/cycle_count", 0);
        if (c > 0) cycles.push_back(c);
        if (full > 0 && design > 0) health.push_back((double)full / (double)design * 100.0);
    }

    static const std::set<int> laptop = {8,9,10,14,30,31,32};
    if (!any) {
        if (laptop.count(r.chassisId)) {
            r.row["Battery Health %"] = "0";
            r.batteryPct = 0;
            r.notes.push_back("Laptop chassis but NO battery detected");
        } else {
            r.row["Battery Health %"] = "N/A";
        }
        return;
    }
    if (health.empty()) {
        r.notes.push_back("Battery present but firmware reports no design capacity");
        return;
    }
    double worst = *std::min_element(health.begin(), health.end());
    if (worst > 100.0)
        r.notes.push_back("Battery reports " + util::fmtInt((long long)(worst + 0.5)) +
                          "% of design capacity");
    worst = util::clampd(worst, 0, 100);
    r.batteryPct = worst;
    r.row["Battery Health %"] = util::fmtInt((long long)(worst + 0.5));
    if (!cycles.empty())
        r.notes.push_back("Battery cycles: " +
                          util::fmtInt(*std::max_element(cycles.begin(), cycles.end())));
}

// ===========================================================================
// 9. Network
// ===========================================================================
void network(Result& r, const std::vector<PciDev>& pci) {
    std::vector<std::string> wifi, eth, bt;

    // Map a netdev back to its PCI slot so we can give it a real product name.
    std::map<std::string, std::string> slotOf;
    for (const auto& n : util::listDir("/sys/class/net")) {
        std::string link = util::readLink("/sys/class/net/" + n + "/device");
        if (!link.empty()) slotOf[n] = util::baseName(link);
    }

    for (const auto& n : util::listDir("/sys/class/net")) {
        if (n == "lo") continue;
        std::string real = util::readLink("/sys/class/net/" + n);
        if (util::contains(real, "/virtual/")) continue;
        bool isWifi = util::exists("/sys/class/net/" + n + "/phy80211") ||
                      util::exists("/sys/class/net/" + n + "/wireless");
        std::string label;
        for (const auto& d : pci) if (d.slot == slotOf[n]) label = pciName(d);
        if (label.empty()) label = n;
        if (isWifi) wifi.push_back(label);
        else if (readLL("/sys/class/net/" + n + "/type", 0) == 1) eth.push_back(label);
    }

    // A card with no driver never creates a netdev, so sweep PCI as well. This
    // is how we tell "no adapter" from "adapter present, firmware missing".
    for (const auto& d : pci) {
        if (d.cls == "0280" && wifi.empty()) wifi.push_back(pciName(d) + " (no driver)");
        if (d.cls == "0200" && eth.empty())  eth.push_back(pciName(d) + " (no driver)");
    }

    if (!util::listDir("/sys/class/bluetooth").empty()) {
        for (const auto& h : util::listDir("/sys/class/bluetooth")) {
            std::string link = util::readLink("/sys/class/bluetooth/" + h + "/device");
            std::string label;
            for (const auto& d : pci) if (d.slot == util::baseName(link)) label = pciName(d);
            bt.push_back(label.empty() ? h : label);
        }
    }
    if (bt.empty()) {
        // USB Bluetooth radios: class e0 is "Wireless Controller".
        for (const auto& n : util::listDir("/sys/bus/usb/devices")) {
            std::string p = "/sys/bus/usb/devices/" + n;
            if (clean(readText(p + "/bDeviceClass")) != "e0") continue;
            std::string man = clean(readText(p + "/manufacturer"));
            std::string prod = clean(readText(p + "/product"));
            std::string label = util::join({man, prod}, " ");
            bt.push_back(label.empty() ? "USB wireless controller" : label);
        }
    }

    r.wifiPresent = !wifi.empty();
    r.row["Wi-Fi"]     = wifi.empty() ? "Not detected" : util::join(wifi);
    r.row["Ethernet"]  = eth.empty()  ? "Not detected" : util::join(eth);
    r.row["Bluetooth"] = bt.empty()   ? "Not detected" : util::join(bt);
}

}  // namespace

// ===========================================================================
Result scanAll() {
    Result r;
    r.row["Scan Date"] = util::nowDate();
    r.row["Scan Time"] = util::nowTime();

    auto sm  = smbios::load();
    auto pci = pciDevices();

    identity(r, sm);
    boot(r);
    cpu(r);
    memory(r, sm);
    storage(r);
    gpu(r, pci);
    display(r);
    battery(r);
    network(r, pci);

    // Stable per-machine key so re-scans can be de-duplicated later.
    std::string serial = r.row["Serial Number"], board = r.row["Motherboard Serial"];
    if (!serial.empty() || !board.empty()) {
        r.row["System ID"] = "HW-" + util::lower(util::sha1hex(
            r.row["Manufacturer"] + "|" + r.row["Model"] + "|" + serial + "|" + board))
            .substr(0, 10);
    } else {
        std::string mac;
        for (const auto& n : util::listDir("/sys/class/net")) {
            if (n == "lo") continue;
            if (util::contains(util::readLink("/sys/class/net/" + n), "/virtual/")) continue;
            std::string a = clean(readText("/sys/class/net/" + n + "/address"));
            if (!a.empty() && a != "00:00:00:00:00:00") { mac = a; break; }
        }
        if (!mac.empty()) {
            r.notes.push_back("System ID derived from MAC (no firmware serial)");
            r.row["System ID"] = "HW-" + util::lower(util::sha1hex("mac|" + mac)).substr(0, 10);
        } else {
            r.notes.push_back("System ID is not stable -- no serial and no MAC");
            r.row["System ID"] = "HW-" + util::lower(util::sha1hex(util::nowDate() + util::nowTime())).substr(0, 10);
        }
    }
    std::transform(r.row["System ID"].begin(), r.row["System ID"].end(),
                   r.row["System ID"].begin(), ::toupper);
    return r;
}

}  // namespace collect
