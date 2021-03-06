//===-- llvm-objdump.cpp - Object file dumping utility for llvm -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This program is a utility that works like binutils "objdump", that is, it
// dumps out a plethora of information about an object file depending on the
// flags.
//
//===----------------------------------------------------------------------===//

#include "llvm-objdump.h"
#include "MCFunction.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/MemoryObject.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/Win64EH.h"
#include <algorithm>
#include <cctype>
#include <cstring>
using namespace llvm;
using namespace object;

static cl::list<std::string>
InputFilenames(cl::Positional, cl::desc("<input object files>"),cl::ZeroOrMore);

static cl::opt<bool>
Disassemble("disassemble",
  cl::desc("Display assembler mnemonics for the machine instructions"));
static cl::alias
Disassembled("d", cl::desc("Alias for --disassemble"),
             cl::aliasopt(Disassemble));

static cl::opt<bool>
Relocations("r", cl::desc("Display the relocation entries in the file"));

static cl::opt<bool>
SectionContents("s", cl::desc("Display the content of each section"));

static cl::opt<bool>
SymbolTable("t", cl::desc("Display the symbol table"));

static cl::opt<bool>
MachO("macho", cl::desc("Use MachO specific object file parser"));
static cl::alias
MachOm("m", cl::desc("Alias for --macho"), cl::aliasopt(MachO));

cl::opt<std::string>
llvm::TripleName("triple", cl::desc("Target triple to disassemble for, "
                                    "see -version for available targets"));

cl::opt<std::string>
llvm::ArchName("arch", cl::desc("Target arch to disassemble for, "
                                "see -version for available targets"));

static cl::opt<bool>
SectionHeaders("section-headers", cl::desc("Display summaries of the headers "
                                           "for each section."));
static cl::alias
SectionHeadersShort("headers", cl::desc("Alias for --section-headers"),
                    cl::aliasopt(SectionHeaders));
static cl::alias
SectionHeadersShorter("h", cl::desc("Alias for --section-headers"),
                      cl::aliasopt(SectionHeaders));

static cl::list<std::string>
MAttrs("mattr",
  cl::CommaSeparated,
  cl::desc("Target specific attributes"),
  cl::value_desc("a1,+a2,-a3,..."));

static cl::opt<bool>
UnwindInfo("unwind-info", cl::desc("Display unwind information"));

static cl::alias
UnwindInfoShort("u", cl::desc("Alias for --unwind-info"),
                cl::aliasopt(::UnwindInfo));

static StringRef ToolName;

static bool error(error_code ec) {
  if (!ec) return false;

  outs() << ToolName << ": error reading file: " << ec.message() << ".\n";
  outs().flush();
  return true;
}

static const Target *getTarget(const ObjectFile *Obj = NULL) {
  // Figure out the target triple.
  llvm::Triple TheTriple("unknown-unknown-unknown");
  if (TripleName.empty()) {
    if (Obj)
      TheTriple.setArch(Triple::ArchType(Obj->getArch()));
  } else
    TheTriple.setTriple(Triple::normalize(TripleName));

  // Get the target specific parser.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(ArchName, TheTriple,
                                                         Error);
  if (!TheTarget) {
    errs() << ToolName << ": " << Error;
    return 0;
  }

  // Update the triple name and return the found target.
  TripleName = TheTriple.getTriple();
  return TheTarget;
}

void llvm::StringRefMemoryObject::anchor() { }

void llvm::DumpBytes(StringRef bytes) {
  static const char hex_rep[] = "0123456789abcdef";
  // FIXME: The real way to do this is to figure out the longest instruction
  //        and align to that size before printing. I'll fix this when I get
  //        around to outputting relocations.
  // 15 is the longest x86 instruction
  // 3 is for the hex rep of a byte + a space.
  // 1 is for the null terminator.
  enum { OutputSize = (15 * 3) + 1 };
  char output[OutputSize];

  assert(bytes.size() <= 15
    && "DumpBytes only supports instructions of up to 15 bytes");
  memset(output, ' ', sizeof(output));
  unsigned index = 0;
  for (StringRef::iterator i = bytes.begin(),
                           e = bytes.end(); i != e; ++i) {
    output[index] = hex_rep[(*i & 0xF0) >> 4];
    output[index + 1] = hex_rep[*i & 0xF];
    index += 3;
  }

  output[sizeof(output) - 1] = 0;
  outs() << output;
}

