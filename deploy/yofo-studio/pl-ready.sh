#!/bin/sh
# Wait until the PL is configured (DEVCFG INT_STS 0xF800700C bit 2, PCFG_DONE). Read-only: one
# PS register, never a PL register. Exit 0 when configured; with --once exit 1 instead of waiting.
INT_STS=0xF800700C
once=0
[ "$1" = "--once" ] && once=1
while :; do
    v=$(devmem2 "$INT_STS" w 2>/dev/null | awk '/Read at address/ {print $NF}')
    if [ -n "$v" ] && [ $(( v & 4 )) -ne 0 ]; then
        exit 0
    fi
    [ "$once" = 1 ] && exit 1
    sleep 2
done
