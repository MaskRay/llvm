//===- SampleProfWriter.cpp - Write LLVM sample profile data --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the class that writes LLVM sample profiles. It
// supports two file formats: text and binary. The textual representation
// is useful for debugging and testing purposes. The binary representation
// is more compact, resulting in smaller file sizes. However, they can
// both be used interchangeably.
//
// See lib/ProfileData/SampleProfReader.cpp for documentation on each of the
// supported formats.
//
//===----------------------------------------------------------------------===//

#include "llvm/ProfileData/SampleProfWriter.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ProfileData/ProfileCommon.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

using namespace llvm;
using namespace sampleprof;

Error SampleProfileWriter::write(const StringMap<FunctionSamples> &ProfileMap) {
  if (Error E = writeHeader(ProfileMap))
    return E;

  // Sort the ProfileMap by total samples.
  typedef std::pair<StringRef, const FunctionSamples *> NameFunctionSamples;
  std::vector<NameFunctionSamples> V;
  for (const auto &I : ProfileMap)
    V.push_back(std::make_pair(I.getKey(), &I.second));

  std::stable_sort(
      V.begin(), V.end(),
      [](const NameFunctionSamples &A, const NameFunctionSamples &B) {
        if (A.second->getTotalSamples() == B.second->getTotalSamples())
          return A.first > B.first;
        return A.second->getTotalSamples() > B.second->getTotalSamples();
      });

  for (const auto &I : V) {
    if (Error E = write(*I.second))
      return E;
  }
  return Error::success();
}

Error SampleProfileWriterCompactBinary::write(
    const StringMap<FunctionSamples> &ProfileMap) {
  if (Error E = SampleProfileWriter::write(ProfileMap))
    return E;
  if (Error E = writeFuncOffsetTable())
    return E;
  return Error::success();
}

/// Write samples to a text file.
///
/// Note: it may be tempting to implement this in terms of
/// FunctionSamples::print().  Please don't.  The dump functionality is intended
/// for debugging and has no specified form.
///
/// The format used here is more structured and deliberate because
/// it needs to be parsed by the SampleProfileReaderText class.
Error SampleProfileWriterText::write(const FunctionSamples &S) {
  auto &OS = *OutputStream;
  OS << S.getName() << ":" << S.getTotalSamples();
  if (Indent == 0)
    OS << ":" << S.getHeadSamples();
  OS << "\n";

  SampleSorter<LineLocation, SampleRecord> SortedSamples(S.getBodySamples());
  for (const auto &I : SortedSamples.get()) {
    LineLocation Loc = I->first;
    const SampleRecord &Sample = I->second;
    OS.indent(Indent + 1);
    if (Loc.Discriminator == 0)
      OS << Loc.LineOffset << ": ";
    else
      OS << Loc.LineOffset << "." << Loc.Discriminator << ": ";

    OS << Sample.getSamples();

    for (const auto &J : Sample.getCallTargets())
      OS << " " << J.first() << ":" << J.second;
    OS << "\n";
  }

  SampleSorter<LineLocation, FunctionSamplesMap> SortedCallsiteSamples(
      S.getCallsiteSamples());
  Indent += 1;
  for (const auto &I : SortedCallsiteSamples.get())
    for (const auto &FS : I->second) {
      LineLocation Loc = I->first;
      const FunctionSamples &CalleeSamples = FS.second;
      OS.indent(Indent);
      if (Loc.Discriminator == 0)
        OS << Loc.LineOffset << ": ";
      else
        OS << Loc.LineOffset << "." << Loc.Discriminator << ": ";
      if (Error E = write(CalleeSamples))
        return E;
    }
  Indent -= 1;

  return Error::success();
}

Error SampleProfileWriterBinary::writeNameIdx(StringRef FName) {
  const auto &ret = NameTable.find(FName);
  if (ret == NameTable.end())
    return createSampleProfError(sampleprof_error::truncated_name_table);
  encodeULEB128(ret->second, *OutputStream);
  return Error::success();
}

void SampleProfileWriterBinary::addName(StringRef FName) {
  NameTable.insert(std::make_pair(FName, 0));
}

void SampleProfileWriterBinary::addNames(const FunctionSamples &S) {
  // Add all the names in indirect call targets.
  for (const auto &I : S.getBodySamples()) {
    const SampleRecord &Sample = I.second;
    for (const auto &J : Sample.getCallTargets())
      addName(J.first());
  }

  // Recursively add all the names for inlined callsites.
  for (const auto &J : S.getCallsiteSamples())
    for (const auto &FS : J.second) {
      const FunctionSamples &CalleeSamples = FS.second;
      addName(CalleeSamples.getName());
      addNames(CalleeSamples);
    }
}

