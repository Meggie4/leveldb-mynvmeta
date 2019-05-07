#include <stdio.h>
#include <stdlib.h>

#include "db/meta.h"
#include "port/cache_flush.h"

struct bplus_tree_config{
    int order;
    int entries;
};

namespace leveldb{
    META_Chunk::META_Chunk(uint64_t number, char* addr)
    : number_(number),
      addr_(addr),
      size_(0) {
    }
   
    void META_Chunk::append(const void* ptr, size_t size, META_Chunk* mchunk) {
        if(mchunk->size_ + size > CHUNK_SIZE) {
            fprintf(stderr, "meta.cc append, overflow exit, name=%06llu.ldb, chunk->size_:%lld, size:%zu\n",
                    static_cast<unsigned long long>(number_), mchunk->size_, size);
            exit(9);
        }
        char* current_ptr = addr_ + size_;
        memcpy(current_ptr, ptr, size);

        size_ += size;
    }

    void META_Chunk::flush() {
       flush_cache(addr_, size_); 
    }

    META::META(const std::string& nvm_file, size_t file_size, bool recovery) 
        : chunk_amount_(0),
          remaining_amount_(0),
          size_(file_size),
          chunk_offset_(0),
          fd_(-1),
          mmap_start_(nullptr),
          OnlineMap_(nullptr),
          index_tree_(nullptr) {
        /////mmap open fd;
        fd_ = open(nvm_file.c_str(), O_RDWR);
        
        if(fd_ == -1) {
            fprintf(stderr, "create nvm_file:%s\n", nvm_file.c_str());
            fd_ = open(nvm_file.c_str(), O_RDWR | O_CREAT, 0664);
            if(fd_ < 0) {
                fprintf(stderr, "create nvm_file failed\n");
                exit(9);
            }
        }
        
        if(ftruncate(fd_, size_) != 0) {
            perror("ftruncate failed\n");
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

    bool META::reserve_chunk(const std::string& chunk_name) {
        //get number, 
        std::string short_file_name = chunk_name;
        if(short_file_name.find("/mnt/ssd/") != -1) {
            int found = chunk_name.find_last_of("/");
            short_file_name = chunk_name.substr(found + 1);
        }
        uint64_t number = atoi(short_file_name.c_str());
        
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

    bool META::reserve_and_alloc_chunk(const std::string& chunk_name, 
            META_Chunk** mchunk) {
        //get number, 
        std::string short_file_name = chunk_name;
        if(short_file_name.find("/mnt/ssd/") != -1) {
            int found = chunk_name.find_last_of("/");
            short_file_name = chunk_name.substr(found + 1);
        }
        uint64_t number = atoi(short_file_name.c_str());
        
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
                *mchunk = new META_Chunk(number, phy_addr);
                update_chunk_index(number, tmp);
                return true;
            }
        }

        for(tmp = 0; tmp < result; tmp++) {
            if(OnlineMap_[tmp] == 0) {
                OnlineMap_[tmp] = 1;
                remaining_amount_--;
                char* phy_addr = mmap_start_ + chunk_offset_ + tmp * CHUNK_SIZE; 
                *mchunk = new META_Chunk(number, phy_addr);
                update_chunk_index(number, tmp);
                return true;
            }
        }
    }

    META_Chunk* META::alloc_chunk(const std::string& chunk_name) {
        std::string short_file_name = chunk_name;
        if(short_file_name.find("/mnt/ssd/") != -1) {
            int found = chunk_name.find_last_of("/");
            short_file_name = chunk_name.substr(found + 1);
        }
        uint64_t number = atoi(short_file_name.c_str());
        
        uint64_t chunk_index = get_chunk_index(number);
        char* phy_addr = mmap_start_ + chunk_offset_ + chunk_index * CHUNK_SIZE; 
        
        META_Chunk* mchunk = new META_Chunk(number, phy_addr);
    }
}
