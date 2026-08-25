#include "util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>     // errno, used by readLL
#include <cstdio>
#include <cstdlib>    // strtoll
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <limits.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace util {

std::string ROOT;

void setRoot(const std::string& path) {
    ROOT = path;
    while (!ROOT.empty() && ROOT.back() == '/') ROOT.pop_back();
}

std::string P(const std::string& path) { return ROOT.empty() ? path : ROOT + path; }

// ---------------------------------------------------------------------------
// filesystem
// ---------------------------------------------------------------------------
std::string readText(const std::string& path, const std::string& def) {
    FILE* f = fopen(P(path).c_str(), "rb");
    if (!f) return def;
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return trim(out);
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen(P(path).c_str(), "rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

long long readLL(const std::string& path, long long def) {
    std::string s = readText(path);
    if (s.empty()) return def;
    errno = 0;
    char* end = nullptr;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno || end == s.c_str()) return def;
    return v;
}

bool exists(const std::string& path) {
    struct stat st;
    return lstat(P(path).c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct stat st;
    if (stat(P(path).c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(P(path).c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string readLink(const std::string& path) {
    char buf[PATH_MAX];
    // realpath() resolves the whole chain; keep the sysroot prefix on the input
    // but hand back a path the collectors can compare against sysfs names.
    if (realpath(P(path).c_str(), buf) != nullptr) return std::string(buf);
    ssize_t n = readlink(P(path).c_str(), buf, sizeof buf - 1);
    if (n > 0) { buf[n] = '\0'; return std::string(buf); }
    return "";
}

std::string baseName(const std::string& path) {
    size_t p = path.find_last_of('/');
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

// ---------------------------------------------------------------------------
// strings
// ---------------------------------------------------------------------------
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    auto sp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0'; };
    while (b < e && sp((unsigned char)s[b])) ++b;
    while (e > b && sp((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string collapse(const std::string& s) {
    std::string out;
    bool sp = false;
    for (unsigned char c : s) {
        if (c == '\0') continue;
        if (isspace(c)) { sp = true; continue; }
        if (sp && !out.empty()) out += ' ';
        sp = false;
        out += (char)c;
    }
    return out;
}

std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return o;
}

// OEMs ship these placeholders constantly. Treating "To Be Filled By O.E.M."
// as a serial number would collapse hundreds of machines onto one System ID.
static const std::set<std::string>& junk() {
    static const std::set<std::string> j = {
        "", "-", "--", ".", "n/a", "na", "none", "null", "unknown", "unspecified",
        "not specified", "not available", "not applicable", "no", "no dimm",
        "to be filled by o.e.m.", "to be filled by o.e.m", "tobefilledbyoem",
        "fill by oem", "filled by oem", "default string", "system serial number",
        "system manufacturer", "system product name", "system version",
        "system sku number", "chassis serial number", "chassis manufacture",
        "chassis manufacturer", "chassis version", "base board version",
        "base board serial number", "asset-1234567890", "0123456789",
        "1234567890", "123456789", "empty", "oem", "o.e.m.", "product name",
        "manufacturer", "invalid entry length", "standard", "not defined",
        "no asset tag", "no asset information", "*", "x.x", "serial number",
        "part number", "module part number", "type1productconfigid",
    };
    return j;
}

std::string clean(const std::string& s) {
    std::string v = collapse(trim(s));
    if (junk().count(lower(v))) return "";
    // strings made only of separators or zeros are placeholders too
    bool meaningful = false;
    for (unsigned char c : v)
        if (c != '0' && c != '.' && c != '-' && c != '_' && !isspace(c)) meaningful = true;
    return meaningful ? v : "";
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::vector<std::string> keep;
    for (const auto& p : parts) {
        std::string c = clean(p);
        if (c.empty()) continue;
        if (std::find(keep.begin(), keep.end(), c) == keep.end()) keep.push_back(c);
    }
    std::string out;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (i) out += sep;
        out += keep[i];
    }
    return out;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// ---------------------------------------------------------------------------
// formatting
// ---------------------------------------------------------------------------
static std::string stripTrailingZero(const std::string& s) {
    std::string o = s;
    if (o.find('.') == std::string::npos) return o;
    while (!o.empty() && o.back() == '0') o.pop_back();
    if (!o.empty() && o.back() == '.') o.pop_back();
    return o;
}

std::string fmtStorage(unsigned long long bytes) {
    if (bytes == 0) return "";
    char buf[64];
    if (bytes >= 1000ULL * 1000 * 1000 * 1000) {
        snprintf(buf, sizeof buf, "%.1f", (double)bytes / 1e12);
        return stripTrailingZero(buf) + " TB";
    }
    if (bytes < 1000ULL * 1000 * 1000) {
        snprintf(buf, sizeof buf, "%llu MB", (unsigned long long)((bytes + 500000) / 1000000));
        return buf;
    }
    snprintf(buf, sizeof buf, "%llu GB", (unsigned long long)((bytes + 500000000ULL) / 1000000000ULL));
    return buf;
}

std::string fmtRam(unsigned long long bytes) {
    if (bytes == 0) return "";
    double gib = (double)bytes / (1024.0 * 1024 * 1024);
    char buf[64];
    if (gib < 1) {
        snprintf(buf, sizeof buf, "%llu MB", (unsigned long long)(bytes / (1024 * 1024)));
        return buf;
    }
    if (gib < 10) {
        snprintf(buf, sizeof buf, "%.1f", gib);
        return stripTrailingZero(buf) + " GB";
    }
    snprintf(buf, sizeof buf, "%llu GB", (unsigned long long)(gib + 0.5));
    return buf;
}

std::string fmtInt(long long v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", v);
    return buf;
}

std::string fmtFixed1(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// SHA-1 (matches Python hashlib.sha1, so System IDs are interchangeable)
// ---------------------------------------------------------------------------
namespace {
struct Sha1 {
    uint32_t h[5];
    uint64_t total = 0;
    uint8_t  buf[64];
    size_t   n = 0;

    Sha1() { h[0]=0x67452301; h[1]=0xEFCDAB89; h[2]=0x98BADCFE; h[3]=0x10325476; h[4]=0xC3D2E1F0; }
    static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                   ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
            uint32_t t = rol(a,5) + f + e + k + w[i];
            e = d; d = c; c = rol(b,30); b = a; a = t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }

    void update(const uint8_t* p, size_t len) {
        total += len;
        while (len) {
            size_t take = 64 - n;
            if (take > len) take = len;
            memcpy(buf + n, p, take);
            n += take; p += take; len -= take;
            if (n == 64) { block(buf); n = 0; }
        }
    }

    std::string hex() {
        uint64_t bits = total * 8;          // capture before padding grows total
        uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (n != 56) update(&zero, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - i * 8));
        update(lb, 8);
        char out[41];
        for (int i = 0; i < 5; ++i) snprintf(out + i*8, 9, "%08x", h[i]);
        return std::string(out, 40);
    }
};
}  // namespace

std::string sha1hex(const std::string& data) {
    Sha1 s;
    s.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return s.hex();
}

// ---------------------------------------------------------------------------
std::string nowDate() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d", &tmv);
    return buf;
}

std::string nowTime() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    return buf;
}

}  // namespace util