void SampleProfileWriterBinary::stablizeNameTable(std::set<StringRef> &V) {
  // Sort the names to make NameTable deterministic.
  for (const auto &I : NameTable)
    V.insert(I.first);
  int i = 0;
  for (const StringRef &N : V)
    NameTable[N] = i++;
}

Error SampleProfileWriterRawBinary::writeNameTable() {
  auto &OS = *OutputStream;
  std::set<StringRef> V;
  stablizeNameTable(V);

  // Write out the name table.
  encodeULEB128(NameTable.size(), OS);
  for (auto N : V) {
    OS << N;
    encodeULEB128(0, OS);
  }
  return Error::success();
}

Error SampleProfileWriterCompactBinary::writeFuncOffsetTable() {
  auto &OS = *OutputStream;

  // Fill the slot remembered by TableOffset with the offset of FuncOffsetTable.
  auto &OFS = static_cast<raw_fd_ostream &>(OS);
  uint64_t FuncOffsetTableStart = OS.tell();
  if (OFS.seek(TableOffset) == (uint64_t)-1)
    return createSampleProfError(sampleprof_error::ostream_seek_unsupported);
  support::endian::Writer Writer(*OutputStream, support::little);
  Writer.write(FuncOffsetTableStart);
  if (OFS.seek(FuncOffsetTableStart) == (uint64_t)-1)
    return createSampleProfError(sampleprof_error::ostream_seek_unsupported);

  // Write out the table size.
  encodeULEB128(FuncOffsetTable.size(), OS);

  // Write out FuncOffsetTable.
  for (auto entry : FuncOffsetTable) {
    if (Error E = writeNameIdx(entry.first))
      return E;
    encodeULEB128(entry.second, OS);
  }
  return Error::success();
}

Error SampleProfileWriterCompactBinary::writeNameTable() {
  auto &OS = *OutputStream;
  std::set<StringRef> V;
  stablizeNameTable(V);

  // Write out the name table.
  encodeULEB128(NameTable.size(), OS);
  for (auto N : V) {
    encodeULEB128(MD5Hash(N), OS);
  }
  return Error::success();
}

Error SampleProfileWriterRawBinary::writeMagicIdent() {
  auto &OS = *OutputStream;
  // Write file magic identifier.
  encodeULEB128(SPMagic(), OS);
  encodeULEB128(SPVersion(), OS);
  return Error::success();
}

Error SampleProfileWriterCompactBinary::writeMagicIdent() {
  auto &OS = *OutputStream;
  // Write file magic identifier.
  encodeULEB128(SPMagic(SPF_Compact_Binary), OS);
  encodeULEB128(SPVersion(), OS);
  return Error::success();
}

Error SampleProfileWriterBinary::writeHeader(
    const StringMap<FunctionSamples> &ProfileMap) {
  (void)writeMagicIdent();

  computeSummary(ProfileMap);
  if (Error E = writeSummary())
    return E;

  // Generate the name table for all the functions referenced in the profile.
  for (const auto &I : ProfileMap) {
    addName(I.first());
    addNames(I.second);
  }

  (void)writeNameTable();
  return Error::success();
}

Error SampleProfileWriterCompactBinary::writeHeader(
    const StringMap<FunctionSamples> &ProfileMap) {
  support::endian::Writer Writer(*OutputStream, support::little);
  if (Error E = SampleProfileWriterBinary::writeHeader(ProfileMap))
    return E;

  // Reserve a slot for the offset of function offset table. The slot will
  // be populated with the offset of FuncOffsetTable later.
  TableOffset = OutputStream->tell();
  Writer.write(static_cast<uint64_t>(-2));
  return Error::success();
}

Error SampleProfileWriterBinary::writeSummary() {
  auto &OS = *OutputStream;
  encodeULEB128(Summary->getTotalCount(), OS);
  encodeULEB128(Summary->getMaxCount(), OS);
  encodeULEB128(Summary->getMaxFunctionCount(), OS);
  encodeULEB128(Summary->getNumCounts(), OS);
  encodeULEB128(Summary->getNumFunctions(), OS);
  std::vector<ProfileSummaryEntry> &Entries = Summary->getDetailedSummary();
  encodeULEB128(Entries.size(), OS);
  for (auto Entry : Entries) {
    encodeULEB128(Entry.Cutoff, OS);
    encodeULEB128(Entry.MinCount, OS);
    encodeULEB128(Entry.NumCounts, OS);
  }
  return Error::success();
}

