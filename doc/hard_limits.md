# Design of Hard Limits for Diskquota

This document describes the design of the hard limit feature for Diskquota 2.0.

## Motivation

Due to the difficulty of observing the intermediate states of an in-progress query, Diskquota 1.0 only supports so-call "soft limits", meaning that Diskquota will not intertupt any running query even though the amount of data it writes exceeds some quota. 

Common types of queries that can write a large amount of data include
- `CREATE TABLE AS`
- `CREATE INDEX`
- `VACUUM FULL`

Running only one single query of such types can take up all the space of a disk, which can cause issues, such as a [Disk Full Failure](https://www.postgresql.org/docs/current/disk-full.html) that crashes the database system at worst.

Therefore, to mitigate the risk of having disk full issues, we plan to introduce "hard limits" in Diskquota 2.0, which enables Diskquota to terminate an in-progress query if the amount of data it writes exceeds some quota.

## Challenge 1: Observing Intermediate States

Diskquota cares about what relations, including tables, indexes, and more, that receives new data. Those relations are called "**active**" relations. Diskquota uses background workers (bgworkers) to collect active relations periodically and then calculates their sizes using an OS system call like `stat()`.

Active relations can be produced in two cases:
- Case 1: By writing new data to existing relations, e.g., using `INSERT` or `COPY FROM`. In this case, Diskquota do not need to observe any intermediate state of in-progress queries because the information of the active relations is committed and is visible to the background worker.
- Case 2: By creating new relations with data, e.g., using `CREATE TABLE AS` or `CREATE INDEX`. This is the hard part. In this case, the information of the active relations are **intermediate states** and has NOT been committed yet. Therefore, the information is not visible to the bgworkers when it scans the catalog tables under MVCC.

For Case 2, to enable the bgworkers to observe the active relations created by an in-progress query, there are two options:
- Option 1: Disregarding MVCC and scanning the catalog tables using `SNAPSHOT_DIRTY`. In this way, the bgworkers can see uncommitted information of the active relations by simpling doing a table scan.
- Option 2: Writing the information of newly created active relations to a shared memory area using **hooks** when executing a query. The bgworkers can retrive the information later from the shared memory area.

## Challenge 2: Ensuring Data Consistency

Since bgworkers are allowed to observe uncommitted states, extra work is required to ensure the bgworkers will never see inconsistent snapshots for both options.
- For Option 1, it is required to determine which tuple should take effect given that there may be multiple versions, including the versions created by aborted transactions.
- For Option 2, it is required to sync the information in the shared memory area against the latest committed version of the catalog tables.

Option 1 is more complicated and more error-prone than Option 2 since it requires Diskquota to do **visibility check** itself. Therefore, we choose Option 2 to implement hard limits.

For Option 2, there may be three types of tuples written by a transaction based on the transaction state:
- Type 1: tuples written by an **in-progress** transaction.
- Type 2: tuples written by an **committed** transaction.
- Type 3: tuples written by an **aborted** transaction.

## Challenge 3: Optimizing the Critical Write Path