static bool RelocAddressLess(RelocationRef a, RelocationRef b) {
  uint64_t a_addr, b_addr;
  if (error(a.getAddress(a_addr))) return false;
  if (error(b.getAddress(b_addr))) return false;
  return a_addr < b_addr;
}

static void DisassembleObject(const ObjectFile *Obj, bool InlineRelocs) {
  const Target *TheTarget = getTarget(Obj);
  // getTarget() will have already issued a diagnostic if necessary, so
  // just bail here if it failed.
  if (!TheTarget)
    return;

  // Package up features to be passed to target/subtarget
  std::string FeaturesStr;
  if (MAttrs.size()) {
    SubtargetFeatures Features;
    for (unsigned i = 0; i != MAttrs.size(); ++i)
      Features.AddFeature(MAttrs[i]);
    FeaturesStr = Features.getString();
  }

  error_code ec;
  for (section_iterator i = Obj->begin_sections(),
                        e = Obj->end_sections();
                        i != e; i.increment(ec)) {
    if (error(ec)) break;
    bool text;
    if (error(i->isText(text))) break;
    if (!text) continue;

    uint64_t SectionAddr;
    if (error(i->getAddress(SectionAddr))) break;

    // Make a list of all the symbols in this section.
    std::vector<std::pair<uint64_t, StringRef> > Symbols;
    for (symbol_iterator si = Obj->begin_symbols(),
                         se = Obj->end_symbols();
                         si != se; si.increment(ec)) {
      bool contains;
      if (!error(i->containsSymbol(*si, contains)) && contains) {
        uint64_t Address;
        if (error(si->getAddress(Address))) break;
        Address -= SectionAddr;

        StringRef Name;
        if (error(si->getName(Name))) break;
        Symbols.push_back(std::make_pair(Address, Name));
      }
    }

    // Sort the symbols by address, just in case they didn't come in that way.
    array_pod_sort(Symbols.begin(), Symbols.end());

    // Make a list of all the relocations for this section.
    std::vector<RelocationRef> Rels;
    if (InlineRelocs) {
      for (relocation_iterator ri = i->begin_relocations(),
                               re = i->end_relocations();
                               ri != re; ri.increment(ec)) {
        if (error(ec)) break;
        Rels.push_back(*ri);
      }
    }

    // Sort relocations by address.
    std::sort(Rels.begin(), Rels.end(), RelocAddressLess);

    StringRef name;
    if (error(i->getName(name))) break;
    outs() << "Disassembly of section " << name << ':';

    // If the section has no symbols just insert a dummy one and disassemble
    // the whole section.
    if (Symbols.empty())
      Symbols.push_back(std::make_pair(0, name));

    // Set up disassembler.
    OwningPtr<const MCAsmInfo> AsmInfo(TheTarget->createMCAsmInfo(TripleName));

    if (!AsmInfo) {
      errs() << "error: no assembly info for target " << TripleName << "\n";
      return;
    }

    OwningPtr<const MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TripleName, "", FeaturesStr));

    if (!STI) {
      errs() << "error: no subtarget info for target " << TripleName << "\n";
      return;
    }

    OwningPtr<const MCDisassembler> DisAsm(
      TheTarget->createMCDisassembler(*STI));
    if (!DisAsm) {
      errs() << "error: no disassembler for target " << TripleName << "\n";
      return;
    }

    OwningPtr<const MCRegisterInfo> MRI(TheTarget->createMCRegInfo(TripleName));
    if (!MRI) {
      errs() << "error: no register info for target " << TripleName << "\n";
      return;
    }

    OwningPtr<const MCInstrInfo> MII(TheTarget->createMCInstrInfo());
    if (!MII) {
      errs() << "error: no instruction info for target " << TripleName << "\n";
      return;
    }

    int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
    OwningPtr<MCInstPrinter> IP(TheTarget->createMCInstPrinter(
                                AsmPrinterVariant, *AsmInfo, *MII, *MRI, *STI));
    if (!IP) {
      errs() << "error: no instruction printer for target " << TripleName
             << '\n';
      return;
    }

    StringRef Bytes;
    if (error(i->getContents(Bytes))) break;
    StringRefMemoryObject memoryObject(Bytes);
    uint64_t Size;
    uint64_t Index;
    uint64_t SectSize;
    if (error(i->getSize(SectSize))) break;

    std::vector<RelocationRef>::const_iterator rel_cur = Rels.begin();
    std::vector<RelocationRef>::const_iterator rel_end = Rels.end();
    // Disassemble symbol by symbol.
    for (unsigned si = 0, se = Symbols.size(); si != se; ++si) {
      uint64_t Start = Symbols[si].first;
      uint64_t End;
      // The end is either the size of the section or the beginning of the next
      // symbol.
      if (si == se - 1)
        End = SectSize;
      // Make sure this symbol takes up space.
      else if (Symbols[si + 1].first != Start)
        End = Symbols[si + 1].first - 1;
      else
        // This symbol has the same address as the next symbol. Skip it.
        continue;

      outs() << '\n' << Symbols[si].second << ":\n";

#ifndef NDEBUG
        raw_ostream &DebugOut = DebugFlag ? dbgs() : nulls();
#else
        raw_ostream &DebugOut = nulls();
#endif

      for (Index = Start; Index < End; Index += Size) {
        MCInst Inst;

        if (DisAsm->getInstruction(Inst, Size, memoryObject, Index,
                                   DebugOut, nulls())) {
          outs() << format("%8" PRIx64 ":\t", SectionAddr + Index);
          DumpBytes(StringRef(Bytes.data() + Index, Size));
          IP->printInst(&Inst, outs(), "");
          outs() << "\n";
        } else {
          errs() << ToolName << ": warning: invalid instruction encoding\n";
          if (Size == 0)
            Size = 1; // skip illegible bytes
        }

        // Print relocation for instruction.
        while (rel_cur != rel_end) {
          bool hidden = false;
          uint64_t addr;
          SmallString<16> name;
          SmallString<32> val;

          // If this relocation is hidden, skip it.
          if (error(rel_cur->getHidden(hidden))) goto skip_print_rel;
          if (hidden) goto skip_print_rel;

          if (error(rel_cur->getAddress(addr))) goto skip_print_rel;
          // Stop when rel_cur's address is past the current instruction.
          if (addr >= Index + Size) break;
          if (error(rel_cur->getTypeName(name))) goto skip_print_rel;
          if (error(rel_cur->getValueString(val))) goto skip_print_rel;

          outs() << format("\t\t\t%8" PRIx64 ": ", SectionAddr + addr) << name
                 << "\t" << val << "\n";

        skip_print_rel:
          ++rel_cur;
        }
      }
    }
  }
}

