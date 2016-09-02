//=-- CoverageMapping.cpp - Code coverage mapping support ---------*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains support for clang's and llvm's instrumentation based
// code coverage.
//
//===----------------------------------------------------------------------===//

#include "llvm/ProfileData/CoverageMapping.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ProfileData/CoverageMappingReader.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace coverage;

#define DEBUG_TYPE "coverage-mapping"

Counter CounterExpressionBuilder::get(const CounterExpression &E) {
  auto It = ExpressionIndices.find(E);
  if (It != ExpressionIndices.end())
    return Counter::getExpression(It->second);
  unsigned I = Expressions.size();
  Expressions.push_back(E);
  ExpressionIndices[E] = I;
  return Counter::getExpression(I);
}

void CounterExpressionBuilder::extractTerms(
    Counter C, int Sign, SmallVectorImpl<std::pair<unsigned, int>> &Terms) {
  switch (C.getKind()) {
  case Counter::Zero:
    break;
  case Counter::CounterValueReference:
    Terms.push_back(std::make_pair(C.getCounterID(), Sign));
    break;
  case Counter::Expression:
    const auto &E = Expressions[C.getExpressionID()];
    extractTerms(E.LHS, Sign, Terms);
    extractTerms(E.RHS, E.Kind == CounterExpression::Subtract ? -Sign : Sign,
                 Terms);
    break;
  }
}

Counter CounterExpressionBuilder::simplify(Counter ExpressionTree) {
  // Gather constant terms.
  llvm::SmallVector<std::pair<unsigned, int>, 32> Terms;
  extractTerms(ExpressionTree, +1, Terms);

  // If there are no terms, this is just a zero. The algorithm below assumes at
  // least one term.
  if (Terms.size() == 0)
    return Counter::getZero();

  // Group the terms by counter ID.
  std::sort(Terms.begin(), Terms.end(),
            [](const std::pair<unsigned, int> &LHS,
               const std::pair<unsigned, int> &RHS) {
    return LHS.first < RHS.first;
  });

  // Combine terms by counter ID to eliminate counters that sum to zero.
  auto Prev = Terms.begin();
  for (auto I = Prev + 1, E = Terms.end(); I != E; ++I) {
    if (I->first == Prev->first) {
      Prev->second += I->second;
      continue;
    }
    ++Prev;
    *Prev = *I;
  }
  Terms.erase(++Prev, Terms.end());

  Counter C;
  // Create additions. We do this before subtractions to avoid constructs like
  // ((0 - X) + Y), as opposed to (Y - X).
  for (auto Term : Terms) {
    if (Term.second <= 0)
      continue;
    for (int I = 0; I < Term.second; ++I)
      if (C.isZero())
        C = Counter::getCounter(Term.first);
      else
        C = get(CounterExpression(CounterExpression::Add, C,
                                  Counter::getCounter(Term.first)));
  }

  // Create subtractions.
  for (auto Term : Terms) {
    if (Term.second >= 0)
      continue;
    for (int I = 0; I < -Term.second; ++I)
      C = get(CounterExpression(CounterExpression::Subtract, C,
                                Counter::getCounter(Term.first)));
  }
  return C;
}

Counter CounterExpressionBuilder::add(Counter LHS, Counter RHS) {
  return simplify(get(CounterExpression(CounterExpression::Add, LHS, RHS)));
}

Counter CounterExpressionBuilder::subtract(Counter LHS, Counter RHS) {
  return simplify(
      get(CounterExpression(CounterExpression::Subtract, LHS, RHS)));
}

void CounterMappingContext::dump(const Counter &C,
                                 llvm::raw_ostream &OS) const {
  switch (C.getKind()) {
  case Counter::Zero:
    OS << '0';
    return;
  case Counter::CounterValueReference:
    OS << '#' << C.getCounterID();
    break;
  case Counter::Expression: {
    if (C.getExpressionID() >= Expressions.size())
      return;
    const auto &E = Expressions[C.getExpressionID()];
    OS << '(';
    dump(E.LHS, OS);
    OS << (E.Kind == CounterExpression::Subtract ? " - " : " + ");
    dump(E.RHS, OS);
    OS << ')';
    break;
  }
  }
  if (CounterValues.empty())
    return;
  ErrorOr<int64_t> Value = evaluate(C);
  if (!Value)
    return;
  OS << '[' << *Value << ']';
}

