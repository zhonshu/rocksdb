//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/internal_stats.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <vector>
#include "db/column_family.h"

#include "db/db_impl.h"
#include "util/string_util.h"

namespace rocksdb {

#ifndef ROCKSDB_LITE
namespace {
const double kMB = 1048576.0;
const double kGB = kMB * 1024;

void PrintLevelStatsHeader(char* buf, size_t len, const std::string& cf_name) {
  snprintf(
      buf, len,
      "\n** Compaction Stats [%s] **\n"
      "Level   Files   Size(MB) Score Read(GB)  Rn(GB) Rnp1(GB) "
      "Write(GB) Wnew(GB) Moved(GB) W-Amp Rd(MB/s) Wr(MB/s) "
      "Comp(sec) Comp(cnt) Avg(sec) "
      "Stall(sec) Stall(cnt) Avg(ms)     RecordIn   RecordDrop\n"
      "--------------------------------------------------------------------"
      "--------------------------------------------------------------------"
      "----------------------------------------------------------\n",
      cf_name.c_str());
}

void PrintLevelStats(char* buf, size_t len, const std::string& name,
    int num_files, int being_compacted, double total_file_size, double score,
    double w_amp, double stall_us, uint64_t stalls,
    const InternalStats::CompactionStats& stats) {
  uint64_t bytes_read = stats.bytes_readn + stats.bytes_readnp1;
  uint64_t bytes_new = stats.bytes_written - stats.bytes_readnp1;
  double elapsed = (stats.micros + 1) / 1000000.0;

  snprintf(buf, len,
           "%4s %5d/%-3d %8.0f %5.1f " /* Level, Files, Size(MB), Score */
           "%8.1f "                    /* Read(GB) */
           "%7.1f "                    /* Rn(GB) */
           "%8.1f "                    /* Rnp1(GB) */
           "%9.1f "                    /* Write(GB) */
           "%8.1f "                    /* Wnew(GB) */
           "%9.1f "                    /* Moved(GB) */
           "%5.1f "                    /* W-Amp */
           "%8.1f "                    /* Rd(MB/s) */
           "%8.1f "                    /* Wr(MB/s) */
           "%9.0f "                   /* Comp(sec) */
           "%9d "                      /* Comp(cnt) */
           "%8.3f "                    /* Avg(sec) */
           "%10.2f "                   /* Stall(sec) */
           "%10" PRIu64
           " "      /* Stall(cnt) */
           "%7.2f " /* Avg(ms) */
           "%12" PRIu64
           " " /* input entries */
           "%12" PRIu64 "\n" /* number of records reduced */,
           name.c_str(), num_files, being_compacted, total_file_size / kMB,
           score, bytes_read / kGB, stats.bytes_readn / kGB,
           stats.bytes_readnp1 / kGB, stats.bytes_written / kGB,
           bytes_new / kGB, stats.bytes_moved / kGB,
           w_amp, bytes_read / kMB / elapsed,
           stats.bytes_written / kMB / elapsed,
           stats.micros / 1000000.0, stats.count,
           stats.count == 0 ? 0 : stats.micros / 1000000.0 / stats.count,
           stall_us / 1000000.0, stalls,
           stalls == 0 ? 0 : stall_us / 1000.0 / stalls,
           stats.num_input_records, stats.num_dropped_records);
}


}

DBPropertyType GetPropertyType(const Slice& property, bool* is_int_property,
                               bool* need_out_of_mutex) {
  assert(is_int_property != nullptr);
  assert(need_out_of_mutex != nullptr);
  Slice in = property;
  Slice prefix("rocksdb.");
  *need_out_of_mutex = false;
  *is_int_property = false;
  if (!in.starts_with(prefix)) {
    return kUnknown;
  }
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    return kNumFilesAtLevel;
  } else if (in == "levelstats") {
    return kLevelStats;
  } else if (in == "stats") {
    return kStats;
  } else if (in == "cfstats") {
    return kCFStats;
  } else if (in == "dbstats") {
    return kDBStats;
  } else if (in == "sstables") {
    return kSsTables;
  }

