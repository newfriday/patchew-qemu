#!/bin/sh

in="$1"
out="$2"
bsd="$3"

awk -v bsd="$3" '{sub("SYS_", "TARGET_" bsd "_NR_", $0); print;}' < $in > $out
