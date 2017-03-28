//===--- BinaryContext.cpp  - Interface for machine-level context ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "llvm/ADT/Twine.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/CommandLine.h"


using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltCategory;

extern cl::opt<bool> Relocs;

static cl::opt<bool>
PrintDebugInfo("print-debug-info",
  cl::desc("print debug info when printing functions"),
  cl::Hidden,
  cl::cat(BoltCategory));

} // namespace opts

BinaryContext::~BinaryContext() { }

MCSymbol *BinaryContext::getOrCreateGlobalSymbol(uint64_t Address,
                                                 Twine Prefix) {
  MCSymbol *Symbol{nullptr};
  std::string Name;
  auto NI = GlobalAddresses.find(Address);
  if (NI != GlobalAddresses.end()) {
    // Even though there could be multiple names registered at the address,
    // we only use the first one.
    Name = NI->second;
  } else {
    Name = (Prefix + "0x" + Twine::utohexstr(Address)).str();
    assert(GlobalSymbols.find(Name) == GlobalSymbols.end() &&
           "created name is not unique");
    GlobalAddresses.emplace(std::make_pair(Address, Name));
  }

  Symbol = Ctx->lookupSymbol(Name);
  if (Symbol)
    return Symbol;

  Symbol = Ctx->getOrCreateSymbol(Name);
  GlobalSymbols[Name] = Address;

  return Symbol;
}

MCSymbol *BinaryContext::getGlobalSymbolAtAddress(uint64_t Address) const {
  auto NI = GlobalAddresses.find(Address);
  if (NI == GlobalAddresses.end())
    return nullptr;

  auto *Symbol = Ctx->lookupSymbol(NI->second);
  assert(Symbol && "symbol cannot be NULL at this point");

  return Symbol;
}

void BinaryContext::foldFunction(BinaryFunction &ChildBF,
                                 BinaryFunction &ParentBF,
                                 std::map<uint64_t, BinaryFunction> &BFs) {

  // Copy name list.
  ParentBF.addNewNames(ChildBF.getNames());

  // Update internal bookkeeping info.
  for (auto &Name : ChildBF.getNames()) {
    // Calls to functions are handled via symbols, and we keep the lookup table
    // that we need to update.
    auto *Symbol = Ctx->lookupSymbol(Name);
    assert(Symbol && "symbol cannot be NULL at this point");
    SymbolToFunctionMap[Symbol] = &ParentBF;

    // NB: there's no need to update GlobalAddresses and GlobalSymbols.
  }

  // Merge execution counts of ChildBF into those of ParentBF.
  ChildBF.mergeProfileDataInto(ParentBF);

  if (opts::Relocs) {
    // Remove ChildBF from the global set of functions in relocs mode.
    auto FI = BFs.find(ChildBF.getAddress());
    assert(FI != BFs.end() && "function not found");
    assert(&ChildBF == &FI->second && "function mismatch");
    FI = BFs.erase(FI);
  } else {
    // In non-relocation mode we keep the function, but rename it.
    std::string NewName = "__ICF_" + ChildBF.Names.back();
    ChildBF.Names.clear();
    ChildBF.Names.push_back(NewName);
    ChildBF.OutputSymbol = Ctx->getOrCreateSymbol(NewName);
    ChildBF.setFolded();
  }
}

void BinaryContext::printGlobalSymbols(raw_ostream& OS) const {
  for (auto &entry : GlobalSymbols) {
    OS << "(" << entry.first << " -> " << entry.second << ")\n";
  }
}

