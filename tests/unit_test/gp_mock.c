/*-------------------------------------------------------------------------
 *
 * gp_mock.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   tests/unit_test/gp_mock.c
 *
 *-------------------------------------------------------------------------
 */

#include "gp_mock.h"

/*----------------------memory management-------------------------*/
void *
__wrap_palloc(Size size)
{
	void *param = calloc(1, size);
	return param;
}

void
__wrap_pfree(void *p)
{
	free(p);
}

/*--------------------------------HTAB----------------------------*/
HTAB *
__wrap_hash_create(const char *tabname, long nelem, HASHCTL *info, int flags)
{
	assert_true(nelem <= HTAB_BUCKET_ARRAY_SIZE);
	assert_non_null(tabname);
	assert_non_null(info);
	assert_true(info->keysize <= info->entrysize);

	int nlen = strlen(tabname);
	assert_true(nlen <= HTAB_NAME_LENGTH);

	HTAB *hashp = palloc(sizeof(HTAB));
	memcpy(hashp->tabname, tabname, nlen);
	hashp->keysize   = info->keysize;
	hashp->entrysize = info->entrysize;

	if (flags & HASH_FUNCTION) hashp->hash = info->hash;

	if (flags & HASH_COMPARE)
		hashp->match = info->match;
	else
		hashp->match = memcmp;

	if (flags & HASH_KEYCOPY)
		hashp->keycopy = info->keycopy;
	else
		hashp->keycopy = memcpy;

	return hashp;
}

void *
__wrap_hash_search(HTAB *hashp, const void *keyPtr, HASHACTION action, bool *foundPtr)
{
	assert_non_null(hashp);
	assert_non_null(keyPtr);

	if (foundPtr) *foundPtr = false;

	switch (action)
	{
		case HASH_FIND:
			for (int i = 0; i < HTAB_BUCKET_ARRAY_SIZE; i++)
			{
				if (hashp->match(hashp->bucket[i], keyPtr, hashp->keysize) == 0)
				{
					if (foundPtr) *foundPtr = true;
					return hashp->bucket[i];
				}
			}
			return NULL;
		case HASH_ENTER:
			for (int i = 0; i < HTAB_BUCKET_ARRAY_SIZE; i++)
			{
				if (hashp->bucket[i] == NULL) continue;
				if (hashp->match(hashp->bucket[i], keyPtr, hashp->keysize) == 0)
				{
					if (foundPtr) *foundPtr = true;
					return hashp->bucket[i];
				}
			}
			for (int i = 0; i < HTAB_BUCKET_ARRAY_SIZE; i++)
			{
				if (hashp->bucket[i] == NULL)
				{
					hashp->bucket[i] = palloc(hashp->entrysize);
					hashp->keycopy(hashp->bucket[i], keyPtr, hashp->keysize);
					return hashp->bucket[i];
				}
			}
			fail_msg("out of memory");
			return NULL;
		case HASH_REMOVE:
			for (int i = 0; i < HTAB_BUCKET_ARRAY_SIZE; i++)
			{
				if (hashp->bucket[i] == NULL) continue;
				if (hashp->match(hashp->bucket[i], keyPtr, hashp->keysize) == 0)
				{
					if (foundPtr) *foundPtr = true;
					pfree(hashp->bucket[i]);
					return NULL;
				}
			}
			return NULL;
		case HASH_ENTER_NULL:
			for (int i = 0; i < HTAB_BUCKET_ARRAY_SIZE; i++)
			{
				if (hashp->bucket[i] == NULL) continue;
				if (hashp->match(hashp->bucket[i], keyPtr, hashp->keysize) == 0)
				{
					if (foundPtr) *foundPtr = true;
					return hashp->bucket[i];
				}
			}
			for (int i = 0; i < HTAB_BUCKET_ARRAY_SIZE; i++)
			{
				if (hashp->bucket[i] == NULL)
				{
					hashp->bucket[i] = palloc(hashp->entrysize);
					hashp->keycopy(hashp->bucket[i], keyPtr, hashp->keysize);
					return hashp->bucket[i];
				}
			}
			return NULL;
		default:
			fail_msg("wrong hash action type");
			return NULL;
	}
}

void
__wrap_hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp)
{
	assert_non_null(status);
	assert_non_null(hashp);

	status->hashp     = hashp;
	status->curBucket = 0;
}

void *
__wrap_hash_seq_search(HASH_SEQ_STATUS *status)
{
	assert_non_null(status);
	assert_non_null(status->hashp);
	HTAB *hashp = status->hashp;
	void *ret   = NULL;

	while (status->curBucket < HTAB_BUCKET_ARRAY_SIZE)
	{
		if (hashp->bucket[status->curBucket] == NULL)
			status->curBucket++;
		else
		{
			ret = hashp->bucket[status->curBucket];
			status->curBucket++;
			break;
		}
	}
	return ret;
}
