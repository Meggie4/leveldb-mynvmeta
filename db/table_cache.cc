// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/coding.h"

///////////meggie
#include "db/meta.h"
#include "util/debug.h"

int table_find_counts = 0;
int table_not_find_counts = 0;
///////////meggie

namespace leveldb {

struct TableAndFile {
  RandomAccessFile* file;
  Table* table;
};

static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

TableCache::TableCache(const std::string& dbname,
                       const Options& options,
                       int entries)
    : env_(options.env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {
}

TableCache::~TableCache() {
  delete cache_;
}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
  *handle = cache_->Lookup(key);
  if (*handle == nullptr) {
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = nullptr;
    Table* table = nullptr;
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
    }
    if (s.ok()) {
      s = Table::Open(options_, file, file_size, &table);
    }

    if (!s.ok()) {
      assert(table == nullptr);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number,
                                  uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != nullptr) {
    *tableptr = nullptr;
  }

  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options);
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  if (tableptr != nullptr) {
    *tableptr = table;
  }
  return result;
}

Status TableCache::Get(const ReadOptions& options,
                       uint64_t file_number,
                       uint64_t file_size,
                       const Slice& k,
                       void* arg,
                       void (*saver)(void*, const Slice&, const Slice&)) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    s = t->InternalGet(options, k, arg, saver);
    cache_->Release(handle);
  }
  return s;
}

void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

/////////////////meggie 
void TableCache::EvictByMeta(META* meta, uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
  meta->evict_chunk(file_number);
}

Status TableCache::FindTableByMeta(uint64_t file_number, 
                                   uint64_t file_size,
                                   META* meta, 
                                   Cache::Handle** handle) {
    Status s;
    char buf[sizeof(file_number)];
    EncodeFixed64(buf, file_number);
    Slice key(buf, sizeof(buf));
    *handle = cache_->Lookup(key);
    if (*handle == nullptr) {
        //////////meggie
        table_not_find_counts++;
        //////////meggie
        std::string fname  = TableFileName(dbname_, file_number);
        RandomAccessFile* file = nullptr;
        Table* table = nullptr;
        Status s;
        s = env_->NewRandomAccessFile(fname, &file);
        
        if (!s.ok()) {
          std::string old_fname = SSTTableFileName(dbname_, file_number);
          if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
            s = Status::OK();
          }
        }

        if (s.ok()) {
          //DEBUG_T("GetByMeta, to alloc_chunk, file_number:%llu\n", file_number);
          META_Chunk* mchunk = meta->alloc_chunk(file_number);
          s = Table::OpenByMeta(options_, file, file_number, 
                                file_size, mchunk, &table);
        }

        if(!s.ok()) {
            assert(table == nullptr);
            delete file;
        } else {
            TableAndFile* tf = new TableAndFile;
            tf->file = file;
            tf->table = table;
            *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
        }
    } else {
        //////////meggie
        table_find_counts++;
        //////////meggie
    }
    return s;
}


Status TableCache::GetByMeta(const ReadOptions& options, 
                    uint64_t file_number,
                    uint64_t file_size,
                    META* meta,
                    const Slice& k,
                    void* arg,
                    void (*saver)(void*, const Slice&, const Slice&)) {
    Cache::Handle* handle = nullptr;
    Status s = FindTableByMeta(file_number, file_size, meta, &handle);
    
    if(s.ok()) {
        Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
        s = t->InternalGet(options, k, arg, saver);
        cache_->Release(handle);
    }
    
    return s;
}
  
Iterator* TableCache::NewIteratorByMeta(const ReadOptions& options,
                                  uint64_t file_number,
                                  uint64_t file_size, 
                                  META* meta,
                                  Table** tableptr) {
    if(tableptr != nullptr) {
        *tableptr = nullptr;
    }
    
    Cache::Handle* handle = nullptr;

    Status s = FindTableByMeta(file_number, file_size, meta, &handle);

    if(!s.ok()) {
        return NewErrorIterator(s);
    }

    Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    Iterator* result = table->NewIterator(options);
    result->RegisterCleanup(&UnrefEntry, cache_, handle);

    if(tableptr != nullptr) {
        *tableptr = table;
    }
    
    return result;
}
////////////////meggie

}  // namespace leveldb
