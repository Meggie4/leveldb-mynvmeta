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

#define CHUNK_SIZE (60 * 1024)

namespace leveldb {

    class META_Chunk {
        public:
            //mmaped address
            char* addr_;
            uint64_t size_;
            uint64_t number_;
        
        public:
            META_Chunk(uint64_t number, char* addr);
            void append(const void* ptr, size_t size, META_Chunk* mchunk); 
            void flush();
    };

    class META {
        public:
            META(const std::string& nvm_file, size_t size, bool recovery = false);
            ~META();
            bool reserve_chunk(const std::string& chunk_name);
            META_Chunk* alloc_chunk(const std::string& chunk_name);
            bool reserve_and_alloc_chunk(const std::string& chunk_name, META_Chunk** mchunk);
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