ErrorOr<int64_t> CounterMappingContext::evaluate(const Counter &C) const {
  switch (C.getKind()) {
  case Counter::Zero:
    return 0;
  case Counter::CounterValueReference:
    if (C.getCounterID() >= CounterValues.size())
      return std::make_error_code(std::errc::argument_out_of_domain);
    return CounterValues[C.getCounterID()];
  case Counter::Expression: {
    if (C.getExpressionID() >= Expressions.size())
      return std::make_error_code(std::errc::argument_out_of_domain);
    const auto &E = Expressions[C.getExpressionID()];
    ErrorOr<int64_t> LHS = evaluate(E.LHS);
    if (!LHS)
      return LHS;
    ErrorOr<int64_t> RHS = evaluate(E.RHS);
    if (!RHS)
      return RHS;
    return E.Kind == CounterExpression::Subtract ? *LHS - *RHS : *LHS + *RHS;
  }
  }
  llvm_unreachable("Unhandled CounterKind");
}

void FunctionRecordIterator::skipOtherFiles() {
  while (Current != Records.end() && !Filename.empty() &&
         Filename != Current->Filenames[0])
    ++Current;
  if (Current == Records.end())
    *this = FunctionRecordIterator();
}

/// Get the function name from the record, removing the filename prefix if
/// necessary.
static StringRef getFuncNameWithoutPrefix(const CoverageMappingRecord &Record) {
  StringRef FunctionName = Record.FunctionName;
  if (Record.Filenames.empty())
    return FunctionName;
  StringRef Filename = sys::path::filename(Record.Filenames[0]);
  if (FunctionName.startswith(Filename))
    FunctionName = FunctionName.drop_front(Filename.size() + 1);
  return FunctionName;
}

ErrorOr<std::unique_ptr<CoverageMapping>>
CoverageMapping::load(CoverageMappingReader &CoverageReader,
                      IndexedInstrProfReader &ProfileReader) {
  auto Coverage = std::unique_ptr<CoverageMapping>(new CoverageMapping());

  std::vector<uint64_t> Counts;
  for (const auto &Record : CoverageReader) {
    CounterMappingContext Ctx(Record.Expressions);

    Counts.clear();
    if (std::error_code EC = ProfileReader.getFunctionCounts(
            Record.FunctionName, Record.FunctionHash, Counts)) {
      if (EC == instrprof_error::hash_mismatch) {
        Coverage->MismatchedFunctionCount++;
        continue;
      } else if (EC != instrprof_error::unknown_function)
        return EC;
      Counts.assign(Record.MappingRegions.size(), 0);
    }
    Ctx.setCounts(Counts);

    assert(!Record.MappingRegions.empty() && "Function has no regions");

    FunctionRecord Function(getFuncNameWithoutPrefix(Record), Record.Filenames);
    for (const auto &Region : Record.MappingRegions) {
      ErrorOr<int64_t> ExecutionCount = Ctx.evaluate(Region.Count);
      if (!ExecutionCount)
        break;
      Function.pushRegion(Region, *ExecutionCount);
    }
    if (Function.CountedRegions.size() != Record.MappingRegions.size()) {
      Coverage->MismatchedFunctionCount++;
      continue;
    }

    Coverage->Functions.push_back(std::move(Function));
  }

  return std::move(Coverage);
}

ErrorOr<std::unique_ptr<CoverageMapping>>
CoverageMapping::load(StringRef ObjectFilename, StringRef ProfileFilename,
                      StringRef Arch) {
  auto CounterMappingBuff = MemoryBuffer::getFileOrSTDIN(ObjectFilename);
  if (std::error_code EC = CounterMappingBuff.getError())
    return EC;
  auto CoverageReaderOrErr =
      BinaryCoverageReader::create(CounterMappingBuff.get(), Arch);
  if (std::error_code EC = CoverageReaderOrErr.getError())
    return EC;
  auto CoverageReader = std::move(CoverageReaderOrErr.get());
  auto ProfileReaderOrErr = IndexedInstrProfReader::create(ProfileFilename);
  if (auto EC = ProfileReaderOrErr.getError())
    return EC;
  auto ProfileReader = std::move(ProfileReaderOrErr.get());
  return load(*CoverageReader, *ProfileReader);
}