static void PrintRelocations(const ObjectFile *o) {
  error_code ec;
  for (section_iterator si = o->begin_sections(), se = o->end_sections();
                                                  si != se; si.increment(ec)){
    if (error(ec)) return;
    if (si->begin_relocations() == si->end_relocations())
      continue;
    StringRef secname;
    if (error(si->getName(secname))) continue;
    outs() << "RELOCATION RECORDS FOR [" << secname << "]:\n";
    for (relocation_iterator ri = si->begin_relocations(),
                             re = si->end_relocations();
                             ri != re; ri.increment(ec)) {
      if (error(ec)) return;

      bool hidden;
      uint64_t address;
      SmallString<32> relocname;
      SmallString<32> valuestr;
      if (error(ri->getHidden(hidden))) continue;
      if (hidden) continue;
      if (error(ri->getTypeName(relocname))) continue;
      if (error(ri->getAddress(address))) continue;
      if (error(ri->getValueString(valuestr))) continue;
      outs() << address << " " << relocname << " " << valuestr << "\n";
    }
    outs() << "\n";
  }
}

static void PrintSectionHeaders(const ObjectFile *o) {
  outs() << "Sections:\n"
            "Idx Name          Size      Address          Type\n";
  error_code ec;
  unsigned i = 0;
  for (section_iterator si = o->begin_sections(), se = o->end_sections();
                                                  si != se; si.increment(ec)) {
    if (error(ec)) return;
    StringRef Name;
    if (error(si->getName(Name))) return;
    uint64_t Address;
    if (error(si->getAddress(Address))) return;
    uint64_t Size;
    if (error(si->getSize(Size))) return;
    bool Text, Data, BSS;
    if (error(si->isText(Text))) return;
    if (error(si->isData(Data))) return;
    if (error(si->isBSS(BSS))) return;
    std::string Type = (std::string(Text ? "TEXT " : "") +
                        (Data ? "DATA " : "") + (BSS ? "BSS" : ""));
    outs() << format("%3d %-13s %09" PRIx64 " %017" PRIx64 " %s\n",
                     i, Name.str().c_str(), Size, Address, Type.c_str());
    ++i;
  }
}

