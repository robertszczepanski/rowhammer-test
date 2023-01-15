// Copyright 2015, Google, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Small test program to systematically check through the memory to find bit
// flips by double-sided row hammering.
//
// Compilation instructions:
//   g++ -std=c++11 [filename]
//
// ./double_sided_rowhammer [-t nsecs] [-p percentage]
//
// Hammers for nsecs seconds, acquires the described fraction of memory (0.0
// to 0.9 or so).
//
// Original author: Thomas Dullien (thomasdullien@google.com)

#include <asm/unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/kernel-page-flags.h>
#include <map>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#define HERE() printf("I'm in %s @ %d, errno: %d\n", __func__, __LINE__, errno)

namespace {

// The fraction of physical memory that should be mapped for testing.
uint32_t fraction_of_physical_memory = 10;

// The time to hammer before aborting. Defaults to one hour.
uint32_t number_of_seconds_to_hammer = 3600;

// The number of memory reads to try.
uint32_t number_of_reads = 1000*1024;

// Obtain the size of the physical memory of the system.
uint32_t GetPhysicalMemorySize() {
  struct sysinfo info;
  HERE();
  sysinfo( &info );
  HERE();
  printf("[!] Total RAM: %d, Unit: %d\n", (size_t)info.totalram, (size_t)info.mem_unit);
  return (size_t)info.totalram * (size_t)info.mem_unit;
}

uint32_t GetPageFrameNumber(int pagemap, uint8_t* virtual_address) {
  // Read the entry in the pagemap.
  uint32_t value;
  printf("pagemap: %d, value: %d, virtual_address: %x, arg: %x\n", pagemap, value, virtual_address, (reinterpret_cast<uintptr_t>(virtual_address) / 0x1000));
  int got = pread(pagemap, &value, 4,
                  (reinterpret_cast<uintptr_t>(virtual_address) / 0x1000) * 4);
  printf("\nGot: %d, errno: %d\n", got, errno);
  assert(got == 4);
  uint32_t page_frame_number = value & ((1ULL << 54)-1);
  return page_frame_number;
}

void SetupMapping(uint32_t* mapping_size, void** mapping) {
  HERE();
  *mapping_size = GetPhysicalMemorySize() / fraction_of_physical_memory;

  HERE();
  printf("errno: %d\n", errno);
  *mapping = mmap(NULL, *mapping_size, PROT_READ | PROT_WRITE,
      MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  HERE();
  assert(*mapping != (void*)-1);
  printf("mapping: %x, mapping_size: %d, errno: %d\n", *mapping, *mapping_size, errno);

  // Initialize the mapping so that the pages are non-empty.
  printf("[!] Initializing large memory mapping ...");
  for (uint32_t index = 0; index < *mapping_size; index += 0x1000) {
    uint32_t* temporary = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(*mapping) + index);
    temporary[0] = index;
  }
  printf("done\n");
}

uint32_t HammerAddressesStandard(
    const std::pair<uint32_t, uint32_t>& first_range,
    const std::pair<uint32_t, uint32_t>& second_range,
    uint32_t number_of_reads) {
  volatile uint32_t* first_pointer =
      reinterpret_cast<uint32_t*>(first_range.first);
  volatile uint32_t* second_pointer =
      reinterpret_cast<uint32_t*>(second_range.first);
  uint32_t sum = 0;

  volatile uint32_t* cache_pointer;
  while (number_of_reads-- > 0) {
    sum += first_pointer[0];
    sum += second_pointer[0];

    if (first_pointer > reinterpret_cast<uint32_t*>(0x20000)) {
      cache_pointer = reinterpret_cast<uint32_t*>(0x10000);
    } else {
      cache_pointer = reinterpret_cast<uint32_t*>(0x30000);
    }
    printf("Clear cache...\n");
    for (int i = 0; i < 4096; i++) {
      asm volatile(
        "lw t0, (%0)\n\t" : : "r" (cache_pointer++) : "memory");
    }
    // printf("Attack memory\n");
    // asm volatile(
    //     "lw t0, (%0)\n\t;"
    //     "lw t0, (%1)\n\t;"
    //     : : "r" (first_pointer), "r" (second_pointer) : "memory");
  }
  return sum;
}

typedef uint32_t(HammerFunction)(
    const std::pair<uint32_t, uint32_t>& first_range,
    const std::pair<uint32_t, uint32_t>& second_range,
    uint32_t number_of_reads);

// A comprehensive test that attempts to hammer adjacent rows for a given 
// assumed row size (and assumptions of sequential physical addresses for 
// various rows.
uint32_t HammerAllReachablePages(uint32_t presumed_row_size, 
    void* memory_mapping, uint32_t memory_mapping_size, HammerFunction* hammer,
    uint32_t number_of_reads) {
  // This vector will be filled with all the pages we can get access to for a
  // given row size.
  std::vector<std::vector<uint8_t*>> pages_per_row;
  uint32_t total_bitflips = 0;

  pages_per_row.resize(memory_mapping_size / presumed_row_size);
  int pagemap = open("/proc/self/pagemap", O_RDONLY);
  assert(pagemap >= 0);

  printf("[!] Identifying rows for accessible pages ...\n ");
  printf("memory_mapping: %x\n", memory_mapping);
  for (uint32_t offset = 0; offset < memory_mapping_size; offset += 0x1000) {
    uint8_t* virtual_address = static_cast<uint8_t*>(memory_mapping) + offset;
    uint32_t page_frame_number = GetPageFrameNumber(pagemap, virtual_address);
    uint32_t physical_address = page_frame_number * 0x1000;
    uint32_t presumed_row_index = physical_address / presumed_row_size;
    printf("[!] put va %lx pa %lx into row %ld\n", (uint32_t)virtual_address,
       physical_address, presumed_row_index);
    if (presumed_row_index > pages_per_row.size()) {
      pages_per_row.resize(presumed_row_index);
    }
    pages_per_row[presumed_row_index].push_back(virtual_address);
    printf("[!] done\n");
  }
  printf("Done\n");

  // We should have some pages for most rows now.
  for (uint32_t row_index = 0; row_index + 2 < pages_per_row.size();
      ++row_index) {
    if ((pages_per_row[row_index].size() != 64) || 
        (pages_per_row[row_index+2].size() != 64)) {
      printf("[!] Can't hammer row %ld - only got %ld/%ld pages "
          "in the rows above/below\n",
          row_index+1, pages_per_row[row_index].size(), 
          pages_per_row[row_index+2].size());
      continue;
    } else if (pages_per_row[row_index+1].size() == 0) {
      printf("[!] Can't hammer row %ld, got no pages from that row\n", 
          row_index+1);
      continue;
    }
    printf("[!] Hammering rows %ld/%ld/%ld of %ld (got %ld/%ld/%ld pages)\n", 
        row_index, row_index+1, row_index+2, pages_per_row.size(), 
        pages_per_row[row_index].size(), pages_per_row[row_index+1].size(), 
        pages_per_row[row_index+2].size());
    // Iterate over all pages we have for the first row.
    for (uint8_t* first_row_page : pages_per_row[row_index]) {
      // Iterate over all pages we have for the second row.
      for (uint8_t* second_row_page : pages_per_row[row_index+2]) {
        // Set all the target pages to 0xFF.
        for (uint8_t* target_page : pages_per_row[row_index+1]) {
          memset(target_page, 0xFF, 0x1000);
        }
        // Now hammer the two pages we care about.
        std::pair<uint32_t, uint32_t> first_page_range(
            reinterpret_cast<uint32_t>(first_row_page),
            reinterpret_cast<uint32_t>(first_row_page+0x1000));
        std::pair<uint32_t, uint32_t> second_page_range(
            reinterpret_cast<uint32_t>(second_row_page),
            reinterpret_cast<uint32_t>(second_row_page+0x1000));
        hammer(first_page_range, second_page_range, number_of_reads);
        // Now check the target pages.
        uint32_t number_of_bitflips_in_target = 0;
        for (const uint8_t* target_page : pages_per_row[row_index+1]) {
          for (uint32_t index = 0; index < 0x1000; ++index) {
            if (target_page[index] != 0xFF) {
              ++number_of_bitflips_in_target;
            }
          }
        }
        if (number_of_bitflips_in_target > 0) {
          printf("[!] Found %ld flips in row %ld (%lx to %lx) when hammering "
              "%lx and %lx\n", number_of_bitflips_in_target, row_index+1,
              ((row_index+1)*presumed_row_size), 
              ((row_index+2)*presumed_row_size)-1,
              GetPageFrameNumber(pagemap, first_row_page)*0x1000, 
              GetPageFrameNumber(pagemap, second_row_page)*0x1000);
          total_bitflips += number_of_bitflips_in_target;
        }
      }
    }
  }
  return total_bitflips;
}

void HammerAllReachableRows(HammerFunction* hammer, uint32_t number_of_reads) {
  uint32_t mapping_size;
  void* mapping;
  HERE();
  SetupMapping(&mapping_size, &mapping);

  HERE();
  HammerAllReachablePages(1024*256, mapping, mapping_size,
                          hammer, number_of_reads);
}

void HammeredEnough(int sig) {
  printf("[!] Spent %ld seconds hammering, exiting now.\n",
      number_of_seconds_to_hammer);
  fflush(stdout);
  fflush(stderr);
  exit(0);
}

}  // namespace

int main(int argc, char** argv) {
  // Turn off stdout buffering when it is a pipe.
  HERE();
  setvbuf(stdout, NULL, _IONBF, 0);
  HERE();

  int opt;
  while ((opt = getopt(argc, argv, "t:p:")) != -1) {
    switch (opt) {
      case 't':
        number_of_seconds_to_hammer = atoi(optarg);
        break;
      case 'p':
        fraction_of_physical_memory = atof(optarg);
        break;
      default:
        fprintf(stderr, "Usage: %s [-t nsecs] [-p percent]\n", 
            argv[0]);
        exit(EXIT_FAILURE);
    }
  }

  HERE();
  signal(SIGALRM, HammeredEnough);

  printf("[!] Starting the testing process...\n");
  HERE();
  alarm(number_of_seconds_to_hammer);
  HERE();
  HammerAllReachableRows(&HammerAddressesStandard, number_of_reads);
}
