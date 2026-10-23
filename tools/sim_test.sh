#!/usr/bin/env bash
# sim_test.sh - cloud check of the generated KawPow kernel on the CPU: for each test
# vector, generates the period's program, compiles gpu/kawpow_kernel.cuh with the
# simulator shim and compares 16 consecutive nonces with the ethash reference.
# usage: tools/sim_test.sh <objdir with ethash .o files>
set -e
cd "$(dirname "$0")/.."
OBJ="${1:-/tmp/fmb}"
g++ -O2 -std=c++17 tools/gen_program.cpp -o "$OBJ/gen_program"
fails=0
run() {  # height header nonce mix final
    local h=$1 epoch=$(( $1 / 7500 ))
    local items
    items=$("$OBJ/ref_test" items $epoch)
    "$OBJ/gen_program" "$h" "$items" > "$OBJ/gen_$h.h"
    g++ -O2 -std=c++20 -Wno-unknown-pragmas -Ithird_party -I. -DGEN_HEADER="\"$OBJ/gen_$h.h\"" tests/sim_kernel.cpp "$OBJ"/kref.o "$OBJ"/ethash.o \
        "$OBJ"/progpow.o "$OBJ"/primes.o "$OBJ"/keccak.o "$OBJ"/kf1600.o "$OBJ"/kf800.o -pthread -o "$OBJ/sim_$h"
    "$OBJ/sim_$h" "$2" "$3" $4 $5 || fails=$((fails+1))
}
run 0 0000000000000000000000000000000000000000000000000000000000000000 0000000000000000 6e97b47b134fda0c7888802988e1a373affeb28bcd813b6e9a0fc669c935d03a e601a7257a70dc48fccc97a7330d704d776047623b92883d77111fb36870f3d1
run 49 63155f732f2bf556967f906155b510c917e48e99685ead76ea83f4eca03ab12b 0000000007073c07 d36f7e815ee09e74eceb9c96993a3d681edf2bf0921fc7bb710364042db99777 e7ced124598fd2500a55ad9f9f48e3569327fe50493c77a4ac9799b96efb9463
run 30000 ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff 123456789abcdef0 177b565752a375501e11b6d9d3679c2df6197b2cab3a1ba2d6b10b8c71a3d459 c824bee0418e3cfb7fae56e0d5b3b8b14ba895777feea81c70c0ba947146da69
run 149163 9e7248f20914913a73d80a70174c331b1d34f260535ac3631d770e656b5dd922 00000000076e482e
run 170915 9e7248f20914913a73d80a70174c331b1d34f260535ac3631d770e656b5dd922 a0b1c2d3e4f50617
echo "SIM RESULT: $([ $fails -eq 0 ] && echo ALL OK || echo "$fails FAILED")"
