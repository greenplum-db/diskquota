# Stress Testing for Diskquota

This directory contains scripts for testing the behavior of Diskquota under heavy workloads. Specifically, we are interested in

- The performance of Diskquota itself for monitoring active tables and updating quota usage, and
- The performance of reading and writing data with Diskquota enabled.

Each script in this directory identifies one potential performance bottleneck and contains code to evaluate its impact.

To run the tests, do in the root directory:
```bash
$ PYTHONPATH='' python3 -m tests.stress <test_case> --<arg1_name>=<arg1_val> --<arg2_name>=<arg2_val> ...
```