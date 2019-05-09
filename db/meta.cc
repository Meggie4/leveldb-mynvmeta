#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "db/meta.h"
#include "port/cache_flush.h"
#include "util/debug.h"
#include "table/format.h"

struct bplus_tree_config{
    int order;
    int entries;
};

namespace leveldb{
    META_Chunk::META_Chunk(uint64_t number, uint64_t index, char* addr)
    : number_(number),
      index_(index),
      addr_(addr),
      length_ptr_(nullptr),
      read_ptr_(addr_),
      size_(0) {
          length_ptr_ = addr_ + 16;
          current_ptr_ = length_ptr_ + 8;
          size_ = 24;
    }
   
    uint64_t META_Chunk::append(const void* ptr, uint64_t size) {
        if((size_ + size) > CHUNK_SIZE) {
            fprintf(stderr, "meta.cc append, overflow exit, name=%06llu.ldb, chunk->size_:%llu, size:%llu\n",
                    static_cast<unsigned long long>(number_), size_, size);
            exit(9);
        }
        
        memcpy(current_ptr_, ptr, size);
        current_ptr_ += size;
        size_ += size;
        
        return (current_ptr_ - addr_);
    }

    void META_Chunk::set_length(uint64_t len) {
        *((uint64_t*)length_ptr_) = len;
        length_ptr_ = current_ptr_;
        current_ptr_ = length_ptr_ + 8;
        size_ += 8;
    }

    
    void META_Chunk::flush() {
       DEBUG_T("flush, size_:%llu\n", size_ - 8);
       *((uint64_t*)addr_) = kTableMagicNumber;
       *((uint64_t*)(addr_ + 8)) = number_;
       flush_cache(addr_, size_ - 8); 
    }

    META::META(const std::string& nvm_file, uint64_t file_size, bool recovery) 
        : chunk_amount_(0),
          remaining_amount_(0),
          size_(file_size),
          chunk_offset_(0),
          fd_(-1),
          mmap_start_(nullptr),
          OnlineMap_(nullptr),
          index_tree_(nullptr) {
        /////mmap open fd;
        DEBUG_T("-----------------META SETUP---------------\n");
        fd_ = open(nvm_file.c_str(), O_RDWR);
        
        if(fd_ == -1) {
            DEBUG_T("create nvm_file:%s\n", nvm_file.c_str());
            fd_ = open(nvm_file.c_str(), O_RDWR | O_CREAT, 0664);
            if(fd_ < 0) {
                fprintf(stderr, "create nvm_file failed\n");
                exit(9);
            }
        }
        
        if(ftruncate(fd_, size_) != 0) {
            perror("ftruncate failed\n");
            DEBUG_T("size_:%llu\n", size_);
            exit(9);
        }
       
        /////config tree;
        struct bplus_tree_config config;
        config.order = 7;
        config.entries = 10;
        index_tree_ = bplus_tree_init(config.order, 
                config.entries);
        if(index_tree_ == nullptr) {
            perror("index bplus tree create failed\n");
            exit(9);
        }
        DEBUG_T("success to setup bplustree\n");
        ///mmap
        mmap_start_ = (char*) mmap(nullptr, size_, PROT_READ|PROT_WRITE,
                MAP_SHARED, fd_, 0);
        char* current_ptr = mmap_start_;
        
        if(recovery) {
            chunk_amount_ = *((uint64_t*)current_ptr);
            current_ptr += 8; 
            remaining_amount_ = *((uint64_t*)current_ptr);
            current_ptr += 8;
            OnlineMap_ = (char*)malloc(chunk_amount_);
            memset(OnlineMap_, 0, chunk_amount_);
            memcpy(OnlineMap_, current_ptr, chunk_amount_);
            chunk_offset_ = chunk_amount_ + 16;
        } else {
            uint64_t estimate_chunk_amount = size_ % CHUNK_SIZE;
            chunk_amount_ = (size_ - (8 + 8 + estimate_chunk_amount)) % CHUNK_SIZE;
            
            remaining_amount_ = chunk_amount_;
            DEBUG_T("chunk_amount:%lld, remaining_amount:%lld\n", 
                    chunk_amount_, remaining_amount_);
            //reserved space 
            //8(chunk_amount) + 8(remaining_amount) + chunk_amount_(OnlineMap_)
            *current_ptr = chunk_amount_;
            current_ptr += 8;
            *current_ptr += remaining_amount_;
            current_ptr += 8;
            OnlineMap_ = (char*)malloc(chunk_amount_);
            memset(OnlineMap_, 0, chunk_amount_);
            chunk_offset_ = chunk_amount_ + 16;
        }
        DEBUG_T("------------END META SETUP-----------\n");
    }