namespace {
/// \brief Distributes functions into instantiation sets.
///
/// An instantiation set is a collection of functions that have the same source
/// code, ie, template functions specializations.
class FunctionInstantiationSetCollector {
  typedef DenseMap<std::pair<unsigned, unsigned>,
                   std::vector<const FunctionRecord *>> MapT;
  MapT InstantiatedFunctions;

public:
  void insert(const FunctionRecord &Function, unsigned FileID) {
    auto I = Function.CountedRegions.begin(), E = Function.CountedRegions.end();
    while (I != E && I->FileID != FileID)
      ++I;
    assert(I != E && "function does not cover the given file");
    auto &Functions = InstantiatedFunctions[I->startLoc()];
    Functions.push_back(&Function);
  }

  MapT::iterator begin() { return InstantiatedFunctions.begin(); }

  MapT::iterator end() { return InstantiatedFunctions.end(); }
};

class SegmentBuilder {
  std::vector<CoverageSegment> Segments;
  SmallVector<const CountedRegion *, 8> ActiveRegions;

  /// Start a segment with no count specified.
  void startSegment(unsigned Line, unsigned Col) {
    DEBUG(dbgs() << "Top level segment at " << Line << ":" << Col << "\n");
    Segments.emplace_back(Line, Col, /*IsRegionEntry=*/false);
  }

  /// Start a segment with the given Region's count.
  void startSegment(unsigned Line, unsigned Col, bool IsRegionEntry,
                    const CountedRegion &Region) {
    if (Segments.empty())
      Segments.emplace_back(Line, Col, IsRegionEntry);
    CoverageSegment S = Segments.back();
    // Avoid creating empty regions.
    if (S.Line != Line || S.Col != Col) {
      Segments.emplace_back(Line, Col, IsRegionEntry);
      S = Segments.back();
    }
    DEBUG(dbgs() << "Segment at " << Line << ":" << Col);
    // Set this region's count.
    if (Region.Kind != coverage::CounterMappingRegion::SkippedRegion) {
      DEBUG(dbgs() << " with count " << Region.ExecutionCount);
      Segments.back().setCount(Region.ExecutionCount);
    }
    DEBUG(dbgs() << "\n");
  }

  /// Start a segment for the given region.
  void startSegment(const CountedRegion &Region) {
    startSegment(Region.LineStart, Region.ColumnStart, true, Region);
  }

  /// Pop the top region off of the active stack, starting a new segment with
  /// the containing Region's count.
  void popRegion() {
    const CountedRegion *Active = ActiveRegions.back();
    unsigned Line = Active->LineEnd, Col = Active->ColumnEnd;
    ActiveRegions.pop_back();
    if (ActiveRegions.empty())
      startSegment(Line, Col);
    else
      startSegment(Line, Col, false, *ActiveRegions.back());
  }

public:
  /// Build a list of CoverageSegments from a sorted list of Regions.
  std::vector<CoverageSegment> buildSegments(ArrayRef<CountedRegion> Regions) {
    const CountedRegion *PrevRegion = nullptr;
    for (const auto &Region : Regions) {
      // Pop any regions that end before this one starts.
      while (!ActiveRegions.empty() &&
             ActiveRegions.back()->endLoc() <= Region.startLoc())
        popRegion();
      if (PrevRegion && PrevRegion->startLoc() == Region.startLoc() &&
          PrevRegion->endLoc() == Region.endLoc()) {
        if (Region.Kind == coverage::CounterMappingRegion::CodeRegion)
          Segments.back().addCount(Region.ExecutionCount);
      } else {
        // Add this region to the stack.
        ActiveRegions.push_back(&Region);
        startSegment(Region);
      }
      PrevRegion = &Region;
    }
    // Pop any regions that are left in the stack.
    while (!ActiveRegions.empty())
      popRegion();
    return Segments;
  }
};
}

