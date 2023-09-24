#!/bin/env python3

import sys
import re

report_1 = sys.argv[1]
report_2 = sys.argv[2]

lines_1 = []
lines_2 = []

with open(report_1) as f:
    lines_1 = f.readlines()
with open(report_2) as f:
    lines_2 = f.readlines()

if len(lines_1) != len(lines_2):
    print(f"Line counts don't match for '{report_1}' and '{report_2}'",
          file=sys.stderr)
    sys.exit(1)

for i in range(len(lines_1)):
    l1 = lines_1[i]
    l2 = lines_2[i]

    if len(l1) == 0:
        continue

    if l1.find(":") >= 0:
        s1 = l1.split(":")
        s2 = l2.split(":")
    elif l1.find("=") >= 0:
        s1 = l1.split("=")
        s2 = l2.split("=")
    else:
        print(f"{l1}")
        continue
    for j in range(len(s1)):
        s1[j] = s1[j].strip()
        s2[j] = s2[j].strip()

    diff = ""
    if "tps" in s1[0] or "latency" in s1[0] or "transactions" in s1[0]:
        m1 = re.findall(r'([0-9.]+)', s1[1])
        m2 = re.findall(r'([0-9.]+)', s2[1])
        f1 = float(m1[0])
        f2 = float(m2[0])
        diff = (f2 - f1) / f1 * 100
        print(f"{s1[0]},{s1[1]},{s2[1]},%{diff}")
