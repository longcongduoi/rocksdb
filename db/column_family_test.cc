//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"
#include "rocksdb/env.h"
#include "rocksdb/db.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "utilities/merge_operators.h"

#include <algorithm>
#include <vector>
#include <string>

namespace rocksdb {

using namespace std;

namespace {
std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}
}  // anonymous namespace

class ColumnFamilyTest {
 public:
  ColumnFamilyTest() : rnd_(139) {
    env_ = Env::Default();
    dbname_ = test::TmpDir() + "/column_family_test";
    db_options_.create_if_missing = true;
    DestroyDB(dbname_, Options(db_options_, column_family_options_));
  }

  void Close() {
    for (auto h : handles_) {
      delete h;
    }
    handles_.clear();
    delete db_;
    db_ = nullptr;
  }

  Status Open() {
    return Open({"default"});
  }

  Status Open(vector<string> cf) {
    vector<ColumnFamilyDescriptor> column_families;
    for (auto x : cf) {
      column_families.push_back(
          ColumnFamilyDescriptor(x, column_family_options_));
    }
    return DB::Open(db_options_, dbname_, column_families, &handles_, &db_);
  }

  DBImpl* dbfull() { return reinterpret_cast<DBImpl*>(db_); }

  void Destroy() {
    for (auto h : handles_) {
      delete h;
    }
    handles_.clear();
    delete db_;
    db_ = nullptr;
    ASSERT_OK(DestroyDB(dbname_, Options(db_options_, column_family_options_)));
  }

  void CreateColumnFamilies(const vector<string>& cfs) {
    int cfi = handles_.size();
    handles_.resize(cfi + cfs.size());
    for (auto cf : cfs) {
      ASSERT_OK(db_->CreateColumnFamily(column_family_options_, cf,
                                        &handles_[cfi++]));
    }
  }

  void DropColumnFamilies(const vector<int>& cfs) {
    for (auto cf : cfs) {
      ASSERT_OK(db_->DropColumnFamily(handles_[cf]));
      delete handles_[cf];
      handles_[cf] = nullptr;
    }
  }

  void PutRandomData(int cf, int bytes) {
    int num_insertions = (bytes + 99) / 100;
    for (int i = 0; i < num_insertions; ++i) {
      // 10 bytes key, 90 bytes value
      ASSERT_OK(Put(cf, test::RandomKey(&rnd_, 10), RandomString(&rnd_, 90)));
    }
  }