static void PrintSectionContents(const ObjectFile *o) {
  error_code ec;
  for (section_iterator si = o->begin_sections(),
                        se = o->end_sections();
                        si != se; si.increment(ec)) {
    if (error(ec)) return;
    StringRef Name;
    StringRef Contents;
    uint64_t BaseAddr;
    if (error(si->getName(Name))) continue;
    if (error(si->getContents(Contents))) continue;
    if (error(si->getAddress(BaseAddr))) continue;

    outs() << "Contents of section " << Name << ":\n";

    // Dump out the content as hex and printable ascii characters.
    for (std::size_t addr = 0, end = Contents.size(); addr < end; addr += 16) {
      outs() << format(" %04" PRIx64 " ", BaseAddr + addr);
      // Dump line of hex.
      for (std::size_t i = 0; i < 16; ++i) {
        if (i != 0 && i % 4 == 0)
          outs() << ' ';
        if (addr + i < end)
          outs() << hexdigit((Contents[addr + i] >> 4) & 0xF, true)
                 << hexdigit(Contents[addr + i] & 0xF, true);
        else
          outs() << "  ";
      }
      // Print ascii.
      outs() << "  ";
      for (std::size_t i = 0; i < 16 && addr + i < end; ++i) {
        if (std::isprint(Contents[addr + i] & 0xFF))
          outs() << Contents[addr + i];
        else
          outs() << ".";
      }
      outs() << "\n";
    }
  }
}

static const char* GetCOFFStringSymbol(const COFFObjectFile *coff,
                                       const coff_symbol *symbol) {
  StringRef name;
  if (error(coff->getSymbolName(symbol, name))) return 0;

  const char* str = 0;
  ArrayRef<uint8_t> contents;

  if (name.startswith("$SG")) {
    // cl-emmited global strings, dump it as a convenience
    const coff_section *sec = 0;
    if (!error(coff->getSection(symbol->SectionNumber, sec))) {
      coff->getSectionContents(sec, contents);
      str = (const char*) contents.data() + symbol->Value;
    }
  }
  
  return str;
}

static void PrintCOFFSymbolTable(const COFFObjectFile *coff) {
  const coff_file_header *header;
  if (error(coff->getHeader(header))) return;
  int aux_count = 0;
  const coff_symbol *symbol = 0;
  for (int i = 0, e = header->NumberOfSymbols; i != e; ++i) {
    if (aux_count--) {
      // Figure out which type of aux this is.
      if (symbol->StorageClass == COFF::IMAGE_SYM_CLASS_STATIC
          && symbol->Value == 0) { // Section definition.
        const coff_aux_section_definition *asd;
        if (error(coff->getAuxSymbol<coff_aux_section_definition>(i, asd)))
          return;
        outs() << "Aux |  "
            << format("Size: 0x%x | #Relocs: %d | #LineNums: %d | Checksum 0x%x "
                         , unsigned(asd->Length)
                         , unsigned(asd->NumberOfRelocations)
                         , unsigned(asd->NumberOfLinenumbers)
                         , unsigned(asd->CheckSum))
               << format("assoc %d comdat %d\n"
                         , unsigned(asd->Number)
                         , unsigned(asd->Selection));
      } else
        outs() << "  Aux. Section Unknown\n";
    } else {
      StringRef name;
      if (error(coff->getSymbol(i, symbol))) return;
      if (error(coff->getSymbolName(symbol, name))) return;

      const char* gstring = GetCOFFStringSymbol(coff, symbol);

      outs() << "#" << format("%2d", i) << " | "
             << " Section: " << format("%2d", int(symbol->SectionNumber))
             << " | Type: " << format("%3x", unsigned(symbol->Type))
             << " | StorageClass: " << format("%3x", unsigned(symbol->StorageClass))
             << " | #AuxSymbols: " << unsigned(symbol->NumberOfAuxSymbols)
             << " | Value: 0x" << format("%08x", unsigned(symbol->Value))
             << " | " << name;

      if (gstring) {
        std::string print(gstring);
        if (print.size() > 16) {
          print.resize(13);
          print.append("...");
        }
        outs() << " \"" << print << "\"";
      }

      outs() << "\n";

      aux_count = symbol->NumberOfAuxSymbols;
    }
  }
}