namespace {

/// Returns a binary function that contains a given address in the input
/// binary, or nullptr if none does.
BinaryFunction *getBinaryFunctionContainingAddress(
    uint64_t Address,
    std::map<uint64_t, BinaryFunction> &BinaryFunctions) {
  auto It = BinaryFunctions.upper_bound(Address);
  if (It != BinaryFunctions.begin()) {
    --It;
    if (It->first + It->second.getSize() > Address) {
      return &It->second;
    }
  }
  return nullptr;
}

// Traverses the DIE tree in a recursive depth-first search and finds lexical
// blocks and instances of inlined subroutines, saving them in
// AddressRangesObjects.
void findAddressRangesObjects(
    const DWARFCompileUnit *Unit,
    const DWARFDebugInfoEntryMinimal *DIE,
    std::map<uint64_t, BinaryFunction> &Functions,
    std::vector<llvm::bolt::AddressRangesDWARFObject> &AddressRangesObjects) {
  auto Tag = DIE->getTag();
  if (Tag == dwarf::DW_TAG_lexical_block ||
      Tag == dwarf::DW_TAG_inlined_subroutine ||
      Tag == dwarf::DW_TAG_try_block ||
      Tag == dwarf::DW_TAG_catch_block) {
    auto const &Ranges = DIE->getAddressRanges(Unit);
    if (!Ranges.empty()) {
      // We have to process all ranges, even for functions that we are not
      // updating. The primary reason is that abbrev entries are shared
      // and if we convert one DIE, it may affect the rest. Thus
      // the conservative approach that does not involve expanding
      // .debug_abbrev, is to switch all DIEs to use .debug_ranges, even if
      // they use a single [a,b) range. The secondary reason is that it allows
      // us to get rid of the original portion of .debug_ranges to save
      // space in the binary.
      auto Function = getBinaryFunctionContainingAddress(Ranges.front().first,
                                                         Functions);
      AddressRangesObjects.emplace_back(Unit, DIE);
      auto &Object = AddressRangesObjects.back();
      for (const auto &Range : Ranges) {
        if (Function && Function->isSimple()) {
          Object.addAddressRange(*Function, Range.first, Range.second);
        } else {
          Object.addAbsoluteRange(Range.first, Range.second);
        }
      }
    }
  }

  // Recursively visit each child.
  for (auto Child = DIE->getFirstChild(); Child; Child = Child->getSibling()) {
    findAddressRangesObjects(Unit, Child, Functions, AddressRangesObjects);
  }
}

/// Recursively finds DWARF DW_TAG_subprogram DIEs and match them with
/// BinaryFunctions. Record DIEs for unknown subprograms (mostly functions that
/// are never called and removed from the binary) in Unknown.
void findSubprograms(DWARFCompileUnit *Unit,
                     const DWARFDebugInfoEntryMinimal *DIE,
                     std::map<uint64_t, BinaryFunction> &BinaryFunctions,
                     BinaryContext::DIECompileUnitVector &Unknown) {
  if (DIE->isSubprogramDIE()) {
    // TODO: handle DW_AT_ranges.
    uint64_t LowPC, HighPC;
    if (DIE->getLowAndHighPC(Unit, LowPC, HighPC)) {
      auto It = BinaryFunctions.find(LowPC);
      if (It != BinaryFunctions.end()) {
        It->second.addSubprogramDIE(Unit, DIE);
      } else {
        Unknown.emplace_back(DIE, Unit);
      }
    }
  }

  for (auto ChildDIE = DIE->getFirstChild();
       ChildDIE != nullptr && !ChildDIE->isNULL();
       ChildDIE = ChildDIE->getSibling()) {
    findSubprograms(Unit, ChildDIE, BinaryFunctions, Unknown);
  }
}

} // namespace

unsigned BinaryContext::addDebugFilenameToUnit(const uint32_t DestCUID,
                                               const uint32_t SrcCUID,
                                               unsigned FileIndex) {
  auto SrcUnit = DwCtx->getCompileUnitForOffset(SrcCUID);
  auto LineTable = DwCtx->getLineTableForUnit(SrcUnit);
  const auto &FileNames = LineTable->Prologue.FileNames;
  // Dir indexes start at 1, as DWARF file numbers, and a dir index 0
  // means empty dir.
  assert(FileIndex > 0 && FileIndex <= FileNames.size() &&
         "FileIndex out of range for the compilation unit.");
  const char *Dir = FileNames[FileIndex - 1].DirIdx ?
    LineTable->Prologue.IncludeDirectories[FileNames[FileIndex - 1].DirIdx - 1] :
    "";
  return Ctx->getDwarfFile(Dir, FileNames[FileIndex - 1].Name, 0, DestCUID);
}

