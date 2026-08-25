// hwscan -- bootable hardware inventory scanner (C++ implementation).
//
//   hwscan                     find a USB stick, scan, append one CSV row
//   hwscan --output-dir DIR    write here instead of hunting for a USB
//   hwscan --sysroot DIR       replay a captured machine (testing)
//   hwscan --stdout            print the CSV row, write nothing
//
// Exit codes: 0 PASS  1 WARN  2 FAIL  3 scan/IO error
#include "collect.h"
#include "report.h"
#include "util.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

const char* C_RESET = "\033[0m";  const char* C_BOLD = "\033[1m";
// Where a scan lands when no USB volume is available. findOutputDir
// falls back to this same path, so [u] finds exactly what was written.
const char* PENDING_CSV = "/tmp/hwscan/inventory.csv";

const char* C_DIM   = "\033[2m";  const char* C_CYAN = "\033[36m";
const char* C_GREEN = "\033[32m"; const char* C_RED  = "\033[31m";
const char* C_ONGRN = "\033[42;30;1m"; const char* C_ONYEL = "\033[43;30;1m";
const char* C_ONRED = "\033[41;97;1m";

void usage() {
    printf(
        "hwscan -- hardware inventory scanner\n\n"
        "  --output-dir DIR   write results here (skips USB detection)\n"
        "  --sysroot DIR      read /sys and /proc under DIR (testing)\n"
        "  --stdout           print the CSV row to stdout, write nothing\n"
        "  --no-wait          do not wait for a USB stick to appear\n"
        "  --quiet            suppress the on-screen summary\n"
        "  --help             this text\n\n"
        "Exit: 0 PASS  1 WARN  2 FAIL  3 error\n");
}