    META::~META() {
        if(mmap_start_){
            munmap(mmap_start_, size_);
        }
        close(fd_);
        free(OnlineMap_);
        bplus_tree_deinit(index_tree_);
    }
    
    inline uint64_t META::get_chunk_index(uint64_t number) {
        return bplus_tree_get(index_tree_, number);
    }

    inline void META::update_chunk_index(uint64_t number, uint64_t index) {
        bplus_tree_put(index_tree_, number, index);
    }

    bool META::reserve_chunk(uint64_t number) {
        //get number, 
        uint64_t result = number % chunk_amount_, tmp;
        
        if(remaining_amount_ <= 0){
            perror("there is not enough remaining free chunk\n");
            return false;
        }

        
        for(tmp = result; tmp < chunk_amount_; tmp++) {
            if(OnlineMap_[tmp] == 0) {
                OnlineMap_[tmp] = 1;
                remaining_amount_--;
                update_chunk_index(number, tmp);
                return true;
            }
        }

        for(tmp = 0; tmp < result; tmp++) {
            if(OnlineMap_[tmp] == 0) {
                OnlineMap_[tmp] = 1;
                remaining_amount_--;
                update_chunk_index(number, tmp);
                return true;
            }
        }
    }

    bool META::reserve_and_alloc_chunk(uint64_t number, 
            META_Chunk** mchunk) {
        //get number, 
        uint64_t result = number % chunk_amount_, tmp;
        
        if(remaining_amount_ <= 0){
            perror("there is not enough remaining free chunk\n");
            return false;
        }

        for(tmp = result; tmp < chunk_amount_; tmp++) {
            if(OnlineMap_[tmp] == 0) {
                OnlineMap_[tmp] = 1;
                remaining_amount_--;
                char* phy_addr = mmap_start_ + chunk_offset_ + tmp * CHUNK_SIZE; 
                *mchunk = new META_Chunk(number, tmp, phy_addr);
                update_chunk_index(number, tmp);
                uint64_t tree_index = get_chunk_index(number);
                DEBUG_T("success to get META_Chunk, number:%llu, tmp:%llu, tree_index:%llu\n", number, tmp, tree_index);

                return true;
            }
        }

        for(tmp = 0; tmp < result; tmp++) {
            if(OnlineMap_[tmp] == 0) {
                OnlineMap_[tmp] = 1;
                remaining_amount_--;
                char* phy_addr = mmap_start_ + chunk_offset_ + tmp * CHUNK_SIZE; 
                *mchunk = new META_Chunk(number, tmp, phy_addr);
                update_chunk_index(number, tmp);
                return true;
            }
        }
    }

    META_Chunk* META::alloc_chunk(uint64_t number) {
        uint64_t chunk_index = get_chunk_index(number);
        assert(OnlineMap_[chunk_index] == 1);
        
        char* phy_addr = mmap_start_ + chunk_offset_ + chunk_index * CHUNK_SIZE; 
        META_Chunk* mchunk = new META_Chunk(number, chunk_index, phy_addr);
    }

    void META::evict_chunk(uint64_t number) {
        uint64_t chunk_index = get_chunk_index(number);
        OnlineMap_[chunk_index] = 0;
    }
}
