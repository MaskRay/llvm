//=-- SampleProf.cpp - Sample profiling format support --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains common definitions used in the reading and writing of
// sample profile data.
//
//===----------------------------------------------------------------------===//

#include "llvm/ProfileData/SampleProf.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

using namespace llvm;
using namespace sampleprof;

namespace llvm {
namespace sampleprof {
SampleProfileFormat FunctionSamples::Format;
DenseMap<uint64_t, StringRef> FunctionSamples::GUIDToFuncNameMap;
Module *FunctionSamples::CurrentModule;
} // namespace sampleprof

namespace {

class SampleProfError : public ErrorInfo<SampleProfError> {
public:
  static char ID;

  SampleProfError(sampleprof_error E) : E(E) {}

  void log(raw_ostream &OS) const override {
    switch (E) {
    case sampleprof_error::success:
      OS << "Success";
      break;
    case sampleprof_error::bad_magic:
      OS << "Invalid sample profile data (bad magic)";
      break;
    case sampleprof_error::unsupported_version:
      OS << "Unsupported sample profile format version";
      break;
    case sampleprof_error::too_large:
      OS << "Too much profile data";
      break;
    case sampleprof_error::truncated:
      OS << "Truncated profile data";
      break;
    case sampleprof_error::malformed:
      OS << "Malformed sample profile data";
      break;
    case sampleprof_error::unrecognized_format:
      OS << "Unrecognized sample profile encoding format";
      break;
    case sampleprof_error::unsupported_writing_format:
      OS << "Profile encoding format unsupported for writing operations";
      break;
    case sampleprof_error::truncated_name_table:
      OS << "Truncated function name table";
      break;
    case sampleprof_error::not_implemented:
      OS << "Unimplemented feature";
      break;
    case sampleprof_error::counter_overflow:
      OS << "Counter overflow";
      break;
    case sampleprof_error::ostream_seek_unsupported:
      OS << "Ostream does not support seek";
      break;
    }
  }

  std::error_code convertToErrorCode() const override {
    llvm_unreachable("Not implemented");
  }

private:
  sampleprof_error E;
};

char SampleProfError::ID = 0;

} // end anonymous namespace

Error createSampleProfError(sampleprof_error E) {
  return Error(llvm::make_unique<SampleProfError>(E));
}

void LineLocation::print(raw_ostream &OS) const {
  OS << LineOffset;
  if (Discriminator > 0)
    OS << "." << Discriminator;
}

raw_ostream &llvm::sampleprof::operator<<(raw_ostream &OS,
                                          const LineLocation &Loc) {
  Loc.print(OS);
  return OS;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void LineLocation::dump() const { print(dbgs()); }
#endif

/// Print the sample record to the stream \p OS indented by \p Indent.
void SampleRecord::print(raw_ostream &OS, unsigned Indent) const {
  OS << NumSamples;
  if (hasCalls()) {
    OS << ", calls:";
    for (const auto &I : getCallTargets())
      OS << " " << I.first() << ":" << I.second;
  }
  OS << "\n";
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void SampleRecord::dump() const { print(dbgs(), 0); }
#endif

raw_ostream &llvm::sampleprof::operator<<(raw_ostream &OS,
                                          const SampleRecord &Sample) {
  Sample.print(OS, 0);
  return OS;
}

/// Print the samples collected for a function on stream \p OS.
void FunctionSamples::print(raw_ostream &OS, unsigned Indent) const {
  OS << TotalSamples << ", " << TotalHeadSamples << ", " << BodySamples.size()
     << " sampled lines\n";

  OS.indent(Indent);
  if (!BodySamples.empty()) {
    OS << "Samples collected in the function's body {\n";
    SampleSorter<LineLocation, SampleRecord> SortedBodySamples(BodySamples);
    for (const auto &SI : SortedBodySamples.get()) {
      OS.indent(Indent + 2);
      OS << SI->first << ": " << SI->second;
    }
    OS.indent(Indent);
    OS << "}\n";
  } else {
    OS << "No samples collected in the function's body\n";
  }

  OS.indent(Indent);
  if (!CallsiteSamples.empty()) {
    OS << "Samples collected in inlined callsites {\n";
    SampleSorter<LineLocation, FunctionSamplesMap> SortedCallsiteSamples(
        CallsiteSamples);
    for (const auto &CS : SortedCallsiteSamples.get()) {
      for (const auto &FS : CS->second) {
        OS.indent(Indent + 2);
        OS << CS->first << ": inlined callee: " << FS.second.getName() << ": ";
        FS.second.print(OS, Indent + 4);
      }
    }
    OS << "}\n";
  } else {
    OS << "No inlined callsites in this function\n";
  }
}

raw_ostream &llvm::sampleprof::operator<<(raw_ostream &OS,
                                          const FunctionSamples &FS) {
  FS.print(OS);
  return OS;
}

unsigned FunctionSamples::getOffset(const DILocation *DIL) {
  return (DIL->getLine() - DIL->getScope()->getSubprogram()->getLine()) &
      0xffff;
}

const FunctionSamples *
FunctionSamples::findFunctionSamples(const DILocation *DIL) const {
  assert(DIL);
  SmallVector<std::pair<LineLocation, StringRef>, 10> S;

  const DILocation *PrevDIL = DIL;
  for (DIL = DIL->getInlinedAt(); DIL; DIL = DIL->getInlinedAt()) {
    S.push_back(std::make_pair(
        LineLocation(getOffset(DIL), DIL->getBaseDiscriminator()),
        PrevDIL->getScope()->getSubprogram()->getLinkageName()));
    PrevDIL = DIL;
  }
  if (S.size() == 0)
    return this;
  const FunctionSamples *FS = this;
  for (int i = S.size() - 1; i >= 0 && FS != nullptr; i--) {
    FS = FS->findFunctionSamplesAt(S[i].first, S[i].second);
  }
  return FS;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void FunctionSamples::dump() const { print(dbgs(), 0); }
#endif
} // namespace llvm
