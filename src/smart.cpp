#include "smart.h"
#include "util.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>

namespace smart {
namespace {

// --- NVMe SMART / Health Information log (log id 0x02) ---------------------
bool nvmeLog(int fd, unsigned char* out512) {
    struct nvme_admin_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.opcode   = 0x02;                       // Get Log Page
    cmd.nsid     = 0xFFFFFFFF;                 // whole controller
    cmd.addr     = (unsigned long long)(uintptr_t)out512;
    cmd.data_len = 512;
    // cdw10: log identifier in bits 7:0, (dwords-1) in bits 31:16
    cmd.cdw10    = 0x02 | (((512 / 4) - 1) << 16);
    return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) == 0;
}

unsigned long long le(const unsigned char* p, int n) {
    unsigned long long v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// --- ATA PASS-THROUGH(16): issue one SMART subcommand ----------------------
//
// Status handling is the subtle part. A SATL that is asked for the ATA output
// registers (CK_COND=1) answers with CHECK CONDITION *even when the command
// succeeded*, carrying descriptor 09h "ATA Status Return" in the sense data.
// Treating any non-zero driver_status as failure therefore rejects every
// healthy SATA drive. We ask with CK_COND=0 and, when sense data comes back
// anyway, we inspect it instead of discarding the transfer.
bool ataSmart(int fd, unsigned char feature, unsigned char* out512) {
    unsigned char cdb[16];
    memset(cdb, 0, sizeof cdb);
    cdb[0]  = 0x85;    // ATA PASS-THROUGH (16)
    cdb[1]  = 4 << 1;  // protocol 4 = PIO Data-In, extend = 0
    cdb[2]  = 0x0E;    // ck_cond=0, t_dir=1 (in), byt_blok=1, t_length=2
    cdb[4]  = feature; // 0xD0 READ DATA, 0xD1 READ THRESHOLDS
    cdb[6]  = 1;       // one 512-byte sector
    cdb[10] = 0x4F;    // lba_mid  ) the SMART signature; without it the drive
    cdb[12] = 0xC2;    // lba_high ) rejects the command
    cdb[14] = 0xB0;    // ATA command: SMART

    unsigned char sense[64];
    memset(sense, 0, sizeof sense);
    memset(out512, 0, 512);

    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    io.interface_id    = 'S';
    io.cmd_len         = sizeof cdb;
    io.mx_sb_len       = sizeof sense;
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.dxfer_len       = 512;
    io.dxferp          = out512;
    io.cmdp            = cdb;
    io.sbp             = sense;
    io.timeout         = 15000;

    if (ioctl(fd, SG_IO, &io) < 0) return false;
    if (io.host_status != 0) return false;          // transport never completed

    // Sense data present: decide whether it describes success or a real error.
    if (io.sb_len_wr > 0) {
        unsigned char rc = sense[0] & 0x7F;
        if (rc == 0x72 || rc == 0x73) {             // descriptor format
            unsigned char key = sense[1] & 0x0F;
            // 0 = NO SENSE, 1 = RECOVERED ERROR: both mean the data is good.
            if (key > 1) return false;
        } else if (rc == 0x70 || rc == 0x71) {      // fixed format
            unsigned char key = sense[2] & 0x0F;
            if (key > 1) return false;
        }
    } else if (io.driver_status != 0) {
        return false;                                // failed with no explanation
    }

    // A short transfer means we did not get a full 512-byte SMART page.
    if (io.resid > 0 && io.resid >= (int)io.dxfer_len) return false;
    return true;
}

struct Attr { int value = -1, worst = -1, threshold = -1; unsigned long long raw = 0; bool present = false; };

// Returns how many attribute slots were populated. An all-zero page parses to
// zero attributes; without this count a drive that answers with garbage would
// be graded "SMART PASS" purely because nothing tripped a threshold.
int parseAttrs(const unsigned char* data, const unsigned char* thresh, Attr out[256]) {
    int found = 0;
    // SMART READ DATA: 2-byte revision, then 30 entries of 12 bytes.
    for (int i = 0; i < 30; ++i) {
        const unsigned char* e = data + 2 + i * 12;
        int id = e[0];
        if (id == 0) continue;
        if (!out[id].present) ++found;
        out[id].present = true;
        out[id].value   = e[3];
        out[id].worst   = e[4];
        out[id].raw     = le(e + 5, 6);
    }
    if (!thresh) return found;
    for (int i = 0; i < 30; ++i) {
        const unsigned char* e = thresh + 2 + i * 12;
        int id = e[0];
        if (id == 0) continue;
        out[id].threshold = e[1];
    }
    return found;
}

}  // namespace

Info query(const std::string& devPath, bool isNvme, bool rotational) {
    Info info;
    int fd = open(devPath.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return info;

    unsigned char buf[512];

    if (isNvme) {
        if (nvmeLog(fd, buf)) {
            info.queried    = true;
            info.haveStatus = true;
            unsigned char critical = buf[0];
            unsigned char spare    = buf[3];
            unsigned char used     = buf[5];
            info.passed    = (critical == 0);
            info.healthPct = util::clampd(100.0 - (double)used, 0, 100);
            info.powerOnHours = (long long)le(buf + 128, 8);
            if (critical) {
                char t[64];
                snprintf(t, sizeof t, "NVMe critical warning 0x%02X", critical);
                info.notes.push_back(t);
            }
            if (spare < 100 && spare > 0)
                info.notes.push_back("NVMe spare " + util::fmtInt(spare) + "%");
        }
        close(fd);
        return info;
    }

    unsigned char thresh[512];
    bool haveThresh = false;
    if (!ataSmart(fd, 0xD0, buf)) { close(fd); return info; }
    haveThresh = ataSmart(fd, 0xD1, thresh);
    close(fd);

    Attr a[256];
    if (parseAttrs(buf, haveThresh ? thresh : nullptr, a) == 0) {
        // The command succeeded but the page is empty. Reporting this as a
        // healthy drive would be worse than reporting nothing.
        info.notes.push_back("SMART page returned no attributes");
        return info;
    }
    info.queried = true;

    // Overall assessment: any pre-fail attribute at or below its threshold.
    if (haveThresh) {
        info.haveStatus = true;
        for (int id = 1; id < 256; ++id)
            if (a[id].present && a[id].threshold > 0 && a[id].value >= 0 &&
                a[id].value <= a[id].threshold) {
                info.passed = false;
                info.notes.push_back("SMART attribute " + util::fmtInt(id) + " below threshold");
            }
    }

    if (a[9].present) info.powerOnHours = (long long)a[9].raw;

    if (!rotational) {
        // Vendors disagree on which attribute carries wear; try in order of
        // how commonly they are populated and how directly they mean "life left".
        for (int id : {231, 233, 177, 202, 173, 169}) {
            if (a[id].present && a[id].value > 0 && a[id].value <= 100) {
                info.healthPct = a[id].value;
                break;
            }
        }
        if (info.healthPct < 0)
            info.notes.push_back("SSD wear indicator not exposed by firmware");
        return info;
    }

    // Spinning disks have no wear counter. This is a triage score, not a
    // remaining-life percentage -- the raw counts go into the notes so the
    // evidence stays visible.
    double score = 100.0;
    unsigned long long realloc_ = a[5].present   ? a[5].raw   : 0;
    unsigned long long pending  = a[197].present ? a[197].raw : 0;
    unsigned long long uncorr   = a[198].present ? a[198].raw : 0;
    unsigned long long hours    = a[9].present   ? a[9].raw   : 0;

    if (realloc_) {
        score -= std::min(45.0, 12.0 + realloc_ * 0.5);
        info.notes.push_back(util::fmtInt((long long)realloc_) + " reallocated sectors");
    }
    if (pending) {
        score -= std::min(45.0, 22.0 + pending * 2.0);
        info.notes.push_back(util::fmtInt((long long)pending) + " pending sectors");
    }
    if (uncorr) {
        score -= std::min(45.0, 22.0 + uncorr * 2.0);
        info.notes.push_back(util::fmtInt((long long)uncorr) + " uncorrectable sectors");
    }
    if (hours) score -= std::min(20.0, (double)hours / 50000.0 * 20.0);

    info.healthPct = util::clampd(score, 1, 100);
    return info;
}

}  // namespace smart