static void PrintSymbolTable(const ObjectFile *o) {
  outs() << "SYMBOL TABLE:\n";

  if (const COFFObjectFile *coff = dyn_cast<const COFFObjectFile>(o))
    PrintCOFFSymbolTable(coff);
  else {
    error_code ec;
    for (symbol_iterator si = o->begin_symbols(),
                         se = o->end_symbols(); si != se; si.increment(ec)) {
      if (error(ec)) return;
      StringRef Name;
      uint64_t Address;
      SymbolRef::Type Type;
      uint64_t Size;
      uint32_t Flags;
      section_iterator Section = o->end_sections();
      if (error(si->getName(Name))) continue;
      if (error(si->getAddress(Address))) continue;
      if (error(si->getFlags(Flags))) continue;
      if (error(si->getType(Type))) continue;
      if (error(si->getSize(Size))) continue;
      if (error(si->getSection(Section))) continue;

      bool Global = Flags & SymbolRef::SF_Global;
      bool Weak = Flags & SymbolRef::SF_Weak;
      bool Absolute = Flags & SymbolRef::SF_Absolute;

      if (Address == UnknownAddressOrSize)
        Address = 0;
      if (Size == UnknownAddressOrSize)
        Size = 0;
      char GlobLoc = ' ';
      if (Type != SymbolRef::ST_Unknown)
        GlobLoc = Global ? 'g' : 'l';
      char Debug = (Type == SymbolRef::ST_Debug || Type == SymbolRef::ST_File)
                   ? 'd' : ' ';
      char FileFunc = ' ';
      if (Type == SymbolRef::ST_File)
        FileFunc = 'f';
      else if (Type == SymbolRef::ST_Function)
        FileFunc = 'F';

      outs() << format("%08" PRIx64, Address) << " "
             << GlobLoc // Local -> 'l', Global -> 'g', Neither -> ' '
             << (Weak ? 'w' : ' ') // Weak?
             << ' ' // Constructor. Not supported yet.
             << ' ' // Warning. Not supported yet.
             << ' ' // Indirect reference to another symbol.
             << Debug // Debugging (d) or dynamic (D) symbol.
             << FileFunc // Name of function (F), file (f) or object (O).
             << ' ';
      if (Absolute)
        outs() << "*ABS*";
      else if (Section == o->end_sections())
        outs() << "*UND*";
      else {
        StringRef SectionName;
        if (error(Section->getName(SectionName)))
          SectionName = "";
        outs() << SectionName;
      }
      outs() << '\t'
             << format("%08" PRIx64 " ", Size)
             << Name
             << '\n';
    }
  }
}

namespace {
  using namespace llvm::Win64EH;
  StringRef GetCOFFUnwindCodeTypeName(uint8_t Code) {
    switch(Code) {
    case UOP_PushNonVol: return "UOP_PushNonVol";
    case UOP_AllocLarge: return "UOP_AllocLarge";
    case UOP_AllocSmall: return "UOP_AllocSmall";
    case UOP_SetFPReg: return "UOP_SetFPReg";
    case UOP_SaveNonVol: return "UOP_SaveNonVol";
    case UOP_SaveNonVolBig: return "UOP_SaveNonVolBig";
    case UOP_SaveXMM128: return "UOP_SaveXMM128";
    case UOP_SaveXMM128Big: return "UOP_SaveXMM128Big";
    case UOP_PushMachFrame: return "UOP_PushMachFrame";
    }
  }
}

