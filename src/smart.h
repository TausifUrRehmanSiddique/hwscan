// smart.h -- drive health, without smartctl.
//
// Two paths, because the two bus types expose wear completely differently:
//
//   NVMe -- ioctl(NVME_IOCTL_ADMIN_CMD) with Get Log Page 0x02. The spec
//           mandates a "Percentage Used" counter, so the answer is exact.
//   ATA  -- ioctl(SG_IO) with ATA PASS-THROUGH(16) issuing SMART READ DATA
//           and SMART READ THRESHOLDS. Vendors disagree about which attribute
//           carries wear, so this part is best-effort.
#pragma once

#include <string>
#include <vector>

namespace smart {

struct Info {
    bool   queried    = false;  // did the drive answer at all
    bool   haveStatus = false;  // is `passed` meaningful
    bool   passed     = true;   // overall self-assessment
    double healthPct  = -1;     // remaining life, -1 = not reported
    long long powerOnHours = -1;
    std::vector<std::string> notes;   // e.g. "12 reallocated sectors"
};

// devPath like "/dev/nvme0n1" or "/dev/sda". Needs root.
Info query(const std::string& devPath, bool isNvme, bool rotational);

}  // namespace smart
