//===- MCDwarf.h - Machine Code Dwarf support -------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of the MCDwarfFile to support the dwarf
// .file directive and the .loc directive.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCDWARF_H
#define LLVM_MC_MCDWARF_H

#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Dwarf.h"
#include <vector>

namespace llvm {
  class MCContext;
  class MCSection;
  class MCSymbol;
  class MCObjectStreamer;
  class raw_ostream;

  /// MCDwarfFile - Instances of this class represent the name of the dwarf
  /// .file directive and its associated dwarf file number in the MC file,
  /// and MCDwarfFile's are created and unique'd by the MCContext class where
  /// the file number for each is its index into the vector of DwarfFiles (note
  /// index 0 is not used and not a valid dwarf file number).
  class MCDwarfFile {
    // Name - the base name of the file without its directory path.
    // The StringRef references memory allocated in the MCContext.
    StringRef Name;

    // DirIndex - the index into the list of directory names for this file name.
    unsigned DirIndex;

  private:  // MCContext creates and uniques these.
    friend class MCContext;
    MCDwarfFile(StringRef name, unsigned dirIndex)
      : Name(name), DirIndex(dirIndex) {}

    MCDwarfFile(const MCDwarfFile&);       // DO NOT IMPLEMENT
    void operator=(const MCDwarfFile&); // DO NOT IMPLEMENT
  public:
    /// getName - Get the base name of this MCDwarfFile.
    StringRef getName() const { return Name; }

    /// getDirIndex - Get the dirIndex of this MCDwarfFile.
    unsigned getDirIndex() const { return DirIndex; }


    /// print - Print the value to the stream \arg OS.
    void print(raw_ostream &OS) const;

    /// dump - Print the value to stderr.
    void dump() const;
  };

  inline raw_ostream &operator<<(raw_ostream &OS, const MCDwarfFile &DwarfFile){
    DwarfFile.print(OS);
    return OS;
  }

  /// MCDwarfLoc - Instances of this class represent the information from a
  /// dwarf .loc directive.
  class MCDwarfLoc {
    // FileNum - the file number.
    unsigned FileNum;
    // Line - the line number.
    unsigned Line;
    // Column - the column position.
    unsigned Column;
    // Flags (see #define's below)
    unsigned Flags;
    // Isa
    unsigned Isa;

// Flag that indicates the initial value of the is_stmt_start flag.
#define DWARF2_LINE_DEFAULT_IS_STMT     1

#define DWARF2_FLAG_IS_STMT        (1 << 0)
#define DWARF2_FLAG_BASIC_BLOCK    (1 << 1)
#define DWARF2_FLAG_PROLOGUE_END   (1 << 2)
#define DWARF2_FLAG_EPILOGUE_BEGIN (1 << 3)

  private:  // MCContext manages these
    friend class MCContext;
    friend class MCLineEntry;
    MCDwarfLoc(unsigned fileNum, unsigned line, unsigned column, unsigned flags,
               unsigned isa)
      : FileNum(fileNum), Line(line), Column(column), Flags(flags), Isa(isa) {}

    // Allow the default copy constructor and assignment operator to be used
    // for an MCDwarfLoc object.

  public:
    /// getFileNum - Get the FileNum of this MCDwarfLoc.
    unsigned getFileNum() { return FileNum; }

    /// getLine - Get the Line of this MCDwarfLoc.
    unsigned getLine() { return Line; }

    /// getColumn - Get the Column of this MCDwarfLoc.
    unsigned getColumn() { return Column; }

    /// getFlags - Get the Flags of this MCDwarfLoc.
    unsigned getFlags() { return Flags; }

    /// getIsa - Get the Isa of this MCDwarfLoc.
    unsigned getIsa() { return Isa; }

    /// setFileNum - Set the FileNum of this MCDwarfLoc.
    void setFileNum(unsigned fileNum) { FileNum = fileNum; }

    /// setLine - Set the Line of this MCDwarfLoc.
    void setLine(unsigned line) { Line = line; }

    /// setColumn - Set the Column of this MCDwarfLoc.
    void setColumn(unsigned column) { Column = column; }

    /// setFlags - Set the Flags of this MCDwarfLoc.
    void setFlags(unsigned flags) { Flags = flags; }

    /// setIsa - Set the Isa of this MCDwarfLoc.
    void setIsa(unsigned isa) { Isa = isa; }
  };

  /// MCLineEntry - Instances of this class represent the line information for
  /// the dwarf line table entries.  Which is created after a machine
  /// instruction is assembled and uses an address from a temporary label
  /// created at the current address in the current section and the info from
  /// the last .loc directive seen as stored in the context.
  class MCLineEntry : public MCDwarfLoc {
    MCSymbol *Label;

  private:
    // Allow the default copy constructor and assignment operator to be used
    // for an MCLineEntry object.

  public:
    // Constructor to create an MCLineEntry given a symbol and the dwarf loc.
    MCLineEntry(MCSymbol *label, const MCDwarfLoc loc) : MCDwarfLoc(loc),
                Label(label) {}

    MCSymbol *getLabel() { return Label; }

    // This is called when an instruction is assembled into the specified
    // section and if there is information from the last .loc directive that
    // has yet to have a line entry made for it is made.
    static void Make(MCObjectStreamer *MCOS, const MCSection *Section);
  };

  /// MCLineSection - Instances of this class represent the line information
  /// for a section where machine instructions have been assembled after seeing
  /// .loc directives.  This is the information used to build the dwarf line
  /// table for a section.
  class MCLineSection {

  private:
    MCLineSection(const MCLineSection&);  // DO NOT IMPLEMENT
    void operator=(const MCLineSection&); // DO NOT IMPLEMENT

  public:
    // Constructor to create an MCLineSection with an empty MCLineEntries
    // vector.
    MCLineSection() {}

    // addLineEntry - adds an entry to this MCLineSection's line entries
    void addLineEntry(const MCLineEntry &LineEntry) {
      MCLineEntries.push_back(LineEntry);
    }

    typedef std::vector<MCLineEntry> MCLineEntryCollection;
    typedef MCLineEntryCollection::iterator iterator;

  private:
    MCLineEntryCollection MCLineEntries;

  public:
    MCLineEntryCollection *getMCLineEntries() { return &MCLineEntries; }
  };

  class MCDwarfFileTable {
  public:
    //
    // This emits the Dwarf file and the line tables.
    //
    static void Emit(MCObjectStreamer *MCOS, const MCSection *DwarfLineSection);
  };

  class MCDwarfLineAddr {
  public:
    /// Utility function to encode a Dwarf pair of LineDelta and AddrDeltas.
    static void Encode(int64_t LineDelta, uint64_t AddrDelta, raw_ostream &OS);

    /// Utility function to emit the encoding to a streamer.
    static void Emit(MCObjectStreamer *MCOS,
                     int64_t LineDelta,uint64_t AddrDelta);

    /// Utility function to compute the size of the encoding.
    static uint64_t ComputeSize(int64_t LineDelta, uint64_t AddrDelta);

    /// Utility function to write the encoding to an object writer.
    static void Write(MCObjectWriter *OW,
                      int64_t LineDelta, uint64_t AddrDelta);
  };
} // end namespace llvm

#endif