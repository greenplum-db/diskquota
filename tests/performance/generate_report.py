#!/bin/env python3

import sys
import re
import os

report = sys.argv[1]

lines = []


def handle_one_file(prefix, file):
    with open(prefix + '/' + file) as f:
        lines = f.readlines()

    print(f"{file},", end="")

    for line in lines:
        if len(line) == 0:
            continue

        if "number of transactions actually processed" in line:
            mch = re.findall(r"([0-9.]+)", line)
            val = int(mch[0])
            print(f"{val},", end="")
        elif "latency average" in line:
            mch = re.findall(r"([0-9.]+)", line)
            val = float(mch[0])
            print(f"{val},", end="")
        elif "tps" in line:
            mch = re.findall(r"([0-9.]+)", line)
            val = "%.4f" % float(mch[0])
            print(f"{val},", end="")
        elif "|" in line:
            if "hard_limit_latency_ms" in line or "insert_count" in line:
                continue
            strs = line.split("|")
            for str in strs:
                s = str.strip()
                if type(eval(s)) == float:
                    s = "%.4f" % (float(s))
                print(f"{s},", end="")
    print("")


def print_header():
    print(
        "name,"
        "number of transactions,"
        "latency,tps(including connections establishing),"
        "tps(excluding connections establishing),"
        "hard_limit_latency_ms,"
        "hard_limit_count,"
        "soft_limit_latency_ms,"
        "soft_limit_count,"
        "insert_count,"
        "delete_count"
    )


print_header()
prefix = sys.argv[1]
for file in os.listdir(prefix):
    handle_one_file(prefix, file)