void BinaryContext::preprocessDebugInfo(
    std::map<uint64_t, BinaryFunction> &BinaryFunctions) {
  // Populate MCContext with DWARF files.
  for (const auto &CU : DwCtx->compile_units()) {
    const auto CUID = CU->getOffset();
    auto LineTable = DwCtx->getLineTableForUnit(CU.get());
    const auto &FileNames = LineTable->Prologue.FileNames;
    for (size_t I = 0, Size = FileNames.size(); I != Size; ++I) {
      // Dir indexes start at 1, as DWARF file numbers, and a dir index 0
      // means empty dir.
      const char *Dir = FileNames[I].DirIdx ?
          LineTable->Prologue.IncludeDirectories[FileNames[I].DirIdx - 1] :
          "";
      Ctx->getDwarfFile(Dir, FileNames[I].Name, 0, CUID);
    }
  }

  // For each CU, iterate over its children DIEs and match subprogram DIEs to
  // BinaryFunctions.
  for (auto &CU : DwCtx->compile_units()) {
    findSubprograms(CU.get(), CU->getUnitDIE(false), BinaryFunctions,
                    UnknownFunctions);
  }

  // Some functions may not have a corresponding subprogram DIE
  // yet they will be included in some CU and will have line number information.
  // Hence we need to associate them with the CU and include in CU ranges.
  for (auto &AddrFunctionPair : BinaryFunctions) {
    auto FunctionAddress = AddrFunctionPair.first;
    auto &Function = AddrFunctionPair.second;
    if (!Function.getSubprogramDIEs().empty())
      continue;
    if (auto DebugAranges = DwCtx->getDebugAranges()) {
      auto CUOffset = DebugAranges->findAddress(FunctionAddress);
      if (CUOffset != -1U) {
        Function.addSubprogramDIE(DwCtx->getCompileUnitForOffset(CUOffset),
                                  nullptr);
        continue;
      }
    }

#ifdef DWARF_LOOKUP_ALL_RANGES
    // Last resort - iterate over all compile units. This should not happen
    // very often. If it does, we need to create a separate lookup table
    // similar to .debug_aranges internally. This slows down processing
    // considerably.
    for (const auto &CU : DwCtx->compile_units()) {
      const auto *CUDie = CU->getUnitDIE();
      for (const auto &Range : CUDie->getAddressRanges(CU.get())) {
        if (FunctionAddress >= Range.first &&
            FunctionAddress < Range.second) {
          Function.addSubprogramDIE(CU.get(), nullptr);
          break;
        }
      }
    }
#endif
  }
}

void BinaryContext::preprocessFunctionDebugInfo(
    std::map<uint64_t, BinaryFunction> &BinaryFunctions) {
  // Iterate over DIE trees finding objects that contain address ranges.
  for (const auto &CU : DwCtx->compile_units()) {
    findAddressRangesObjects(CU.get(), CU->getUnitDIE(false), BinaryFunctions,
                             AddressRangesObjects);
  }

  // Iterate over location lists and save them in LocationLists.
  auto DebugLoc = DwCtx->getDebugLoc();
  for (const auto &DebugLocEntry : DebugLoc->getLocationLists()) {
    if (DebugLocEntry.Entries.empty())
      continue;
    auto StartAddress = DebugLocEntry.Entries.front().Begin;
    auto *Function = getBinaryFunctionContainingAddress(StartAddress,
                                                        BinaryFunctions);
    if (!Function || !Function->isSimple())
      continue;
    LocationLists.emplace_back(DebugLocEntry.Offset);
    auto &LocationList = LocationLists.back();
    for (const auto &Location : DebugLocEntry.Entries) {
      LocationList.addLocation(&Location.Loc, *Function, Location.Begin,
                               Location.End);
    }
  }
}