Error SampleProfileWriterBinary::writeBody(const FunctionSamples &S) {
  auto &OS = *OutputStream;

  if (Error E = writeNameIdx(S.getName()))
    return E;

  encodeULEB128(S.getTotalSamples(), OS);

  // Emit all the body samples.
  encodeULEB128(S.getBodySamples().size(), OS);
  for (const auto &I : S.getBodySamples()) {
    LineLocation Loc = I.first;
    const SampleRecord &Sample = I.second;
    encodeULEB128(Loc.LineOffset, OS);
    encodeULEB128(Loc.Discriminator, OS);
    encodeULEB128(Sample.getSamples(), OS);
    encodeULEB128(Sample.getCallTargets().size(), OS);
    for (const auto &J : Sample.getCallTargets()) {
      StringRef Callee = J.first();
      uint64_t CalleeSamples = J.second;
      if (Error E = writeNameIdx(Callee))
        return E;
      encodeULEB128(CalleeSamples, OS);
    }
  }

  // Recursively emit all the callsite samples.
  uint64_t NumCallsites = 0;
  for (const auto &J : S.getCallsiteSamples())
    NumCallsites += J.second.size();
  encodeULEB128(NumCallsites, OS);
  for (const auto &J : S.getCallsiteSamples())
    for (const auto &FS : J.second) {
      LineLocation Loc = J.first;
      const FunctionSamples &CalleeSamples = FS.second;
      encodeULEB128(Loc.LineOffset, OS);
      encodeULEB128(Loc.Discriminator, OS);
      if (Error E = writeBody(CalleeSamples))
        return E;
    }

  return Error::success();
}

/// Write samples of a top-level function to a binary file.
///
/// \returns true if the samples were written successfully, false otherwise.
Error SampleProfileWriterBinary::write(const FunctionSamples &S) {
  encodeULEB128(S.getHeadSamples(), *OutputStream);
  return writeBody(S);
}

Error SampleProfileWriterCompactBinary::write(const FunctionSamples &S) {
  uint64_t Offset = OutputStream->tell();
  StringRef Name = S.getName();
  FuncOffsetTable[Name] = Offset;
  encodeULEB128(S.getHeadSamples(), *OutputStream);
  return writeBody(S);
}

/// Create a sample profile file writer based on the specified format.
///
/// \param Filename The file to create.
///
/// \param Format Encoding format for the profile file.
///
/// \returns an error code indicating the status of the created writer.
Expected<std::unique_ptr<SampleProfileWriter>>
SampleProfileWriter::create(StringRef Filename, SampleProfileFormat Format) {
  std::error_code EC;
  std::unique_ptr<raw_ostream> OS;
  if (Format == SPF_Binary || Format == SPF_Compact_Binary)
    OS.reset(new raw_fd_ostream(Filename, EC, sys::fs::F_None));
  else
    OS.reset(new raw_fd_ostream(Filename, EC, sys::fs::F_Text));
  if (EC)
    return errorCodeToError(EC);

  return create(OS, Format);
}

/// Create a sample profile stream writer based on the specified format.
///
/// \param OS The output stream to store the profile data to.
///
/// \param Format Encoding format for the profile file.
///
/// \returns an Expected indicating the status of the created writer.
Expected<std::unique_ptr<SampleProfileWriter>>
SampleProfileWriter::create(std::unique_ptr<raw_ostream> &OS,
                            SampleProfileFormat Format) {
  std::unique_ptr<SampleProfileWriter> Writer;

  if (Format == SPF_Binary)
    Writer.reset(new SampleProfileWriterRawBinary(OS));
  else if (Format == SPF_Compact_Binary)
    Writer.reset(new SampleProfileWriterCompactBinary(OS));
  else if (Format == SPF_Text)
    Writer.reset(new SampleProfileWriterText(OS));
  else if (Format == SPF_GCC)
    return createSampleProfError(sampleprof_error::unsupported_writing_format);
  else
    return createSampleProfError(sampleprof_error::unrecognized_format);

  return std::move(Writer);
}

void SampleProfileWriter::computeSummary(
    const StringMap<FunctionSamples> &ProfileMap) {
  SampleProfileSummaryBuilder Builder(ProfileSummaryBuilder::DefaultCutoffs);
  for (const auto &I : ProfileMap) {
    const FunctionSamples &Profile = I.second;
    Builder.addRecord(Profile);
  }
  Summary = Builder.getSummary();
}
