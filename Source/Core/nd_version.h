#pragma once

// nd_version.h - the core's version, the one symbol the C0 scaffold has.

namespace nd {

/// The core's own version string ("major.minor.patch"). A developer and
/// forensics number; the suite's version is the user-facing one.
const char* coreVersion();

/// The highest program-format version this core writes (DESIGN §12).
constexpr int kProgramFormatVersion = 1;

}  // namespace nd
