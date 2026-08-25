// report.h -- schema, grading, CSV output and USB discovery.
#pragma once

#include "collect.h"
#include <map>
#include <string>
#include <vector>

namespace report {

// The 42 columns, in output order. The TPM columns are deliberately absent
// (dropped by request); Storage Health and Storage Health % are present and
// are filled from a direct SMART read.
extern const std::vector<std::string> FIELDS;

struct Thresholds {
    double batteryFail = 50, batteryWarn = 70;
    double storageFail = 30, storageWarn = 60;
    double minRamGb    = 8;
    bool   requireSerial = true;
    bool   requireUefi   = false;
};

// Returns "PASS" / "WARN" / "FAIL" and appends the reasons.
std::string grade(const collect::Result& r, const Thresholds& t,
                  std::vector<std::string>& reasons);

// Append one row, writing the header if the file is new. flock + fsync, so a
// stick pulled mid-write cannot corrupt earlier rows.
bool appendCsv(const std::string& path, const std::map<std::string, std::string>& row,
               std::string& err);

// Find a writable removable volume and mount it. Returns "" if none.
// `mountedByUs` tells the caller whether to unmount afterwards.
// `usedFallback` is set when no USB volume was found and the caller is being
// handed a RAM-backed directory instead -- the caller must say so loudly.
std::string findOutputDir(const std::string& subdir, int waitSeconds,
                          bool& mountedByUs, std::string& device,
                          bool& usedFallback);
void releaseOutput(bool mountedByUs);

// Append every data row of a RAM-held CSV to a USB volume, then clear it.
// Used by the operator-triggered retry: the scan already happened, the rows
// are sitting in tmpfs, and the stick has just been coaxed back to life.
// Returns the number of rows written, or -1 if no USB volume could be found.
int flushPending(const std::string& pendingPath, const std::string& subdir,
                 int waitSeconds, std::string& device, std::string& err);

}  // namespace report