namespace {
  llvm::raw_ostream &writeHexNumber(llvm::raw_ostream &Out, unsigned long long N) {
    if (N >= 10)
      Out << "0x";
    Out.write_hex(N);
    return Out;
  }

 bool IsCSpecificHandler(const COFFObjectFile* o, uint64_t offset) {
    error_code ec;
    for (symbol_iterator si = o->begin_symbols(),
                         se = o->end_symbols(); si != se; si.increment(ec)) {
      if (error(ec)) continue;

      StringRef Name;
      if (error(si->getName(Name))) continue;

#if 0
      uint64_t Addr;
      if (error(si->getFileOffset(Addr))) continue;

      if (Addr == offset)
#endif
      if (Name.compare("__C_specific_handler") == 0)
        return true;
    }

    return false;
  }

  // Used in standard 64-bit __try/__except/__finally
  // constructs.
  struct ScopeTableAMD64 {
    uint32_t Count;
    struct {
      uint32_t BeginAddress;
      uint32_t EndAddress;
      uint32_t HandlerAddress;
      uint32_t JumpTarget;
    } ScopeRecord[1];
  };
}


static void PrintCOFFUnwindInfo(const COFFObjectFile* o) {
  const coff_file_header *header;
  if (error(o->getHeader(header))) return;

  if (header->Machine != COFF::IMAGE_FILE_MACHINE_AMD64) {
    errs() << "Unsupported image machine type "
              "(currently only AMD64 is supported).\n";
    return;
  }

  const coff_section* pdata = 0, *xdata = 0;
  
  error_code ec;
  for (section_iterator si = o->begin_sections(),
                            se = o->end_sections();
                            si != se; si.increment(ec)) {
    if (error(ec)) return;
    
    StringRef Name;
    if (error(si->getName(Name))) continue;
    
    if (Name.compare(".pdata") == 0) {
      pdata = o->getCOFFSection(si);
      continue;
    }

    if (Name.compare(".xdata") == 0) {
      xdata = o->getCOFFSection(si);
      continue;
    }
  }

  if (!pdata) return;

  ArrayRef<uint8_t> Contents;
  if (error(o->getSectionContents(pdata, Contents))) return;
  if (Contents.empty()) return;

  ArrayRef<uint8_t> XContents;
  if (xdata)
    o->getSectionContents(xdata, XContents);

  unsigned i = 0;
  while ((Contents.size() - i) >= sizeof(RuntimeFunction)) {
    RuntimeFunction* RF = (RuntimeFunction*)(Contents.data() + i);

    outs() << "Function Table:\n";
    
    outs() << "  Start Address: ";
    writeHexNumber(outs(), RF->startAddress);
    outs() << "\n";

    outs() << "  End Address: ";
    writeHexNumber(outs(), RF->endAddress);
    outs() << "\n";

    outs() << "  Unwind Info Address: ";
    writeHexNumber(outs(), RF->unwindInfoOffset);
    outs() << "\n";

    if (RF->unwindInfoOffset >= XContents.size())
      continue;

    Win64EH::UnwindInfo* UI = (Win64EH::UnwindInfo*)
                         (XContents.data() + RF->unwindInfoOffset);

    outs() << "  Size of prolog: " << (int) UI->prologSize << "\n";

    if (UI->flags & Win64EH::UNW_ChainInfo) {
      outs() << "  Chained Unwind Info\n";
      continue;
    }

    if (UI->numCodes)
      outs() << "\n  Unwind Codes:";

    for (unsigned c = 0; c < UI->numCodes; ++c) {
      UnwindCode UC = UI->unwindCodes[c];
      outs() << "\n    " << GetCOFFUnwindCodeTypeName(UC.u.unwindOp) << "\n";
    }

    if (UI->flags & (UNW_ExceptionHandler | UNW_TerminateHandler)) {
      uint64_t offset = UI->getLanguageSpecificHandlerOffset();

      // Check if the handler is the SEH standard one.
      if (IsCSpecificHandler(o, offset)) {
        ScopeTableAMD64* ST = (ScopeTableAMD64*) UI->getExceptionData();
        if (ST->Count)
          outs() << "\n  Scope Table:\n";
        for (unsigned i = 0; i < ST->Count; ++i) {
            outs() << "    Begin: ";
            writeHexNumber(outs(), ST->ScopeRecord->BeginAddress);

            outs() << "\n    End: ";
            writeHexNumber(outs(), ST->ScopeRecord->EndAddress);

            outs() << "\n    Handler: ";
            writeHexNumber(outs(), ST->ScopeRecord->HandlerAddress);

            outs() << "\n    Jump Target: ";
            writeHexNumber(outs(), ST->ScopeRecord->JumpTarget);
            outs() << "\n";
        }
      }
    }

    outs() << "\n\n";

    i += sizeof(RuntimeFunction);
  }
}