  *is_int_property = true;
  if (in == "num-immutable-mem-table") {
    return kNumImmutableMemTable;
  } else if (in == "mem-table-flush-pending") {
    return kMemtableFlushPending;
  } else if (in == "compaction-pending") {
    return kCompactionPending;
  } else if (in == "background-errors") {
    return kBackgroundErrors;
  } else if (in == "cur-size-active-mem-table") {
    return kCurSizeActiveMemTable;
  } else if (in == "cur-size-all-mem-tables") {
    return kCurSizeAllMemTables;
  } else if (in == "num-entries-active-mem-table") {
    return kNumEntriesInMutableMemtable;
  } else if (in == "num-entries-imm-mem-tables") {
    return kNumEntriesInImmutableMemtable;
  } else if (in == "estimate-num-keys") {
    return kEstimatedNumKeys;
  } else if (in == "estimate-table-readers-mem") {
    *need_out_of_mutex = true;
    return kEstimatedUsageByTableReaders;
  } else if (in == "is-file-deletions-enabled") {
    return kIsFileDeletionEnabled;
  } else if (in == "num-snapshots") {
    return kNumSnapshots;
  } else if (in == "oldest-snapshot-time") {
    return kOldestSnapshotTime;
  } else if (in == "num-live-versions") {
    return kNumLiveVersions;
  }
  return kUnknown;
}

bool InternalStats::GetIntPropertyOutOfMutex(DBPropertyType property_type,
                                             Version* version,
                                             uint64_t* value) const {
  assert(value != nullptr);
  if (property_type != kEstimatedUsageByTableReaders) {
    return false;
  }
  if (version == nullptr) {
    *value = 0;
  } else {
    *value = version->GetMemoryUsageByTableReaders();
  }
  return true;
}

bool InternalStats::GetStringProperty(DBPropertyType property_type,
                                      const Slice& property,
                                      std::string* value) {
  assert(value != nullptr);
  auto* current = cfd_->current();
  const auto* vstorage = current->storage_info();
  Slice in = property;

  switch (property_type) {
    case kNumFilesAtLevel: {
      in.remove_prefix(strlen("rocksdb.num-files-at-level"));
      uint64_t level;
      bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
      if (!ok || (int)level >= number_levels_) {
        return false;
      } else {
        char buf[100];
        snprintf(buf, sizeof(buf), "%d",
                 vstorage->NumLevelFiles(static_cast<int>(level)));
        *value = buf;
        return true;
      }
    }
    case kLevelStats: {
      char buf[1000];
      snprintf(buf, sizeof(buf),
               "Level Files Size(MB)\n"
               "--------------------\n");
      value->append(buf);

      for (int level = 0; level < number_levels_; level++) {
        snprintf(buf, sizeof(buf), "%3d %8d %8.0f\n", level,
                 vstorage->NumLevelFiles(level),
                 vstorage->NumLevelBytes(level) / kMB);
        value->append(buf);
      }
      return true;
    }
    case kStats: {
      if (!GetStringProperty(kCFStats, "rocksdb.cfstats", value)) {
        return false;
      }
      if (!GetStringProperty(kDBStats, "rocksdb.dbstats", value)) {
        return false;
      }
      return true;
    }
    case kCFStats: {
      DumpCFStats(value);
      return true;
    }
    case kDBStats: {
      DumpDBStats(value);
      return true;
    }
    case kSsTables:
      *value = current->DebugString();
      return true;
    default:
      return false;
  }
}