  void WaitForFlush(int cf) {
    ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable(handles_[cf]));
  }

  Status Put(int cf, const string& key, const string& value) {
    return db_->Put(WriteOptions(), handles_[cf], Slice(key), Slice(value));
  }
  Status Merge(int cf, const string& key, const string& value) {
    return db_->Merge(WriteOptions(), handles_[cf], Slice(key), Slice(value));
  }
  Status Flush(int cf) {
    return db_->Flush(FlushOptions(), handles_[cf]);
  }

  string Get(int cf, const string& key) {
    ReadOptions options;
    options.verify_checksums = true;
    string result;
    Status s = db_->Get(options, handles_[cf], Slice(key), &result);
    if (s.IsNotFound()) {
      result = "NOT_FOUND";
    } else if (!s.ok()) {
      result = s.ToString();
    }
    return result;
  }

  void Compact(int cf, const Slice& start, const Slice& limit) {
    ASSERT_OK(db_->CompactRange(handles_[cf], &start, &limit));
  }

  int NumTableFilesAtLevel(int cf, int level) {
    string property;
    ASSERT_TRUE(db_->GetProperty(
        handles_[cf], "rocksdb.num-files-at-level" + NumberToString(level),
        &property));
    return atoi(property.c_str());
  }

  // Return spread of files per level
  string FilesPerLevel(int cf) {
    string result;
    int last_non_zero_offset = 0;
    for (int level = 0; level < column_family_options_.num_levels; level++) {
      int f = NumTableFilesAtLevel(cf, level);
      char buf[100];
      snprintf(buf, sizeof(buf), "%s%d", (level ? "," : ""), f);
      result += buf;
      if (f > 0) {
        last_non_zero_offset = result.size();
      }
    }
    result.resize(last_non_zero_offset);
    return result;
  }

  int CountLiveFiles(int cf) {
    std::vector<LiveFileMetaData> metadata;
    db_->GetLiveFilesMetaData(&metadata);
    return static_cast<int>(metadata.size());
  }

  // Do n memtable flushes, each of which produces an sstable
  // covering the range [small,large].
  void MakeTables(int cf, int n, const string& small,
                  const string& large) {
    for (int i = 0; i < n; i++) {
      ASSERT_OK(Put(cf, small, "begin"));
      ASSERT_OK(Put(cf, large, "end"));
      ASSERT_OK(db_->Flush(FlushOptions(), handles_[cf]));
    }
  }

  int CountLiveLogFiles() {
    int ret = 0;
    VectorLogPtr wal_files;
    ASSERT_OK(db_->GetSortedWalFiles(wal_files));
    for (const auto& wal : wal_files) {
      if (wal->Type() == kAliveLogFile) {
        ++ret;
      }
    }
    return ret;
  }

  void CopyFile(const string& source, const string& destination,
                uint64_t size = 0) {
    const EnvOptions soptions;
    unique_ptr<SequentialFile> srcfile;
    ASSERT_OK(env_->NewSequentialFile(source, &srcfile, soptions));
    unique_ptr<WritableFile> destfile;
    ASSERT_OK(env_->NewWritableFile(destination, &destfile, soptions));

    if (size == 0) {
      // default argument means copy everything
      ASSERT_OK(env_->GetFileSize(source, &size));
    }

    char buffer[4096];
    Slice slice;
    while (size > 0) {
      uint64_t one = min(uint64_t(sizeof(buffer)), size);
      ASSERT_OK(srcfile->Read(one, &slice, buffer));
      ASSERT_OK(destfile->Append(slice));
      size -= slice.size();
    }
    ASSERT_OK(destfile->Close());
  }

  vector<ColumnFamilyHandle*> handles_;
  ColumnFamilyOptions column_family_options_;
  DBOptions db_options_;
  string dbname_;
  DB* db_ = nullptr;
  Env* env_;
  Random rnd_;
};

TEST(ColumnFamilyTest, AddDrop) {
  ASSERT_OK(Open({"default"}));
  CreateColumnFamilies({"one", "two", "three"});
  DropColumnFamilies({2});
  CreateColumnFamilies({"four"});
  Close();
  ASSERT_TRUE(Open({"default"}).IsInvalidArgument());
  ASSERT_OK(Open({"default", "one", "three", "four"}));
  Close();

  vector<string> families;
  ASSERT_OK(DB::ListColumnFamilies(db_options_, dbname_, &families));
  sort(families.begin(), families.end());
  ASSERT_TRUE(families == vector<string>({"default", "four", "one", "three"}));
}

TEST(ColumnFamilyTest, DropTest) {
  // first iteration - dont reopen DB before dropping
  // second iteration - reopen DB before dropping
  for (int iter = 0; iter < 2; ++iter) {
    ASSERT_OK(Open({"default"}));
    CreateColumnFamilies({"pikachu"});
    Close();
    ASSERT_OK(Open({"default", "pikachu"}));
    for (int i = 0; i < 100; ++i) {
      ASSERT_OK(Put(1, std::to_string(i), "bar" + std::to_string(i)));
    }
    ASSERT_OK(Flush(1));

    if (iter == 1) {
      Close();
      ASSERT_OK(Open({"default", "pikachu"}));
    }
    ASSERT_EQ("bar1", Get(1, "1"));

    ASSERT_EQ(CountLiveFiles(1), 1);
    DropColumnFamilies({1});
    // make sure that all files are deleted when we drop the column family
    ASSERT_EQ(CountLiveFiles(1), 0);
    Destroy();
  }
}

TEST(ColumnFamilyTest, WriteBatchFailure) {
  Open();
  WriteBatch batch;
  batch.Put(1, Slice("non-existing"), Slice("column-family"));
  Status s = db_->Write(WriteOptions(), &batch);
  ASSERT_TRUE(s.IsInvalidArgument());
  CreateColumnFamilies({"one"});
  ASSERT_OK(db_->Write(WriteOptions(), &batch));
  Close();
}

