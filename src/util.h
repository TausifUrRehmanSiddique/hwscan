// util.h -- foundations shared by every collector.
//
// Rule for this whole program, same as the Python version it replaces:
// nothing here throws. A laptop with a corrupt SMBIOS table, no battery and a
// dead panel must still produce a CSV row.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace util {

// Optional filesystem prefix. Set with --sysroot to replay a captured machine
// on a workstation, so the collectors can be tested without hardware.
extern std::string ROOT;
void setRoot(const std::string& path);
std::string P(const std::string& path);

// ---- filesystem -----------------------------------------------------------
std::string              readText(const std::string& path, const std::string& def = "");
std::vector<uint8_t>     readBytes(const std::string& path);
long long                readLL(const std::string& path, long long def = -1);
bool                     exists(const std::string& path);
bool                     isDir(const std::string& path);
std::vector<std::string> listDir(const std::string& path);      // names, sorted
std::string              readLink(const std::string& path);     // resolved target
std::string              baseName(const std::string& path);

// ---- strings --------------------------------------------------------------
std::string trim(const std::string& s);
std::string collapse(const std::string& s);   // squeeze internal whitespace
std::string lower(const std::string& s);

// Blank out the placeholder junk OEMs leave in SMBIOS fields
// ("To Be Filled By O.E.M.", "Default string", "0123456789", ...).
std::string clean(const std::string& s);

std::string join(const std::vector<std::string>& parts, const std::string& sep = " | ");
bool        contains(const std::string& hay, const std::string& needle);
bool        startsWith(const std::string& s, const std::string& prefix);

// ---- formatting -----------------------------------------------------------
std::string fmtStorage(unsigned long long bytes);   // decimal GB/TB, as sold
std::string fmtRam(unsigned long long bytes);       // binary GB, as sold
std::string fmtInt(long long v);
std::string fmtFixed1(double v);

// ---- misc -----------------------------------------------------------------
std::string sha1hex(const std::string& data);   // matches the Python System ID
std::string nowDate();                          // YYYY-MM-DD
std::string nowTime();                          // HH:MM:SS
double      clampd(double v, double lo, double hi);

}  // namespace util
