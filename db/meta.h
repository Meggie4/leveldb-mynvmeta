#include <stdint.h>
#include <string>

#include <sys/types.h>
#include <sys/mman.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>    //provides O_RDONLY, 
#include <linux/fs.h>   //provides BLKGETSIZE
#include <sys/ioctl.h>  //provides ioctl()

#include <stdbool.h>

#include <vector>
#include <list>
#include <set>

extern "C" {
    #include "bplustree/lib/bplustree.h"
}

#define CHUNK_SIZE (94 * 1024)

namespace leveldb {

    class META_Chunk {
        public:
            //mmaped address
            char* addr_;
            uint64_t size_;
            uint64_t number_;
            uint64_t index_;
            char* current_ptr_;
            char* length_ptr_;
        
        public:
            META_Chunk(uint64_t number, uint64_t index, char* addr);
            uint64_t append(const void* ptr, uint64_t size); 
            void set_length(uint64_t len);
            void flush();

            uint64_t get_number() { 
                uint64_t res = *((uint64_t*) current_ptr_); 
                current_ptr_ += 8;
                return res;
            }
            
            char* get_blockcontents(uint64_t len) {
                char* res = current_ptr_;
                current_ptr_ += len;
                return res;
            }

            uint64_t get_index() {return index_;}
    };

    class META {
        public:
            META(const std::string& nvm_file, uint64_t size, bool recovery = false);
            ~META();
            bool reserve_chunk(uint64_t number);
            META_Chunk* alloc_chunk(uint64_t number);
            bool reserve_and_alloc_chunk(uint64_t number, META_Chunk** mchunk);
            inline uint64_t get_chunk_index(uint64_t number);
            inline void update_chunk_index(uint64_t number, uint64_t index);
        public:
            uint64_t chunk_amount_;
            uint64_t remaining_amount_;
            uint64_t size_;
            uint64_t chunk_offset_;
            int fd_;
            char* mmap_start_;
            char* OnlineMap_;
            bplus_tree* index_tree_;
    };
}