void BinaryContext::printCFI(raw_ostream &OS, uint32_t Operation) {
  switch(Operation) {
    case MCCFIInstruction::OpSameValue:        OS << "OpSameValue";       break;
    case MCCFIInstruction::OpRememberState:    OS << "OpRememberState";   break;
    case MCCFIInstruction::OpRestoreState:     OS << "OpRestoreState";    break;
    case MCCFIInstruction::OpOffset:           OS << "OpOffset";          break;
    case MCCFIInstruction::OpDefCfaRegister:   OS << "OpDefCfaRegister";  break;
    case MCCFIInstruction::OpDefCfaOffset:     OS << "OpDefCfaOffset";    break;
    case MCCFIInstruction::OpDefCfa:           OS << "OpDefCfa";          break;
    case MCCFIInstruction::OpRelOffset:        OS << "OpRelOffset";       break;
    case MCCFIInstruction::OpAdjustCfaOffset:  OS << "OfAdjustCfaOffset"; break;
    case MCCFIInstruction::OpEscape:           OS << "OpEscape";          break;
    case MCCFIInstruction::OpRestore:          OS << "OpRestore";         break;
    case MCCFIInstruction::OpUndefined:        OS << "OpUndefined";       break;
    case MCCFIInstruction::OpRegister:         OS << "OpRegister";        break;
    case MCCFIInstruction::OpWindowSave:       OS << "OpWindowSave";      break;
    case MCCFIInstruction::OpGnuArgsSize:      OS << "OpGnuArgsSize";     break;
    default:                                   OS << "Op#" << Operation; break;
  }
}

void BinaryContext::printInstruction(raw_ostream &OS,
                                     const MCInst &Instruction,
                                     uint64_t Offset,
                                     const BinaryFunction* Function,
                                     bool printMCInst) const {
  if (MIA->isEHLabel(Instruction)) {
    OS << "  EH_LABEL: " << *MIA->getTargetSymbol(Instruction) << '\n';
    return;
  }
  OS << format("    %08" PRIx64 ": ", Offset);
  if (MIA->isCFI(Instruction)) {
    uint32_t Offset = Instruction.getOperand(0).getImm();
    OS << "\t!CFI\t$" << Offset << "\t; ";
    if (Function)
      printCFI(OS, Function->getCFIFor(Instruction)->getOperation());
    OS << "\n";
    return;
  }
  InstPrinter->printInst(&Instruction, OS, "", *STI);
  if (MIA->isCall(Instruction)) {
    if (MIA->isTailCall(Instruction))
      OS << " # TAILCALL ";
    if (MIA->isInvoke(Instruction)) {
      const MCSymbol *LP;
      uint64_t Action;
      std::tie(LP, Action) = MIA->getEHInfo(Instruction);
      OS << " # handler: ";
      if (LP)
        OS << *LP;
      else
        OS << '0';
      OS << "; action: " << Action;
      auto GnuArgsSize = MIA->getGnuArgsSize(Instruction);
      if (GnuArgsSize >= 0)
        OS << "; GNU_args_size = " << GnuArgsSize;
    }
  }
  if (MIA->isIndirectBranch(Instruction)) {
    if (auto JTAddress = MIA->getJumpTable(Instruction)) {
      OS << " # JUMPTABLE @0x" << Twine::utohexstr(JTAddress);
    }
  }

  MIA->forEachAnnotation(
    Instruction,
    [&OS](const MCAnnotation *Annotation) {
      OS << " # " << Annotation->getName() << ": ";
      Annotation->print(OS);
    }
  );

  const DWARFDebugLine::LineTable *LineTable =
    Function && opts::PrintDebugInfo ? Function->getDWARFUnitLineTable().second
                                     : nullptr;

  if (LineTable) {
    auto RowRef = DebugLineTableRowRef::fromSMLoc(Instruction.getLoc());

    if (RowRef != DebugLineTableRowRef::NULL_ROW) {
      const auto &Row = LineTable->Rows[RowRef.RowIndex - 1];
      OS << " # debug line "
         << LineTable->Prologue.FileNames[Row.File - 1].Name
         << ":" << Row.Line;

      if (Row.Column) {
        OS << ":" << Row.Column;
      }
    }
  }

  OS << "\n";

  if (printMCInst) {
    Instruction.dump_pretty(OS, InstPrinter.get());
    OS << "\n";
  }
}

