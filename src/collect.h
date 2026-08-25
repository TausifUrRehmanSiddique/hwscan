// collect.h -- every hardware probe.
//
// TPM columns were dropped by request. SMART is NOT dropped: Storage Health
// and Storage Health % are read directly via ioctl in src/smart.cpp, with no
// smartctl binary involved.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace collect {

struct Result {
    std::map<std::string, std::string> row;
    std::vector<std::string>           notes;

    // Numeric side channel, kept out of the row so grading works on numbers
    // rather than re-parsing formatted strings.
    double             batteryPct   = -1;   // -1 = unknown or not applicable
    unsigned long long ramBytes     = 0;
    unsigned long long storageBytes = 0;
    double             storageHealth = -1;   // worst drive, -1 = unknown
    bool               smartFail    = false;
    int                chassisId    = 0;
    bool               wifiPresent  = false;
};

Result scanAll();

}  // namespace collect
