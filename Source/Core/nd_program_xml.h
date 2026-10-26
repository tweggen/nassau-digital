#pragma once

// nd_program_xml.h - the program format (DESIGN §12): one XML grammar, owned
// by the core, read by qbx and by the later plugin player with the same code.
//
//   <NassauDigitalProgram version="1" name="Kit 1" polyphony="32" ... nextId="3">
//     <zone id="1" keyLo="36" keyHi="36" ... mode="oneshot" output="aux1" ...>
//       <layer id="2" src="audio/kick.wav" velLo="0" velHi="127" .../>
//     </zone>
//   </NassauDigitalProgram>
//
// Every known attribute is written, in table order (nd_program.h), followed
// by the unknown attributes the file came with; unknown child elements are
// written after the known ones. A missing known attribute takes its default.
//
// Refused on load, with a message: a malformed value, a value out of range,
// an unknown enum value, a zone or layer without an id, a duplicate id, a
// zone with more than four layers, a root element of another name.

#include <string>

#include "nd_program.h"

namespace nd {

constexpr const char* kProgramElement = "NassauDigitalProgram";

struct ProgramParseResult {
    bool ok = false;
    std::string error;
    Program program;
};

/// Reads a program from its element as text (a whole file or the subtree qbx
/// hands over).
ProgramParseResult parseProgram(const std::string& xml);
/// Reads a program from an already parsed element.
ProgramParseResult programFromXml(const XmlNode& root);

/// Writes the program as XML text (no XML declaration).
std::string writeProgram(const Program& program);
XmlNode programToXml(const Program& program);

}  // namespace nd
