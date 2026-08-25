#include "smbios.h"
#include "util.h"

#include <cstring>

namespace smbios {

std::string Entry::str(size_t offset) const {
    if (offset >= data.size()) return "";
    uint8_t idx = data[offset];
    if (idx == 0 || idx > strings.size()) return "";
    return util::clean(strings[idx - 1]);
}
uint8_t  Entry::u8(size_t o)  const { return o < data.size() ? data[o] : 0; }
uint16_t Entry::u16(size_t o) const {
    if (o + 1 >= data.size()) return 0;
    return (uint16_t)(data[o] | (data[o + 1] << 8));
}
uint32_t Entry::u32(size_t o) const {
    if (o + 3 >= data.size()) return 0;
    return (uint32_t)data[o] | ((uint32_t)data[o+1] << 8) |
           ((uint32_t)data[o+2] << 16) | ((uint32_t)data[o+3] << 24);
}

std::vector<Entry> load(const std::string& path) {
    std::vector<Entry> out;
    std::vector<uint8_t> buf = util::readBytes(path);
    size_t off = 0;
    while (off + 4 <= buf.size()) {
        uint8_t type = buf[off];
        uint8_t len  = buf[off + 1];
        if (len < 4 || off + len > buf.size()) break;

        Entry e;
        e.type = type;
        e.data.assign(buf.begin() + off, buf.begin() + off + len);

        // String table: NUL-separated, terminated by a double NUL.
        size_t p = off + len;
        while (p < buf.size()) {
            if (buf[p] == 0) {                       // end of one string
                if (p + 1 < buf.size() && buf[p + 1] == 0) { p += 2; break; }
                ++p;
                continue;
            }
            size_t start = p;
            while (p < buf.size() && buf[p] != 0) ++p;
            e.strings.push_back(std::string(reinterpret_cast<const char*>(&buf[start]), p - start));
        }
        // A struct with no strings still has the terminating double NUL.
        if (p == off + len) p += 2;

        out.push_back(std::move(e));
        off = p;
        if (type == 127) break;                      // end-of-table
    }
    return out;
}

// --- SMBIOS 7.18.2 Memory Device -- Type -----------------------------------
static std::string memTypeName(uint8_t c) {
    switch (c) {
        case 0x12: return "DDR";     case 0x13: return "DDR2";
        case 0x14: return "DDR2 FB-DIMM";
        case 0x18: return "DDR3";    case 0x1A: return "DDR4";
        case 0x1B: return "LPDDR";   case 0x1C: return "LPDDR2";
        case 0x1D: return "LPDDR3";  case 0x1E: return "LPDDR4";
        case 0x20: return "HBM";     case 0x21: return "HBM2";
        case 0x22: return "DDR5";    case 0x23: return "LPDDR5";
        case 0x1F: return "Logical non-volatile";
        default:   return "";
    }
}

// --- SMBIOS 7.18.1 Memory Device -- Form Factor ----------------------------
static std::string formFactorName(uint8_t c) {
    switch (c) {
        case 0x09: return "DIMM";
        case 0x0B: return "Row Of Chips";   // soldered: not upgradeable
        case 0x0D: return "SODIMM";
        case 0x0F: return "FB-DIMM";
        case 0x0A: return "TSOP";
        case 0x10: return "Die";
        default:   return "";
    }
}

std::vector<Dimm> memoryDevices(const std::vector<Entry>& entries) {
    std::vector<Dimm> out;
    for (const auto& e : entries) {
        if (e.type != 17 || e.data.size() < 0x15) continue;
        Dimm d;
        d.locator = e.str(0x10);

        uint16_t sz = e.u16(0x0C);
        if (sz == 0 || sz == 0xFFFF) {           // 0 = empty slot, FFFF = unknown
            out.push_back(d);
            continue;
        }
        if (sz == 0x7FFF && e.data.size() >= 0x20) {
            d.bytes = (unsigned long long)e.u32(0x1C) * 1024ULL * 1024ULL;
        } else if (sz & 0x8000) {                 // bit 15 set -> value is in KB
            d.bytes = (unsigned long long)(sz & 0x7FFF) * 1024ULL;
        } else {
            d.bytes = (unsigned long long)sz * 1024ULL * 1024ULL;
        }
        if (d.bytes == 0) { out.push_back(d); continue; }

        d.populated  = true;
        d.type       = memTypeName(e.u8(0x12));
        d.formFactor = formFactorName(e.u8(0x0E));
        d.vendor     = e.str(0x17);
        d.serial     = e.str(0x18);
        d.part       = e.str(0x1A);

        uint16_t confSpeed = e.data.size() >= 0x22 ? e.u16(0x20) : 0;
        uint16_t rated     = e.u16(0x15);
        uint16_t speed     = confSpeed ? confSpeed : rated;
        if (speed && speed != 0xFFFF) d.speed = util::fmtInt(speed) + " MT/s";

        out.push_back(d);
    }
    return out;
}

int memorySlotCount(const std::vector<Entry>& entries) {
    int fromArray = -1;
    for (const auto& e : entries) {
        if (e.type == 16 && e.data.size() >= 0x0F) {
            uint16_t n = e.u16(0x0D);
            if (n && n != 0xFFFF) fromArray = (fromArray < 0 ? 0 : fromArray) + n;
        }
    }
    if (fromArray > 0) return fromArray;
    int count = 0;
    for (const auto& e : entries) if (e.type == 17) ++count;
    return count ? count : -1;
}

// --- SMBIOS 7.4.1 System Enclosure -- Type ---------------------------------
std::string chassisTypeName(uint8_t code) {
    static const char* names[] = {
        "", "Other", "Unknown", "Desktop", "Low Profile Desktop", "Pizza Box",
        "Mini Tower", "Tower", "Portable", "Laptop", "Notebook", "Hand Held",
        "Docking Station", "All In One", "Sub Notebook", "Space-saving",
        "Lunch Box", "Main Server Chassis", "Expansion Chassis", "Sub Chassis",
        "Bus Expansion Chassis", "Peripheral Chassis", "RAID Chassis",
        "Rack Mount Chassis", "Sealed-case PC", "Multi-system Chassis",
        "Compact PCI", "Advanced TCA", "Blade", "Blade Enclosure", "Tablet",
        "Convertible", "Detachable", "IoT Gateway", "Embedded PC", "Mini PC",
        "Stick PC",
    };
    if (code < sizeof(names) / sizeof(names[0])) return names[code];
    return "Unknown";
}

}  // namespace smbios