std::vector<StringRef> CoverageMapping::getUniqueSourceFiles() const {
  std::vector<StringRef> Filenames;
  for (const auto &Function : getCoveredFunctions())
    for (const auto &Filename : Function.Filenames)
      Filenames.push_back(Filename);
  std::sort(Filenames.begin(), Filenames.end());
  auto Last = std::unique(Filenames.begin(), Filenames.end());
  Filenames.erase(Last, Filenames.end());
  return Filenames;
}

static Optional<unsigned> findMainViewFileID(StringRef SourceFile,
                                             const FunctionRecord &Function) {
  llvm::SmallVector<bool, 8> IsExpandedFile(Function.Filenames.size(), false);
  llvm::SmallVector<bool, 8> FilenameEquivalence(Function.Filenames.size(),
                                                 false);
  for (unsigned I = 0, E = Function.Filenames.size(); I < E; ++I)
    if (SourceFile == Function.Filenames[I])
      FilenameEquivalence[I] = true;
  for (const auto &CR : Function.CountedRegions)
    if (CR.Kind == CounterMappingRegion::ExpansionRegion &&
        FilenameEquivalence[CR.FileID])
      IsExpandedFile[CR.ExpandedFileID] = true;
  for (unsigned I = 0, E = Function.Filenames.size(); I < E; ++I)
    if (FilenameEquivalence[I] && !IsExpandedFile[I])
      return I;
  return None;
}

static Optional<unsigned> findMainViewFileID(const FunctionRecord &Function) {
  llvm::SmallVector<bool, 8> IsExpandedFile(Function.Filenames.size(), false);
  for (const auto &CR : Function.CountedRegions)
    if (CR.Kind == CounterMappingRegion::ExpansionRegion)
      IsExpandedFile[CR.ExpandedFileID] = true;
  for (unsigned I = 0, E = Function.Filenames.size(); I < E; ++I)
    if (!IsExpandedFile[I])
      return I;
  return None;
}

static SmallSet<unsigned, 8> gatherFileIDs(StringRef SourceFile,
                                           const FunctionRecord &Function) {
  SmallSet<unsigned, 8> IDs;
  for (unsigned I = 0, E = Function.Filenames.size(); I < E; ++I)
    if (SourceFile == Function.Filenames[I])
      IDs.insert(I);
  return IDs;
}

/// Sort a nested sequence of regions from a single file.
template <class It> static void sortNestedRegions(It First, It Last) {
  std::sort(First, Last,
            [](const CountedRegion &LHS, const CountedRegion &RHS) {
    if (LHS.startLoc() == RHS.startLoc())
      // When LHS completely contains RHS, we sort LHS first.
      return RHS.endLoc() < LHS.endLoc();
    return LHS.startLoc() < RHS.startLoc();
  });
}

static bool isExpansion(const CountedRegion &R, unsigned FileID) {
  return R.Kind == CounterMappingRegion::ExpansionRegion && R.FileID == FileID;
}

CoverageData CoverageMapping::getCoverageForFile(StringRef Filename) {
  CoverageData FileCoverage(Filename);
  std::vector<coverage::CountedRegion> Regions;

  for (const auto &Function : Functions) {
    auto MainFileID = findMainViewFileID(Filename, Function);
    if (!MainFileID)
      continue;
    auto FileIDs = gatherFileIDs(Filename, Function);
    for (const auto &CR : Function.CountedRegions)
      if (FileIDs.count(CR.FileID)) {
        Regions.push_back(CR);
        if (isExpansion(CR, *MainFileID))
          FileCoverage.Expansions.emplace_back(CR, Function);
      }
  }

  sortNestedRegions(Regions.begin(), Regions.end());
  DEBUG(dbgs() << "Emitting segments for file: " << Filename << "\n");
  FileCoverage.Segments = SegmentBuilder().buildSegments(Regions);

  return FileCoverage;
}