std::string csvEscapeOut(const std::string& v) {
    if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
    std::string o = "\"";
    for (char c : v) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

void printSummary(const collect::Result& r, const std::string& status) {
    static const char* show[] = {
        "Brand", "Model", "Serial Number", "CPU", "RAM Total", "Storage Total",
        "Display Diagonal (in)", "Battery Health %", "Boot Mode",
    };
    printf("\n%s%s%s\n", C_BOLD, std::string(64, '=').c_str(), C_RESET);
    for (const char* k : show) {
        auto it = r.row.find(k);
        std::string v = (it == r.row.end() || it->second.empty()) ? "-" : it->second;
        printf("  %s%-24s%s %s\n", C_DIM, k, C_RESET, v.c_str());
    }
    printf("%s%s%s\n", C_BOLD, std::string(64, '=').c_str(), C_RESET);

    const char* bg = status == "PASS" ? C_ONGRN : (status == "WARN" ? C_ONYEL : C_ONRED);
    std::string label = "  RESULT: " + status + "  ";
    if (label.size() > 64) label.resize(64);
    // All three arithmetic terms must stay signed. The original mixed int with
    // string::size_type, so an over-long label wrapped to a huge unsigned
    // length and std::string(huge, ' ') aborted -- there are no exceptions in
    // the static build to catch it.
    int width = 64;
    int len   = (int)label.size();
    int pad   = (width - len) / 2;
    if (pad < 0) pad = 0;
    int tail  = width - pad - len;
    if (tail < 0) tail = 0;
    printf("%s%s%s%s\n", bg, std::string(pad, ' ').c_str(),
           (label + std::string(tail, ' ')).c_str(), C_RESET);

    auto d = r.row.find("Detail");
    if (d != r.row.end() && !d->second.empty())
        printf("%s  %s%s\n", C_DIM, d->second.substr(0, 600).c_str(), C_RESET);
    printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string outputDir, sysroot;
    bool toStdout = false, quiet = false, noWait = false, flushPending = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--output-dir") outputDir = next();
        else if (a == "--sysroot") sysroot = next();
        else if (a == "--stdout") { toStdout = true; quiet = true; }
        else if (a == "--quiet") quiet = true;
        else if (a == "--no-wait") noWait = true;
        else if (a == "--flush-pending") flushPending = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 3; }
    }

    if (!sysroot.empty()) util::setRoot(sysroot);

    // Operator-triggered retry. The scan already ran and its rows are sitting
    // in tmpfs; the init script has just reset the USB bus, so try again to
    // put them on the stick. No re-probe: re-scanning would append a second
    // row for the same machine.
    if (flushPending) {
        std::string device, err;
        printf("\n%s  saving to USB...%s\n", C_CYAN, C_RESET);
        int n = report::flushPending(PENDING_CSV, "HWSCAN", 20, device, err);
        if (n < 0) {
            printf("%s  still cannot save: %s%s\n", C_RED, err.c_str(), C_RESET);
            printf("%s  Unplug the stick, wait two seconds, plug it back in,%s\n",
                   C_RED, C_RESET);
            printf("%s  then press [u] again.%s\n\n", C_RED, C_RESET);
            return 3;
        }
        printf("%s  saved %d scan(s) -> %s%s\n", C_GREEN, n, device.c_str(), C_RESET);
        printf("%s  safe to remove%s\n\n", C_DIM, C_RESET);
        return 0;
    }

    if (geteuid() != 0 && sysroot.empty() && !quiet)
        fprintf(stderr, "%swarning: run as root for serial numbers%s\n", C_DIM, C_RESET);

    if (!quiet) printf("%s%shwscan: probing hardware...%s\n", C_CYAN, C_BOLD, C_RESET);

    collect::Result r = collect::scanAll();

    report::Thresholds t;
    std::vector<std::string> reasons;
    std::string status = report::grade(r, t, reasons);
    r.row["Overall Status"] = status;

    for (const auto& n : r.notes) reasons.push_back(n);
    std::string detail;
    for (size_t i = 0; i < reasons.size(); ++i) {
        if (i) detail += "; ";
        detail += reasons[i];
    }
    if (detail.size() > 1800) detail = detail.substr(0, 1800);
    r.row["Detail"] = detail;

    int code = status == "PASS" ? 0 : (status == "WARN" ? 1 : 2);

    if (toStdout) {
        for (size_t i = 0; i < report::FIELDS.size(); ++i)
            printf("%s%s", i ? "," : "", csvEscapeOut(report::FIELDS[i]).c_str());
        printf("\n");
        for (size_t i = 0; i < report::FIELDS.size(); ++i) {
            auto it = r.row.find(report::FIELDS[i]);
            printf("%s%s", i ? "," : "",
                   csvEscapeOut(it == r.row.end() ? "" : it->second).c_str());
        }
        printf("\n");
        return code;
    }

    if (!quiet) printSummary(r, status);

    bool mounted = false, fallback = false;
    std::string device, dir;
    if (!outputDir.empty()) {
        dir = outputDir;
        mkdir(dir.c_str(), 0777);
    } else {
        dir = report::findOutputDir("HWSCAN", noWait ? 0 : 20, mounted, device, fallback);
    }

    std::string path = dir + "/inventory.csv", err;
    if (!report::appendCsv(path, r.row, err)) {
        fprintf(stderr, "%swrite failed: %s%s\n", C_RED, err.c_str(), C_RESET);
        report::releaseOutput(mounted);
        return 3;
    }
    if (!quiet) {
        if (fallback) {
            // /tmp is a tmpfs. Saying "saved" here without qualification is a
            // lie the operator only discovers after powering off.
            printf("\n%s  NOT SAVED TO USB%s\n", C_ONRED, C_RESET);
            printf("%s  Written to %s, which is RAM. It will be LOST on reboot.%s\n",
                   C_RED, path.c_str(), C_RESET);
            printf("%s  The USB stick you booted from should have received this%s\n",
                   C_RED, C_RESET);
            printf("%s  scan. No block device appeared at all, which normally means%s\n",
                   C_RED, C_RESET);
            printf("%s  Press [u] at the menu below to reset the USB bus and%s\n",
                   C_RED, C_RESET);
            printf("%s  try again -- the scan is kept, nothing is re-probed.%s\n\n",
                   C_RED, C_RESET);
        } else {
            printf("%s  saved -> %s%s\n", C_GREEN, path.c_str(), C_RESET);
            if (!device.empty())
                printf("%s  media: %s (safe to remove)%s\n", C_DIM, device.c_str(), C_RESET);
        }
    }
    report::releaseOutput(mounted);
    return code;
}