static void PrintUnwindInfo(const ObjectFile *o) {
  outs() << "Unwind info:\n\n";
  outs().flush();

  if (const COFFObjectFile *coff = dyn_cast<const COFFObjectFile>(o)) {
    PrintCOFFUnwindInfo(coff);
  } else {
    // TODO: Extract DWARF dump tool to objdump.
    errs() << "This operation is only currently supported "
              "for COFF object files.\n";
    return;
  }
}

static void DumpObject(const ObjectFile *o) {
  outs() << '\n';
  outs() << o->getFileName()
         << ":\tfile format " << o->getFileFormatName() << "\n\n";

  if (Disassemble)
    DisassembleObject(o, Relocations);
  if (Relocations && !Disassemble)
    PrintRelocations(o);
  if (SectionHeaders)
    PrintSectionHeaders(o);
  if (SectionContents)
    PrintSectionContents(o);
  if (SymbolTable)
    PrintSymbolTable(o);
  if (::UnwindInfo)
    PrintUnwindInfo(o);
}

/// @brief Dump each object file in \a a;
static void DumpArchive(const Archive *a) {
  for (Archive::child_iterator i = a->begin_children(),
                               e = a->end_children(); i != e; ++i) {
    OwningPtr<Binary> child;
    if (error_code ec = i->getAsBinary(child)) {
      // Ignore non-object files.
      if (ec != object_error::invalid_file_type)
        errs() << ToolName << ": '" << a->getFileName() << "': " << ec.message()
               << ".\n";
      continue;
    }
    if (ObjectFile *o = dyn_cast<ObjectFile>(child.get()))
      DumpObject(o);
    else
      errs() << ToolName << ": '" << a->getFileName() << "': "
              << "Unrecognized file type.\n";
  }
}

/// @brief Open file and figure out how to dump it.
static void DumpInput(StringRef file) {
  // If file isn't stdin, check that it exists.
  if (file != "-" && !sys::fs::exists(file)) {
    errs() << ToolName << ": '" << file << "': " << "No such file\n";
    return;
  }

  if (MachO && Disassemble) {
    DisassembleInputMachO(file);
    return;
  }

  // Attempt to open the binary.
  OwningPtr<Binary> binary;
  if (error_code ec = createBinary(file, binary)) {
    errs() << ToolName << ": '" << file << "': " << ec.message() << ".\n";
    return;
  }

  if (Archive *a = dyn_cast<Archive>(binary.get()))
    DumpArchive(a);
  else if (ObjectFile *o = dyn_cast<ObjectFile>(binary.get()))
    DumpObject(o);
  else
    errs() << ToolName << ": '" << file << "': " << "Unrecognized file type.\n";
}

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  // Initialize targets and assembly printers/parsers.
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllDisassemblers();

  // Register the target printer for --version.
  cl::AddExtraVersionPrinter(TargetRegistry::printRegisteredTargetsForVersion);

  cl::ParseCommandLineOptions(argc, argv, "llvm object file dumper\n");
  TripleName = Triple::normalize(TripleName);

  ToolName = argv[0];

  // Defaults to a.out if no filenames specified.
  if (InputFilenames.size() == 0)
    InputFilenames.push_back("a.out");

  if (!Disassemble
      && !Relocations
      && !SectionHeaders
      && !SectionContents
      && !SymbolTable
      && !::UnwindInfo) {
    cl::PrintHelpMessage();
    return 2;
  }

  std::for_each(InputFilenames.begin(), InputFilenames.end(),
                DumpInput);

  return 0;
}