ErrorOr<SectionRef> BinaryContext::getSectionForAddress(uint64_t Address) const{
  auto SI = AllocatableSections.upper_bound(Address);
  if (SI != AllocatableSections.begin()) {
    --SI;
    if (SI->first + SI->second.getSize() > Address)
      return SI->second;
  }
  return std::make_error_code(std::errc::bad_address);
}

void BinaryContext::addSectionRelocation(SectionRef Section, uint64_t Offset,
                                         MCSymbol *Symbol, uint64_t Type,
                                         uint64_t Addend) {
  auto RI = SectionRelocations.find(Section);
  if (RI == SectionRelocations.end()) {
    auto Result =
      SectionRelocations.emplace(Section, std::set<Relocation>());
    RI = Result.first;
  }
  RI->second.emplace(Relocation{Offset, Symbol, Type, Addend, 0});
}

void BinaryContext::addRelocation(uint64_t Address, MCSymbol *Symbol,
                                  uint64_t Type, uint64_t Addend) {
  auto ContainingSection = getSectionForAddress(Address);
  assert(ContainingSection && "cannot find section for address");
  addSectionRelocation(*ContainingSection,
                       Address - ContainingSection->getAddress(),
                       Symbol,
                       Type,
                       Addend);
}

void BinaryContext::removeRelocationAt(uint64_t Address) {
  auto ContainingSection = getSectionForAddress(Address);
  assert(ContainingSection && "cannot find section for address");
  auto RI = SectionRelocations.find(*ContainingSection);
  if (RI == SectionRelocations.end())
    return;

  auto &Relocations = RI->second;
  auto RelocI = Relocations.find(
            Relocation{Address - ContainingSection->getAddress(), 0, 0, 0, 0});
  if (RelocI == Relocations.end())
    return;

  Relocations.erase(RelocI);
}

size_t Relocation::getSizeForType(uint64_t Type) {
  switch (Type) {
  default:
    llvm_unreachable("unsupported relocation type");
  case ELF::R_X86_64_PC8:
    return 1;
  case ELF::R_X86_64_PLT32:
  case ELF::R_X86_64_PC32:
  case ELF::R_X86_64_32S:
  case ELF::R_X86_64_32:
  case ELF::R_X86_64_GOTPCREL:
  case ELF::R_X86_64_GOTTPOFF:
  case ELF::R_X86_64_TPOFF32:
  case ELF::R_X86_64_GOTPCRELX:
  case ELF::R_X86_64_REX_GOTPCRELX:
    return 4;
  case ELF::R_X86_64_PC64:
  case ELF::R_X86_64_64:
    return 8;
  }
}

bool Relocation::isPCRelative(uint64_t Type) {
  switch (Type) {
  default:
    llvm_unreachable("Unknown relocation type");

  case ELF::R_X86_64_64:
  case ELF::R_X86_64_32:
  case ELF::R_X86_64_32S:
  case ELF::R_X86_64_TPOFF32:
    return false;

  case ELF::R_X86_64_PC8:
  case ELF::R_X86_64_PC32:
  case ELF::R_X86_64_GOTPCREL:
  case ELF::R_X86_64_PLT32:
  case ELF::R_X86_64_GOTTPOFF:
  case ELF::R_X86_64_GOTPCRELX:
  case ELF::R_X86_64_REX_GOTPCRELX:
    return true;
  }
}

size_t Relocation::emit(MCStreamer *Streamer) const {
  const auto Size = getSizeForType(Type);
  auto &Ctx = Streamer->getContext();
  if (isPCRelative(Type)) {
    auto *TempLabel = Ctx.createTempSymbol();
    Streamer->EmitLabel(TempLabel);
    auto Value =
      MCBinaryExpr::createSub(MCSymbolRefExpr::create(Symbol, Ctx),
                              MCSymbolRefExpr::create(TempLabel, Ctx),
                              Ctx);
    if (Addend) {
      Value = MCBinaryExpr::createAdd(Value,
                                      MCConstantExpr::create(Addend, Ctx),
                                      Ctx);
    }
    Streamer->EmitValue(Value, Size);
  } else {
    Streamer->EmitSymbolValue(Symbol, Size);
  }
  return Size;
}
