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

// This is required on Mac OS X for getting PRI* macros #defined.
#define __STDC_FORMAT_MACROS

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define HERE() printf("I'm in %s @ %d, errno: %d\n", __func__, __LINE__, errno)

const size_t mem_size = 1 << 10;
const int toggles = 540000;

char *g_mem;

char *pick_addr() {
  size_t offset = (rand() << 12) % mem_size;
  return g_mem + offset;
}

class Timer {
  struct timeval start_time_;

 public:
  Timer() {
    // Note that we use gettimeofday() (with microsecond resolution)
    // rather than clock_gettime() (with nanosecond resolution) so
    // that this works on Mac OS X, because OS X doesn't provide
    // clock_gettime() and we don't really need nanosecond resolution.
    int rc = gettimeofday(&start_time_, NULL);
    assert(rc == 0);
  }

  double get_diff() {
    struct timeval end_time;
    int rc = gettimeofday(&end_time, NULL);
    assert(rc == 0);
    return (end_time.tv_sec - start_time_.tv_sec
            + (double) (end_time.tv_usec - start_time_.tv_usec) / 1e6);
  }
};

static void toggle(int iterations, int addr_count) {
  Timer timer;
  // HERE();
  for (int j = 0; j < iterations; j++) {
    // HERE();
    uint32_t *addrs[addr_count];
    for (int a = 0; a < addr_count; a++)
      addrs[a] = (uint32_t *) pick_addr();
    // HERE();

    uint32_t sum = 0;
    for (int i = 0; i < toggles; i++) {
      for (int a = 0; a < addr_count; a++)
        sum += *addrs[a] + 1;
      for (int j = 0x10000; j < 0x10000 + 4096; j++) {
        ((volatile unsigned int *) addrs + j);
      }
      asm volatile(
        ".word(0x100F)\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
      );
      asm volatile(".word(0x500F)\n");
    }
    // HERE();

    // Sanity check.  We don't expect this to fail, because reading
    // these rows refreshes them.
    if (sum != 0) {
      printf("error: sum=%x\n", sum);
      exit(1);
    }
    // HERE();
  }

  // Print statistics derived from the time and number of accesses.
  // double time_taken = timer.get_diff();
  // printf("  Took %.1f ms per address set\n",
  //        time_taken / iterations * 1e3);
  // printf("  Took %g sec in total for %i address sets\n",
  //        time_taken, iterations);
  // int memory_accesses = iterations * addr_count * toggles;
  // printf("  Took %.3f nanosec per memory access (for %i memory accesses)\n",
  //        time_taken / memory_accesses * 1e9,
  //        memory_accesses);
  // int refresh_period_ms = 64;
  // printf("  This gives %i accesses per address per %i ms refresh period\n",
  //        (int) (refresh_period_ms * 1e-3 * iterations * toggles / time_taken),
  //        refresh_period_ms);
}

void main_prog() {
  g_mem = (char *) mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
  assert(g_mem != MAP_FAILED);

  printf("clear\n");
  memset(g_mem, 0xff, mem_size);
  printf("memory set\n");

  Timer t;
  int iter = 0;
  for (;;) {
    printf("Iteration %i\n", iter++);
    // printf("Iteration %i (after %.2fs)\n", iter++, t.get_diff());
    // HERE();
    toggle(10, 8);
    // HERE();

    Timer check_timer;
    uint32_t *end = (uint32_t *) (g_mem + mem_size);
    uint32_t *ptr;
    int errors = 0;
    // HERE();
    for (ptr = (uint32_t *) g_mem; ptr < end; ptr++) {
      uint32_t got = *ptr;
      // HERE();
      if (got != ~(uint32_t) 0) {
        // HERE();
        printf("error at %p: got 0x%x" PRIx64 "\n", ptr, got);
        errors++;
      }
    }
    printf("  Checked for bit flips\n");
    // printf("  Checking for bit flips took %f sec\n", check_timer.get_diff());
    if (errors)
      exit(1);
  }
}


int main() {
  // In case we are running as PID 1, we fork() a subprocess to run
  // the test in.  Otherwise, if process 1 exits or crashes, this will
  // cause a kernel panic (which can cause a reboot or just obscure
  // log output and prevent console scrollback from working).
  int pid = fork();
  if (pid == 0) {
    main_prog();
    _exit(1);
  }

  int status;
  if (waitpid(pid, &status, 0) == pid) {
    printf("** exited with status %i (0x%x)\n", status, status);
  }

  for (;;) {
    sleep(999);
  }
  return 0;
}
