// smbios.h -- read SMBIOS/DMI directly, replacing dmidecode.
//
// The kernel hands us the raw table at /sys/firmware/dmi/tables/DMI, which is
// exactly what dmidecode parses. Reading it ourselves removes a 250 KB binary
// and, more importantly, removes the need to shell out and screen-scrape.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace smbios {

struct Entry {
    uint8_t              type = 0;
    std::vector<uint8_t> data;      // formatted area
    std::vector<std::string> strings;  // 1-based in the spec; index 0 unused here

    // Read the string whose 1-based index is stored at `offset`.
    std::string str(size_t offset) const;
    uint8_t     u8(size_t offset) const;
    uint16_t    u16(size_t offset) const;
    uint32_t    u32(size_t offset) const;
};

// Parse the whole table. Empty vector means no SMBIOS (VM, or locked firmware).
std::vector<Entry> load(const std::string& path = "/sys/firmware/dmi/tables/DMI");

// One physical memory module, from type 17.
struct Dimm {
    bool                populated = false;
    unsigned long long  bytes = 0;
    std::string         type;        // DDR4, LPDDR5, ...
    std::string         formFactor;  // SODIMM, Row Of Chips (soldered), ...
    std::string         speed;       // configured speed, else rated
    std::string         vendor;
    std::string         serial;
    std::string         part;
    std::string         locator;
};

std::vector<Dimm> memoryDevices(const std::vector<Entry>& entries);
int  memorySlotCount(const std::vector<Entry>& entries);   // -1 if unknown
std::string chassisTypeName(uint8_t code);

}  // namespace smbios
