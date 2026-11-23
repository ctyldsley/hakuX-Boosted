/* hakuX-Boosted jit bench: the host side of workloads.c. Prints the result of
 * each "name n seed" given on the command line (three arguments a case). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W(x) uint32_t wl_##x(uint32_t, uint32_t);
#include "workloads.def"
#undef W
int main(int argc, char **argv) {
  for (int i = 1; i + 2 < argc; i += 3) {
    uint32_t n = (uint32_t)strtoul(argv[i + 1], 0, 0), seed = (uint32_t)strtoul(argv[i + 2], 0, 0), r = 0;
    int found = 0;
#define W(x) if (!strcmp(argv[i], #x)) { r = wl_##x(n, seed); found = 1; }
#include "workloads.def"
#undef W
    if (!found) { fprintf(stderr, "no workload %s\n", argv[i]); return 1; }
    printf("%s %u %u %u\n", argv[i], n, seed, r);
  }
  return 0;
}
