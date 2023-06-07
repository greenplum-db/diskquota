/*-------------------------------------------------------------------------
 *
 * gp_mock.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   tests/unit_test/gp_mock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_MOCK_H
#define GP_MOCK_H

#include "c.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cmocka.h>

/*-----------------------------------------------error report------------------------------------------------------*/
#include "utils/elog.h"

#ifdef Assert
#undef Assert
#define Assert assert_true
#endif

#ifdef ereport
#undef ereport
#endif
#ifdef sprintf
#undef sprintf
#endif
#ifdef printf
#undef printf
#endif

#define errcode(sqlerrcode) printf("%d", sqlerrcode)
#define errmsg(fmt, ...) printf(fmt, __VA_ARGS__)

#define ereport(elevel, ...)         \
	do                               \
	{                                \
		__VA_ARGS__;                 \
		if (elevel >= ERROR) fail(); \
	} while (0)

/*---------------------------------------------------palloc------------------------------------------------------*/
extern void *palloc(Size size);
extern void  pfree(void *p);

/*-----------------------------------------------hash table------------------------------------------------------*/
/* Flags to indicate which parameters are supplied */
#define HASH_PARTITION 0x001   /* Hashtable is used w/partitioned locking */
#define HASH_SEGMENT 0x002     /* Set segment size */
#define HASH_DIRSIZE 0x004     /* Set directory size (initial and max) */
#define HASH_FFACTOR 0x008     /* Set fill factor */
#define HASH_FUNCTION 0x010    /* Set user defined hash function */
#define HASH_ELEM 0x020        /* Set keysize and entrysize */
#define HASH_SHARED_MEM 0x040  /* Hashtable is in shared memory */
#define HASH_ATTACH 0x080      /* Do not initialize hctl */
#define HASH_ALLOC 0x100       /* Set memory allocator */
#define HASH_CONTEXT 0x200     /* Set memory allocation context */
#define HASH_COMPARE 0x400     /* Set user defined comparison function */
#define HASH_KEYCOPY 0x800     /* Set user defined key-copying function */
#define HASH_FIXED_SIZE 0x1000 /* Initial size is a hard limit */

#define HTAB_NAME_LENGTH 256
#define HTAB_BUCKET_ARRAY_SIZE 1024
#define HELEMENT void *

typedef uint32 (*HashValueFunc)(const void *key, Size keysize);
typedef int (*HashCompareFunc)(const void *key1, const void *key2, Size keysize);
typedef void *(*HashCopyFunc)(void *dest, const void *src, Size keysize);

typedef struct HTAB
{
	char            tabname[HTAB_NAME_LENGTH];
	HELEMENT        bucket[HTAB_BUCKET_ARRAY_SIZE];
	Size            keysize;   /* hash key length in bytes */
	Size            entrysize; /* total user element size in bytes */
	HashValueFunc   hash;      /* hash function */
	HashCompareFunc match;     /* key comparison function */
	HashCopyFunc    keycopy;   /* key copying function */
} HTAB;

/* Parameter data structure for hash_create */
/* Only those fields indicated by hash_flags need be set */
typedef struct HASHCTL
{
	Size            keysize;   /* hash key length in bytes */
	Size            entrysize; /* total user element size in bytes */
	HashValueFunc   hash;      /* hash function */
	HashCompareFunc match;     /* key comparison function */
	HashCopyFunc    keycopy;   /* key copying function */
} HASHCTL;

/* hash_search operations */
typedef enum
{
	HASH_FIND,
	HASH_ENTER, /* monitor shared memory htab */
	HASH_REMOVE,
	HASH_ENTER_NULL
} HASHACTION;

/* hash_seq status (should be considered an opaque type by callers) */
typedef struct
{
	HTAB  *hashp;
	uint32 curBucket; /* index of current bucket */
} HASH_SEQ_STATUS;

extern HTAB *hash_create(const char *tabname, long nelem, HASHCTL *info, int flags);
extern void *hash_search(HTAB *hashp, const void *keyPtr, HASHACTION action, bool *foundPtr);
extern void  hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp);
extern void *hash_seq_search(HASH_SEQ_STATUS *status);

#endif