std::vector<const FunctionRecord *>
CoverageMapping::getInstantiations(StringRef Filename) {
  FunctionInstantiationSetCollector InstantiationSetCollector;
  for (const auto &Function : Functions) {
    auto MainFileID = findMainViewFileID(Filename, Function);
    if (!MainFileID)
      continue;
    InstantiationSetCollector.insert(Function, *MainFileID);
  }

  std::vector<const FunctionRecord *> Result;
  for (const auto &InstantiationSet : InstantiationSetCollector) {
    if (InstantiationSet.second.size() < 2)
      continue;
    for (auto Function : InstantiationSet.second)
      Result.push_back(Function);
  }
  return Result;
}

CoverageData
CoverageMapping::getCoverageForFunction(const FunctionRecord &Function) {
  auto MainFileID = findMainViewFileID(Function);
  if (!MainFileID)
    return CoverageData();

  CoverageData FunctionCoverage(Function.Filenames[*MainFileID]);
  std::vector<coverage::CountedRegion> Regions;
  for (const auto &CR : Function.CountedRegions)
    if (CR.FileID == *MainFileID) {
      Regions.push_back(CR);
      if (isExpansion(CR, *MainFileID))
        FunctionCoverage.Expansions.emplace_back(CR, Function);
    }

  sortNestedRegions(Regions.begin(), Regions.end());
  DEBUG(dbgs() << "Emitting segments for function: " << Function.Name << "\n");
  FunctionCoverage.Segments = SegmentBuilder().buildSegments(Regions);

  return FunctionCoverage;
}

CoverageData
CoverageMapping::getCoverageForExpansion(const ExpansionRecord &Expansion) {
  CoverageData ExpansionCoverage(
      Expansion.Function.Filenames[Expansion.FileID]);
  std::vector<coverage::CountedRegion> Regions;
  for (const auto &CR : Expansion.Function.CountedRegions)
    if (CR.FileID == Expansion.FileID) {
      Regions.push_back(CR);
      if (isExpansion(CR, Expansion.FileID))
        ExpansionCoverage.Expansions.emplace_back(CR, Expansion.Function);
    }

  sortNestedRegions(Regions.begin(), Regions.end());
  DEBUG(dbgs() << "Emitting segments for expansion of file " << Expansion.FileID
               << "\n");
  ExpansionCoverage.Segments = SegmentBuilder().buildSegments(Regions);

  return ExpansionCoverage;
}

LLVMCoverageMappingRef LLVMCreateCoverageMapping(const char *ObjectFilename,
                                                 const char *ProfileFilename) {
  return LLVMCreateCoverageMappingForArch(ObjectFilename, ProfileFilename, "");
}

LLVMCoverageMappingRef LLVMCreateCoverageMappingForArch(
    const char *ObjectFilename, const char *ProfileFilename, const char *Arch) {
  auto CMOrErr = CoverageMapping::load(ObjectFilename, ProfileFilename, Arch);
  if (!CMOrErr)
    return nullptr;
  return wrap(CMOrErr->release());
}

void LLVMDisposeCoverageMapping(LLVMCoverageMappingRef CMR) {
  if (CMR)
    delete unwrap(CMR);
}

LLVMCoverageFunctionRangeRef
LLVMCreateCoverageFunctionRange(LLVMCoverageMappingRef CMR) {
  auto *CFR =
      new FunctionRecordIterator(unwrap(CMR)->getCoveredFunctions().begin());
  return wrap(CFR);
}

LLVMCoverageFunctionRangeRef
LLVMCreateCoverageFunctionRangeForFile(LLVMCoverageMappingRef CMR,
                                       const char *Filename) {
  auto *CFR = new FunctionRecordIterator(
      unwrap(CMR)->getCoveredFunctions(Filename).begin());
  return wrap(CFR);
}

void
LLVMDisposeCoverageFunctionRange(LLVMCoverageFunctionRangeRef CFR) {
  if (CFR)
    delete unwrap(CFR);
}