bool InternalStats::GetIntProperty(DBPropertyType property_type,
                                   uint64_t* value, DBImpl* db) const {
  db->mutex_.AssertHeld();
  const auto* vstorage = cfd_->current()->storage_info();

  switch (property_type) {
    case kNumImmutableMemTable:
      *value = cfd_->imm()->size();
      return true;
    case kMemtableFlushPending:
      // Return number of mem tables that are ready to flush (made immutable)
      *value = (cfd_->imm()->IsFlushPending() ? 1 : 0);
      return true;
    case kCompactionPending:
      // 1 if the system already determines at least one compacdtion is needed.
      // 0 otherwise,
      *value = (cfd_->compaction_picker()->NeedsCompaction(vstorage) ? 1 : 0);
      return true;
    case kBackgroundErrors:
      // Accumulated number of  errors in background flushes or compactions.
      *value = GetBackgroundErrorCount();
      return true;
    case kCurSizeActiveMemTable:
      // Current size of the active memtable
      *value = cfd_->mem()->ApproximateMemoryUsage();
      return true;
    case kCurSizeAllMemTables:
      // Current size of the active memtable + immutable memtables
      *value = cfd_->mem()->ApproximateMemoryUsage() +
               cfd_->imm()->ApproximateMemoryUsage();
      return true;
    case kNumEntriesInMutableMemtable:
      // Current number of entires in the active memtable
      *value = cfd_->mem()->GetNumEntries();
      return true;
    case kNumEntriesInImmutableMemtable:
      // Current number of entries in the immutable memtables
      *value = cfd_->imm()->current()->GetTotalNumEntries();
      return true;
    case kEstimatedNumKeys:
      // Estimate number of entries in the column family:
      // Use estimated entries in tables + total entries in memtables.
      *value = cfd_->mem()->GetNumEntries() +
               cfd_->imm()->current()->GetTotalNumEntries() +
               vstorage->GetEstimatedActiveKeys();
      return true;
    case kNumSnapshots:
      *value = db->snapshots().count();
      return true;
    case kOldestSnapshotTime:
      *value = static_cast<uint64_t>(db->snapshots().GetOldestSnapshotTime());
      return true;
    case kNumLiveVersions:
      *value = cfd_->GetNumLiveVersions();
      return true;
#ifndef ROCKSDB_LITE
    case kIsFileDeletionEnabled:
      *value = db->IsFileDeletionsEnabled();
      return true;
#endif
    default:
      return false;
  }
}

void InternalStats::DumpDBStats(std::string* value) {
  char buf[1000];
  // DB-level stats, only available from default column family
  double seconds_up = (env_->NowMicros() - started_at_ + 1) / 1000000.0;
  double interval_seconds_up = seconds_up - db_stats_snapshot_.seconds_up;
  snprintf(buf, sizeof(buf),
           "\n** DB Stats **\nUptime(secs): %.1f total, %.1f interval\n",
           seconds_up, interval_seconds_up);
  value->append(buf);
  // Cumulative
  uint64_t user_bytes_written = db_stats_[InternalStats::BYTES_WRITTEN];
  uint64_t num_keys_written = db_stats_[InternalStats::NUMBER_KEYS_WRITTEN];
  uint64_t write_other = db_stats_[InternalStats::WRITE_DONE_BY_OTHER];
  uint64_t write_self = db_stats_[InternalStats::WRITE_DONE_BY_SELF];
  uint64_t wal_bytes = db_stats_[InternalStats::WAL_FILE_BYTES];
  uint64_t wal_synced = db_stats_[InternalStats::WAL_FILE_SYNCED];
  uint64_t write_with_wal = db_stats_[InternalStats::WRITE_WITH_WAL];
  uint64_t write_stall_micros = db_stats_[InternalStats::WRITE_STALL_MICROS];
  // Data
  // writes: total number of write requests.
  // keys: total number of key updates issued by all the write requests
  // batches: number of group commits issued to the DB. Each group can contain
  //          one or more writes.
  // so writes/keys is the average number of put in multi-put or put
  // writes/batches is the average group commit size.
  //
  // The format is the same for interval stats.
  snprintf(buf, sizeof(buf),
           "Cumulative writes: %" PRIu64 " writes, %" PRIu64 " keys, %" PRIu64
           " batches, %.1f writes per batch, %.2f GB user ingest, "
           "stall micros: %" PRIu64 "\n",
           write_other + write_self, num_keys_written, write_self,
           (write_other + write_self) / static_cast<double>(write_self + 1),
           user_bytes_written / kGB, write_stall_micros);
  value->append(buf);
  // WAL
  snprintf(buf, sizeof(buf),
           "Cumulative WAL: %" PRIu64 " writes, %" PRIu64 " syncs, "
           "%.2f writes per sync, %.2f GB written\n",
           write_with_wal, wal_synced,
           write_with_wal / static_cast<double>(wal_synced + 1),
           wal_bytes / kGB);
  value->append(buf);

  // Interval
  uint64_t interval_write_other = write_other - db_stats_snapshot_.write_other;
  uint64_t interval_write_self = write_self - db_stats_snapshot_.write_self;
  uint64_t interval_num_keys_written =
      num_keys_written - db_stats_snapshot_.num_keys_written;
  snprintf(buf, sizeof(buf),
           "Interval writes: %" PRIu64 " writes, %" PRIu64 " keys, %" PRIu64
           " batches, %.1f writes per batch, %.1f MB user ingest, "
           "stall micros: %" PRIu64 "\n",
           interval_write_other + interval_write_self,
           interval_num_keys_written, interval_write_self,
           static_cast<double>(interval_write_other + interval_write_self) /
               (interval_write_self + 1),
           (user_bytes_written - db_stats_snapshot_.ingest_bytes) / kMB,
           write_stall_micros - db_stats_snapshot_.write_stall_micros);
  value->append(buf);

  uint64_t interval_write_with_wal =
      write_with_wal - db_stats_snapshot_.write_with_wal;
  uint64_t interval_wal_synced = wal_synced - db_stats_snapshot_.wal_synced;
  uint64_t interval_wal_bytes = wal_bytes - db_stats_snapshot_.wal_bytes;

  snprintf(buf, sizeof(buf),
           "Interval WAL: %" PRIu64 " writes, %" PRIu64 " syncs, "
           "%.2f writes per sync, %.2f MB written\n",
           interval_write_with_wal,
           interval_wal_synced,
           interval_write_with_wal /
              static_cast<double>(interval_wal_synced + 1),
           interval_wal_bytes / kGB);
  value->append(buf);

  db_stats_snapshot_.seconds_up = seconds_up;
  db_stats_snapshot_.ingest_bytes = user_bytes_written;
  db_stats_snapshot_.write_other = write_other;
  db_stats_snapshot_.write_self = write_self;
  db_stats_snapshot_.num_keys_written = num_keys_written;
  db_stats_snapshot_.wal_bytes = wal_bytes;
  db_stats_snapshot_.wal_synced = wal_synced;
  db_stats_snapshot_.write_with_wal = write_with_wal;
  db_stats_snapshot_.write_stall_micros = write_stall_micros;
}