TEST(ColumnFamilyTest, ReadWrite) {
  ASSERT_OK(Open({"default"}));
  CreateColumnFamilies({"one", "two"});
  Close();
  ASSERT_OK(Open({"default", "one", "two"}));
  ASSERT_OK(Put(0, "foo", "v1"));
  ASSERT_OK(Put(0, "bar", "v2"));
  ASSERT_OK(Put(1, "mirko", "v3"));
  ASSERT_OK(Put(0, "foo", "v2"));
  ASSERT_OK(Put(2, "fodor", "v5"));

  for (int iter = 0; iter <= 3; ++iter) {
    ASSERT_EQ("v2", Get(0, "foo"));
    ASSERT_EQ("v2", Get(0, "bar"));
    ASSERT_EQ("v3", Get(1, "mirko"));
    ASSERT_EQ("v5", Get(2, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(0, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(1, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(2, "foo"));
    if (iter <= 1) {
      // reopen
      Close();
      ASSERT_OK(Open({"default", "one", "two"}));
    }
  }
  Close();
}

TEST(ColumnFamilyTest, IgnoreRecoveredLog) {
  string backup_logs = dbname_ + "/backup_logs";

  // delete old files in backup_logs directory
  ASSERT_OK(env_->CreateDirIfMissing(dbname_));
  ASSERT_OK(env_->CreateDirIfMissing(backup_logs));
  vector<string> old_files;
  env_->GetChildren(backup_logs, &old_files);
  for (auto& file : old_files) {
    if (file != "." && file != "..") {
      env_->DeleteFile(backup_logs + "/" + file);
    }
  }

  column_family_options_.merge_operator =
      MergeOperators::CreateUInt64AddOperator();
  db_options_.wal_dir = dbname_ + "/logs";
  Destroy();
  ASSERT_OK(Open({"default"}));
  CreateColumnFamilies({"cf1", "cf2"});

  // fill up the DB
  string one, two, three;
  PutFixed64(&one, 1);
  PutFixed64(&two, 2);
  PutFixed64(&three, 3);
  ASSERT_OK(Merge(0, "foo", one));
  ASSERT_OK(Merge(1, "mirko", one));
  ASSERT_OK(Merge(0, "foo", one));
  ASSERT_OK(Merge(2, "bla", one));
  ASSERT_OK(Merge(2, "fodor", one));
  ASSERT_OK(Merge(0, "bar", one));
  ASSERT_OK(Merge(2, "bla", one));
  ASSERT_OK(Merge(1, "mirko", two));
  ASSERT_OK(Merge(1, "franjo", one));

  // copy the logs to backup
  vector<string> logs;
  env_->GetChildren(db_options_.wal_dir, &logs);
  for (auto& log : logs) {
    if (log != ".." && log != ".") {
      CopyFile(db_options_.wal_dir + "/" + log, backup_logs + "/" + log);
    }
  }

  // recover the DB
  Close();

  // 1. check consistency
  // 2. copy the logs from backup back to WAL dir. if the recovery happens
  // again on the same log files, this should lead to incorrect results
  // due to applying merge operator twice
  // 3. check consistency
  for (int iter = 0; iter < 2; ++iter) {
    // assert consistency
    ASSERT_OK(Open({"default", "cf1", "cf2"}));
    ASSERT_EQ(two, Get(0, "foo"));
    ASSERT_EQ(one, Get(0, "bar"));
    ASSERT_EQ(three, Get(1, "mirko"));
    ASSERT_EQ(one, Get(1, "franjo"));
    ASSERT_EQ(one, Get(2, "fodor"));
    ASSERT_EQ(two, Get(2, "bla"));
    Close();

    if (iter == 0) {
      // copy the logs from backup back to wal dir
      for (auto& log : logs) {
        if (log != ".." && log != ".") {
          CopyFile(backup_logs + "/" + log, db_options_.wal_dir + "/" + log);
        }
      }
    }
  }
}

TEST(ColumnFamilyTest, FlushTest) {
  ASSERT_OK(Open({"default"}));
  CreateColumnFamilies({"one", "two"});
  Close();
  ASSERT_OK(Open({"default", "one", "two"}));
  ASSERT_OK(Put(0, "foo", "v1"));
  ASSERT_OK(Put(0, "bar", "v2"));
  ASSERT_OK(Put(1, "mirko", "v3"));
  ASSERT_OK(Put(0, "foo", "v2"));
  ASSERT_OK(Put(2, "fodor", "v5"));
  for (int i = 0; i < 3; ++i) {
    Flush(i);
  }
  Close();
  ASSERT_OK(Open({"default", "one", "two"}));

  for (int iter = 0; iter <= 2; ++iter) {
    ASSERT_EQ("v2", Get(0, "foo"));
    ASSERT_EQ("v2", Get(0, "bar"));
    ASSERT_EQ("v3", Get(1, "mirko"));
    ASSERT_EQ("v5", Get(2, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(0, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(1, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(2, "foo"));
    if (iter <= 1) {
      // reopen
      Close();
      ASSERT_OK(Open({"default", "one", "two"}));
    }
  }
  Close();
}

// Makes sure that obsolete log files get deleted
TEST(ColumnFamilyTest, LogDeletionTest) {
  column_family_options_.write_buffer_size = 100000;  // 100KB
  ASSERT_OK(Open());
  CreateColumnFamilies({"one", "two", "three", "four"});
  // Each bracket is one log file. if number is in (), it means
  // we don't need it anymore (it's been flushed)
  // []
  ASSERT_EQ(CountLiveLogFiles(), 0);
  PutRandomData(0, 100);
  // [0]
  PutRandomData(1, 100);
  // [0, 1]
  PutRandomData(1, 100000);
  WaitForFlush(1);
  // [0, (1)] [1]
  ASSERT_EQ(CountLiveLogFiles(), 2);
  PutRandomData(0, 100);
  // [0, (1)] [0, 1]
  ASSERT_EQ(CountLiveLogFiles(), 2);
  PutRandomData(2, 100);
  // [0, (1)] [0, 1, 2]
  PutRandomData(2, 100000);
  WaitForFlush(2);
  // [0, (1)] [0, 1, (2)] [2]
  ASSERT_EQ(CountLiveLogFiles(), 3);
  PutRandomData(2, 100000);
  WaitForFlush(2);
  // [0, (1)] [0, 1, (2)] [(2)] [2]
  ASSERT_EQ(CountLiveLogFiles(), 4);
  PutRandomData(3, 100);
  // [0, (1)] [0, 1, (2)] [(2)] [2, 3]
  PutRandomData(1, 100);
  // [0, (1)] [0, 1, (2)] [(2)] [1, 2, 3]
  ASSERT_EQ(CountLiveLogFiles(), 4);
  PutRandomData(1, 100000);
  WaitForFlush(1);
  // [0, (1)] [0, (1), (2)] [(2)] [(1), 2, 3] [1]
  ASSERT_EQ(CountLiveLogFiles(), 5);
  PutRandomData(0, 100000);
  WaitForFlush(0);
  // [(0), (1)] [(0), (1), (2)] [(2)] [(1), 2, 3] [1, (0)] [0]
  // delete obsolete logs -->
  // [(1), 2, 3] [1, (0)] [0]
  ASSERT_EQ(CountLiveLogFiles(), 3);
  PutRandomData(0, 100000);
  WaitForFlush(0);
  // [(1), 2, 3] [1, (0)], [(0)] [0]
  ASSERT_EQ(CountLiveLogFiles(), 4);
  PutRandomData(1, 100000);
  WaitForFlush(1);
  // [(1), 2, 3] [(1), (0)] [(0)] [0, (1)] [1]
  ASSERT_EQ(CountLiveLogFiles(), 5);
  PutRandomData(2, 100000);
  WaitForFlush(2);
  // [(1), (2), 3] [(1), (0)] [(0)] [0, (1)] [1, (2)], [2]
  ASSERT_EQ(CountLiveLogFiles(), 6);
  PutRandomData(3, 100000);
  WaitForFlush(3);
  // [(1), (2), (3)] [(1), (0)] [(0)] [0, (1)] [1, (2)], [2, (3)] [3]
  // delete obsolete logs -->
  // [0, (1)] [1, (2)], [2, (3)] [3]
  ASSERT_EQ(CountLiveLogFiles(), 4);
  Close();
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  return rocksdb::test::RunAllTests();
}
