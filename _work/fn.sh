#!/bin/bash
# usage: fn.sh ADDR [lines]  -> prints the generated C of F_ADDR
a=$(echo $1 | tr a-z A-Z); n=${2:-120}
cd "$(dirname "$0")/../games/Recomp_build"
r=$(grep -n "^void F_$a(CPU\* RESTRICT c, u64 e){" code_*.c | head -1)
f=${r%%:*}; l=${r#*:}; l=${l%%:*}
echo "== $f:$l"; sed -n "${l},$((l+n))p" $f