void InternalStats::DumpCFStats(std::string* value) {
  const VersionStorageInfo* vstorage = cfd_->current()->storage_info();

  int num_levels_to_check =
      (cfd_->ioptions()->compaction_style != kCompactionStyleUniversal &&
       cfd_->ioptions()->compaction_style != kCompactionStyleFIFO)
          ? vstorage->num_levels() - 1
          : 1;

  // Compaction scores are sorted base on its value. Restore them to the
  // level order
  std::vector<double> compaction_score(number_levels_, 0);
  for (int i = 0; i < num_levels_to_check; ++i) {
    compaction_score[vstorage->CompactionScoreLevel(i)] =
        vstorage->CompactionScore(i);
  }
  // Count # of files being compacted for each level
  std::vector<int> files_being_compacted(number_levels_, 0);
  for (int level = 0; level < num_levels_to_check; ++level) {
    for (auto* f : vstorage->LevelFiles(level)) {
      if (f->being_compacted) {
        ++files_being_compacted[level];
      }
    }
  }

  char buf[1000];
  // Per-ColumnFamily stats
  PrintLevelStatsHeader(buf, sizeof(buf), cfd_->GetName());
  value->append(buf);

  CompactionStats stats_sum(0);
  int total_files = 0;
  int total_files_being_compacted = 0;
  double total_file_size = 0;
  uint64_t total_slowdown_soft = 0;
  uint64_t total_slowdown_count_soft = 0;
  uint64_t total_slowdown_hard = 0;
  uint64_t total_slowdown_count_hard = 0;
  uint64_t total_stall_count = 0;
  double total_stall_us = 0;
  for (int level = 0; level < number_levels_; level++) {
    int files = vstorage->NumLevelFiles(level);
    total_files += files;
    total_files_being_compacted += files_being_compacted[level];
    if (comp_stats_[level].micros > 0 || files > 0) {
      uint64_t stalls = level == 0 ?
        (cf_stats_count_[LEVEL0_SLOWDOWN] +
         cf_stats_count_[LEVEL0_NUM_FILES] +
         cf_stats_count_[MEMTABLE_COMPACTION])
        : (stall_leveln_slowdown_count_soft_[level] +
           stall_leveln_slowdown_count_hard_[level]);

      double stall_us = level == 0 ?
         (cf_stats_value_[LEVEL0_SLOWDOWN] +
          cf_stats_value_[LEVEL0_NUM_FILES] +
          cf_stats_value_[MEMTABLE_COMPACTION])
         : (stall_leveln_slowdown_soft_[level] +
            stall_leveln_slowdown_hard_[level]);

      stats_sum.Add(comp_stats_[level]);
      total_file_size += vstorage->NumLevelBytes(level);
      total_stall_us += stall_us;
      total_stall_count += stalls;
      total_slowdown_soft += stall_leveln_slowdown_soft_[level];
      total_slowdown_count_soft += stall_leveln_slowdown_count_soft_[level];
      total_slowdown_hard += stall_leveln_slowdown_hard_[level];
      total_slowdown_count_hard += stall_leveln_slowdown_count_hard_[level];
      double w_amp = (comp_stats_[level].bytes_readn == 0) ? 0.0
          : comp_stats_[level].bytes_written /
            static_cast<double>(comp_stats_[level].bytes_readn);
      PrintLevelStats(buf, sizeof(buf), "L" + ToString(level), files,
                      files_being_compacted[level],
                      vstorage->NumLevelBytes(level), compaction_score[level],
                      w_amp, stall_us, stalls, comp_stats_[level]);
      value->append(buf);
    }
  }
  uint64_t curr_ingest = cf_stats_value_[BYTES_FLUSHED];
  // Cumulative summary
  double w_amp = stats_sum.bytes_written / static_cast<double>(curr_ingest + 1);
  // Stats summary across levels
  PrintLevelStats(buf, sizeof(buf), "Sum", total_files,
      total_files_being_compacted, total_file_size, 0, w_amp,
      total_stall_us, total_stall_count, stats_sum);
  value->append(buf);
  // Interval summary
  uint64_t interval_ingest =
      curr_ingest - cf_stats_snapshot_.ingest_bytes + 1;
  CompactionStats interval_stats(stats_sum);
  interval_stats.Subtract(cf_stats_snapshot_.comp_stats);
  w_amp = interval_stats.bytes_written / static_cast<double>(interval_ingest);
  PrintLevelStats(buf, sizeof(buf), "Int", 0, 0, 0, 0,
      w_amp, total_stall_us - cf_stats_snapshot_.stall_us,
      total_stall_count - cf_stats_snapshot_.stall_count, interval_stats);
  value->append(buf);

  snprintf(buf, sizeof(buf),
           "Flush(GB): accumulative %.3f, interval %.3f\n",
           curr_ingest / kGB, interval_ingest / kGB);
  value->append(buf);
  snprintf(buf, sizeof(buf),
           "Stalls(secs): %.3f level0_slowdown, %.3f level0_numfiles, "
           "%.3f memtable_compaction, %.3f leveln_slowdown_soft, "
           "%.3f leveln_slowdown_hard\n",
           cf_stats_value_[LEVEL0_SLOWDOWN] / 1000000.0,
           cf_stats_value_[LEVEL0_NUM_FILES] / 1000000.0,
           cf_stats_value_[MEMTABLE_COMPACTION] / 1000000.0,
           total_slowdown_soft / 1000000.0,
           total_slowdown_hard / 1000000.0);
  value->append(buf);

  snprintf(buf, sizeof(buf),
           "Stalls(count): %" PRIu64 " level0_slowdown, "
           "%" PRIu64 " level0_numfiles, %" PRIu64 " memtable_compaction, "
           "%" PRIu64 " leveln_slowdown_soft, "
           "%" PRIu64 " leveln_slowdown_hard\n",
           cf_stats_count_[LEVEL0_SLOWDOWN],
           cf_stats_count_[LEVEL0_NUM_FILES],
           cf_stats_count_[MEMTABLE_COMPACTION],
           total_slowdown_count_soft, total_slowdown_count_hard);
  value->append(buf);

  cf_stats_snapshot_.ingest_bytes = curr_ingest;
  cf_stats_snapshot_.comp_stats = stats_sum;
  cf_stats_snapshot_.stall_us = total_stall_us;
  cf_stats_snapshot_.stall_count = total_stall_count;
}


#else

DBPropertyType GetPropertyType(const Slice& property, bool* is_int_property,
                               bool* need_out_of_mutex) {
  return kUnknown;
}

#endif  // !ROCKSDB_LITE

}  // namespace rocksdb