LLVMCoverageFunctionRef
LLVMGetNextCoverageFunction(LLVMCoverageMappingRef CMR,
                            LLVMCoverageFunctionRangeRef CFRR) {
  FunctionRecordIterator *CFR = unwrap(CFRR);
  if (*CFR == unwrap(CMR)->getCoveredFunctions().end())
    return nullptr;
  const FunctionRecord &CF = **CFR;
  ++*CFR;
  return wrap(&CF);
}

const char *LLVMGetCoverageFunctionName(LLVMCoverageFunctionRef CFR) {
  return unwrap(CFR)->Name.c_str();
}

uint64_t LLVMGetCoverageFunctionExecutionCount(LLVMCoverageFunctionRef CFR) {
  return unwrap(CFR)->ExecutionCount;
}

LLVMCoverageDataRef LLVMCreateCoverageDataForFile(LLVMCoverageMappingRef CMR,
                                                  const char *Filename) {
  auto *CD = new CoverageData(unwrap(CMR)->getCoverageForFile(Filename));
  return wrap(CD);
}

LLVMCoverageDataRef
LLVMCreateCoverageDataForFunction(LLVMCoverageMappingRef CMR,
                                  const LLVMCoverageFunctionRef Function) {
  FunctionRecord *FR = unwrap(Function);
  auto *CD = new CoverageData(unwrap(CMR)->getCoverageForFunction(*FR));
  return wrap(CD);
}

void LLVMDisposeCoverageData(LLVMCoverageDataRef CDR) {
  if (CDR)
    delete unwrap(CDR);
}

LLVMCoverageSegmentRef LLVMGetFirstCoverageSegment(LLVMCoverageDataRef CDR) {
  CoverageData *CD = unwrap(CDR);
  if (CD->empty())
    return nullptr;
  return wrap(&*CD->begin());
}

LLVMCoverageSegmentRef LLVMGetNextCoverageSegment(LLVMCoverageDataRef CDR,
                                                  LLVMCoverageSegmentRef CSR) {
  CoverageData *CD = unwrap(CDR);
  CoverageSegment *CS = unwrap(CSR);

  assert(!CD->empty() && "invalid segment for data");
  auto *First = &*CD->begin();

  assert(CS >= First && CS < First + CD->size() && "invalid segment for data");
  if (static_cast<size_t>(++CS - First) < CD->size())
    return wrap(CS);
  return nullptr;
}

unsigned LLVMGetCoverageSegmentLine(LLVMCoverageSegmentRef CSR) {
  return unwrap(CSR)->Line;
}

unsigned LLVMGetCoverageSegmentColumn(LLVMCoverageSegmentRef CSR) {
  return unwrap(CSR)->Col;
}

uint64_t LLVMGetCoverageSegmentCount(LLVMCoverageSegmentRef CSR) {
  return unwrap(CSR)->Count;
}

LLVMBool LLVMGetCoverageSegmentHasCount(LLVMCoverageSegmentRef CSR) {
  return unwrap(CSR)->HasCount;
}

LLVMBool LLVMGetCoverageSegmentIsRegionEntry(LLVMCoverageSegmentRef CSR) {
  return unwrap(CSR)->IsRegionEntry;
}

namespace {
class CoverageMappingErrorCategoryType : public std::error_category {
  const char *name() const LLVM_NOEXCEPT override { return "llvm.coveragemap"; }
  std::string message(int IE) const override {
    auto E = static_cast<coveragemap_error>(IE);
    switch (E) {
    case coveragemap_error::success:
      return "Success";
    case coveragemap_error::eof:
      return "End of File";
    case coveragemap_error::no_data_found:
      return "No coverage data found";
    case coveragemap_error::unsupported_version:
      return "Unsupported coverage format version";
    case coveragemap_error::truncated:
      return "Truncated coverage data";
    case coveragemap_error::malformed:
      return "Malformed coverage data";
    }
    llvm_unreachable("A value of coveragemap_error has no message.");
  }
};
}

static ManagedStatic<CoverageMappingErrorCategoryType> ErrorCategory;

const std::error_category &llvm::coveragemap_category() {
  return *ErrorCategory;
}
