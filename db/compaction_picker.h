//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "db/compaction.h"
#include "db/version_set.h"
#include "options/cf_options.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "rocksdb/terark_namespace.h"
#include "util/chash_set.h"

namespace TERARKDB_NAMESPACE {

class Compaction;
struct CompactionInputFiles;
struct EnvOptions;
class LogBuffer;
class TableCache;
class VersionStorageInfo;

class CompactionPicker {
 public:
  CompactionPicker(TableCache* table_cache, const EnvOptions& env_options,
                   const ImmutableCFOptions& ioptions,
                   const InternalKeyComparator* icmp);
  virtual ~CompactionPicker();

  struct SortedRun {
    SortedRun()
        : level(-1),
          file(nullptr),
          size(0),
          compensated_file_size(0),
          being_compacted(false),
          skip_composite(false) {}
    SortedRun(int _level, FileMetaData* _file, uint64_t _size,
              uint64_t _compensated_file_size, bool _being_compacted)
        : level(_level),
          file(_file),
          size(_size),
          compensated_file_size(_compensated_file_size),
          being_compacted(_being_compacted),
          skip_composite(false) {
      assert(compensated_file_size > 0);
      assert(level != 0 || file != nullptr);
    }

    void Dump(char* out_buf, size_t out_buf_size,
              bool print_path = false) const;

    // sorted_run_count is added into the string to print
    void DumpSizeInfo(char* out_buf, size_t out_buf_size,
                      size_t sorted_run_count) const;

    int level;
    // `file` Will be null for level > 0. For level = 0, the sorted run is
    // for this file.
    FileMetaData* file;
    // For level > 0, `size` and `compensated_file_size` are sum of sizes all
    // files in the level. `being_compacted` should be the same for all files
    // in a non-zero level. Use the value here.
    uint64_t size;
    uint64_t compensated_file_size;
    bool being_compacted;
    bool skip_composite;
  };

  static CompactionReason ConvertCompactionReason(
      uint8_t marked, CompactionReason default_reason);

  static double GetQ(std::vector<double>::const_iterator b,
                     std::vector<double>::const_iterator e, size_t g);

  static bool ReadMapElement(MapSstElement& map_element, InternalIterator* iter,
                             LogBuffer* log_buffer, const std::string& cf_name);

  static bool FixInputRange(std::vector<SelectedRange>& input_range,
                            const InternalKeyComparator& icmp, bool sort,
                            bool merge);

  const EnvOptions& env_options() { return env_options_; }

  TableCache* table_cache() { return table_cache_; }

  // Pick level and inputs for a new compaction.
  // Returns nullptr if there is no compaction to be done.
  // Otherwise returns a pointer to a heap-allocated object that
  // describes the compaction.  Caller should delete the result.
  virtual Compaction* PickCompaction(
      const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
      VersionStorageInfo* vstorage,
      const std::vector<SequenceNumber>& snapshots, LogBuffer* log_buffer) = 0;

  // Pick compaction which level has map or link sst
  Compaction* PickGarbageCollection(const std::string& cf_name,
                                    const MutableCFOptions& mutable_cf_options,
                                    VersionStorageInfo* vstorage,
                                    LogBuffer* log_buffer);

  virtual void InitFilesBeingCompact(const MutableCFOptions& mutable_cf_options,
                                     VersionStorageInfo* vstorage,
                                     const InternalKey* begin,
                                     const InternalKey* end,
                                     chash_set<uint64_t>* files_being_compact);

  // Return a compaction object for compacting the range [begin,end] in
  // the specified level.  Returns nullptr if there is nothing in that
  // level that overlaps the specified range.  Caller should delete
  // the result.
  //
  // The returned Compaction might not include the whole requested range.
  // In that case, compaction_end will be set to the next key that needs
  // compacting. In case the compaction will compact the whole range,
  // compaction_end will be set to nullptr.
  // Client is responsible for compaction_end storage -- when called,
  // *compaction_end should point to valid InternalKey!
  virtual Compaction* CompactRange(
      const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
      SeparationType separation_type, VersionStorageInfo* vstorage,
      int input_level, int output_level, uint32_t output_path_id,
      uint32_t max_subcompactions, const InternalKey* begin,
      const InternalKey* end, InternalKey** compaction_end,
      bool* manual_conflict, const chash_set<uint64_t>* files_being_compact);

  // Pick compaction which pointed range files
  // range use internal keys
  Compaction* PickRangeCompaction(
      const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
      SeparationType separation_type, VersionStorageInfo* vstorage, int level,
      const InternalKey* begin, const InternalKey* end,
      uint32_t max_subcompactions,
      const chash_set<uint64_t>* files_being_compact, bool* manual_conflict,
      LogBuffer* log_buffer);

  // The maximum allowed output level.  Default value is NumberLevels() - 1.
  virtual int MaxOutputLevel() const { return NumberLevels() - 1; }

