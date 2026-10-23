// gen_program.cpp - prints the generated KawPow program header for a block height
// (used by tools/sim_test.sh, and handy to read a period's program).
// usage: gen_program height dag_items_2048
#include <cstdio>
#include <cstdlib>
#include "../gpu/progpow_gen.h"

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: gen_program height dag_items\n"); return 2; }
    const int height = atoi(argv[1]);
    const unsigned items = (unsigned)strtoul(argv[2], nullptr, 10);
    fmgen::Program p = fmgen::make_program((uint64_t)height / fmgen::PERIOD);
    printf("#define SIM_HEIGHT %d\n#define SIM_EPOCH %d\n#define FM_DAG_ITEMS %uu\n", height, height / 7500, items);
    printf("%s", fmgen::emit_macros(p).c_str());
    return 0;
}
