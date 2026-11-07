// SPDX-License-Identifier: AGPL-3.0-only
// The few things that differ between Windows and iPadOS: folders, the clock, fonts, opening files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform
{
// Settings and log (created if missing)
std::string DataDir();
// Screenshots (video = false) and recordings (video = true), created if missing.
// Windows: Pictures\Orbit Play and Videos\Orbit Play. iPad: the app's Documents folder (Files app).
std::string CaptureDir(bool video);
// e.g. "OrbitPlay_20261107_103000.png"
std::string TimestampName(const char *prefix, const char *ext);
std::string JoinPath(const std::string &dir, const std::string &name);
// Local time as the user's system formats it, without seconds
std::string ClockText();
// Show a folder in the system file browser (no-op where there isn't one)
void OpenFolder(const std::string &path);
bool FileExists(const std::string &path);
void RemoveFile(const std::string &path);
// Built-in font data: weight 1 light, 2 regular, 3 medium, 4 semibold. Returned memory stays valid.
bool FontData(int weight, const void **data, int *size);
// Icon font file, or "" when the platform has none
std::string IconFontPath();
// Addresses of this device's local IPv4 /24 network (to find consoles where broadcasts are blocked)
std::vector<std::string> LocalSubnetHosts();
// Human-readable name of the release
const char *Name();
}