  virtual bool NeedsCompaction(const VersionStorageInfo* vstorage) const = 0;

// Sanitize the input set of compaction input files.
// When the input parameters do not describe a valid compaction, the
// function will try to fix the input_files by adding necessary
// files.  If it's not possible to conver an invalid input_files
// into a valid one by adding more files, the function will return a
// non-ok status with specific reason.
#ifndef ROCKSDB_LITE
  Status SanitizeCompactionInputFiles(std::unordered_set<uint64_t>* input_files,
                                      const ColumnFamilyMetaData& cf_meta,
                                      const int output_level) const;
#endif  // ROCKSDB_LITE

  // Free up the files that participated in a compaction
  //
  // Requirement: DB mutex held
  void ReleaseCompactionFiles(Compaction* c, Status status);

  // Returns true if any one of the specified files are being compacted
  bool AreFilesInCompaction(const std::vector<FileMetaData*>& files);

  // Takes a list of CompactionInputFiles and returns a (manual) Compaction
  // object.
  //
  // Caller must provide a set of input files that has been passed through
  // `SanitizeCompactionInputFiles` earlier. The lock should not be released
  // between that call and this one.
  Compaction* CompactFiles(const CompactionOptions& compact_options,
                           const std::vector<CompactionInputFiles>& input_files,
                           int output_level, VersionStorageInfo* vstorage,
                           const MutableCFOptions& mutable_cf_options,
                           uint32_t output_path_id);

  // Converts a set of compaction input file numbers into
  // a list of CompactionInputFiles.
  Status GetCompactionInputsFromFileNumbers(
      std::vector<CompactionInputFiles>* input_files,
      std::unordered_set<uint64_t>* input_set,
      const VersionStorageInfo* vstorage,
      const CompactionOptions& compact_options) const;

  // Is there currently a compaction involving level 0 taking place
  bool IsLevel0CompactionInProgress() const {
    return !level0_compactions_in_progress_.empty();
  }

  // Return true if the passed key range overlap with a compaction output
  // that is currently running.
  bool RangeOverlapWithCompaction(const Slice& smallest_user_key,
                                  const Slice& largest_user_key,
                                  int level) const;

  // Stores the minimal range that covers all entries in inputs in
  // *smallest, *largest.
  // REQUIRES: inputs is not empty
  void GetRange(const CompactionInputFiles& inputs, InternalKey* smallest,
                InternalKey* largest) const;

  // Stores the minimal range that covers all entries in inputs1 and inputs2
  // in *smallest, *largest.
  // REQUIRES: inputs is not empty
  void GetRange(const CompactionInputFiles& inputs1,
                const CompactionInputFiles& inputs2, InternalKey* smallest,
                InternalKey* largest) const;

  // Stores the minimal range that covers all entries in inputs
  // in *smallest, *largest.
  // REQUIRES: inputs is not empty (at least on entry have one file)
  void GetRange(const std::vector<CompactionInputFiles>& inputs,
                InternalKey* smallest, InternalKey* largest) const;

  int NumberLevels() const { return ioptions_.num_levels; }

  // Add more files to the inputs on "level" to make sure that
  // no newer version of a key is compacted to "level+1" while leaving an older
  // version in a "level". Otherwise, any Get() will search "level" first,
  // and will likely return an old/stale value for the key, since it always
  // searches in increasing order of level to find the value. This could
  // also scramble the order of merge operands. This function should be
  // called any time a new Compaction is created, and its inputs_[0] are
  // populated.
  //
  // Will return false if it is impossible to apply this compaction.
  bool ExpandInputsToCleanCut(const std::string& cf_name,
                              VersionStorageInfo* vstorage,
                              CompactionInputFiles* inputs,
                              InternalKey** next_smallest = nullptr);

  // Returns true if any one of the parent files are being compacted
  bool IsRangeInCompaction(VersionStorageInfo* vstorage,
                           const InternalKey* smallest,
                           const InternalKey* largest, int level, int* index);

  // Returns true if the key range that `inputs` files cover overlap with the
  // key range of a currently running compaction.
  bool FilesRangeOverlapWithCompaction(
      const std::vector<CompactionInputFiles>& inputs, int level) const;

  bool SetupOtherInputs(const std::string& cf_name,
                        const MutableCFOptions& mutable_cf_options,
                        VersionStorageInfo* vstorage,
                        CompactionInputFiles* inputs,
                        CompactionInputFiles* output_level_inputs,
                        int* parent_index, int base_index);

  void GetGrandparents(VersionStorageInfo* vstorage,
                       const CompactionInputFiles& inputs,
                       const CompactionInputFiles& output_level_inputs,
                       std::vector<FileMetaData*>* grandparents);

  // Pick compaction which level has map or link sst
  Compaction* PickCompositeCompaction(
      const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
      VersionStorageInfo* vstorage,
      const std::vector<SequenceNumber>& snapshots,
      const std::vector<SortedRun>& sorted_runs, LogBuffer* log_buffer);

  void PickFilesMarkedForCompaction(const std::string& cf_name,
                                    VersionStorageInfo* vstorage,
                                    int* start_level, int* output_level,
                                    CompactionInputFiles* start_level_inputs);

  bool GetOverlappingL0Files(VersionStorageInfo* vstorage,
                             CompactionInputFiles* start_level_inputs,
                             int output_level, int* parent_index);

  // Register this compaction in the set of running compactions
  Compaction* RegisterCompaction(Compaction* c);

  // Remove this compaction from the set of running compactions
  void UnregisterCompaction(Compaction* c);

  std::set<Compaction*>* level0_compactions_in_progress() {
    return &level0_compactions_in_progress_;
  }
  std::unordered_set<Compaction*>* compactions_in_progress() {
    return &compactions_in_progress_;
  }

 protected:
  TableCache* table_cache_;
  const EnvOptions& env_options_;
  const ImmutableCFOptions& ioptions_;

// A helper function to SanitizeCompactionInputFiles() that
// sanitizes "input_files" by adding necessary files.
#ifndef ROCKSDB_LITE
  virtual Status SanitizeCompactionInputFilesForAllLevels(
      std::unordered_set<uint64_t>* input_files,
      const ColumnFamilyMetaData& cf_meta, const int output_level) const;
#endif  // ROCKSDB_LITE

  // Keeps track of all compactions that are running on Level0.
  // Protected by DB mutex
  std::set<Compaction*> level0_compactions_in_progress_;

  // Keeps track of all compactions that are running.
  // Protected by DB mutex
  std::unordered_set<Compaction*> compactions_in_progress_;

  const InternalKeyComparator* const icmp_;
};

class LevelCompactionPicker : public CompactionPicker {
 public:
  LevelCompactionPicker(TableCache* table_cache, const EnvOptions& env_options,
                        const ImmutableCFOptions& ioptions,
                        const InternalKeyComparator* icmp)
      : CompactionPicker(table_cache, env_options, ioptions, icmp) {}
  Compaction* PickCompaction(const std::string& cf_name,
                             const MutableCFOptions& mutable_cf_options,
                             VersionStorageInfo* vstorage,
                             const std::vector<SequenceNumber>& snapshots,
                             LogBuffer* log_buffer) override;

  bool NeedsCompaction(const VersionStorageInfo* vstorage) const override;
};

#ifndef ROCKSDB_LITE

class NullCompactionPicker : public CompactionPicker {
 public:
  NullCompactionPicker(TableCache* table_cache, const EnvOptions& env_options,
                       const ImmutableCFOptions& ioptions,
                       const InternalKeyComparator* icmp)
      : CompactionPicker(table_cache, env_options, ioptions, icmp) {}
  virtual ~NullCompactionPicker() {}

  // Always return "nullptr"
  Compaction* PickCompaction(const std::string& /*cf_name*/,
                             const MutableCFOptions& /*mutable_cf_options*/,
                             VersionStorageInfo* /*vstorage*/,
                             const std::vector<SequenceNumber>& /*snapshots*/,
                             LogBuffer* /*log_buffer*/) override {
    return nullptr;
  }

  // Always return "nullptr"
  Compaction* CompactRange(
      const std::string& /*cf_name*/,
      const MutableCFOptions& /*mutable_cf_options*/,
      SeparationType /*separation_type*/, VersionStorageInfo* /*vstorage*/,
      int /*input_level*/, int /*output_level*/, uint32_t /*output_path_id*/,
      uint32_t /*max_subcompactions*/, const InternalKey* /*begin*/,
      const InternalKey* /*end*/, InternalKey** /*compaction_end*/,
      bool* /*manual_conflict*/,
      const chash_set<uint64_t>* /*files_being_compact*/) override {
    return nullptr;
  }

  // Always returns false.
  virtual bool NeedsCompaction(
      const VersionStorageInfo* /*vstorage*/) const override {
    return false;
  }
};
#endif  // !ROCKSDB_LITE

bool FindIntraL0Compaction(const std::vector<FileMetaData*>& level_files,
                           size_t min_files_to_compact,
                           uint64_t max_compact_bytes_per_del_file,
                           CompactionInputFiles* comp_inputs);

CompressionType GetCompressionType(const ImmutableCFOptions& ioptions,
                                   const VersionStorageInfo* vstorage,
                                   const MutableCFOptions& mutable_cf_options,
                                   int level, int base_level,
                                   const bool enable_compression = true);

CompressionOptions GetCompressionOptions(const ImmutableCFOptions& ioptions,
                                         const VersionStorageInfo* vstorage,
                                         int level,
                                         const bool enable_compression = true);

}  // namespace TERARKDB_NAMESPACE
