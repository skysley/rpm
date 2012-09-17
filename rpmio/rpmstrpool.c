#include "system.h"
#include <stdio.h>
#include <rpm/rpmstring.h>
#include <rpm/rpmstrpool.h>
#include "debug.h"

#define STRDATA_CHUNK 65536
#define STROFFS_CHUNK 2048
/* XXX this is ridiculously small... */
#define STRHASH_INITSIZE 5

static int pool_debug = 0;

typedef struct poolHash_s * poolHash;
typedef struct poolHashBucket_s * poolHashBucket;

struct poolHashBucket_s {
    poolHashBucket next;
    const char * key;
    rpmsid data;
};

struct poolHash_s {
    int numBuckets;
    poolHashBucket * buckets;
    int bucketCount;
    int keyCount;
};

static poolHash poolHashCreate(int numBuckets)
{
    poolHash ht;

    ht = xmalloc(sizeof(*ht));
    ht->numBuckets = numBuckets;
    ht->buckets = xcalloc(numBuckets, sizeof(*ht->buckets));
    ht->bucketCount = ht->keyCount = 0;
    return ht;
}

static void poolHashResize(poolHash ht, int numBuckets)
{
    poolHashBucket * buckets = xcalloc(numBuckets, sizeof(*ht->buckets));

    for (int i=0; i<ht->numBuckets; i++) {
	poolHashBucket b = ht->buckets[i];
	poolHashBucket nextB;
	while (b != NULL) {
	    unsigned int hash = rstrhash(b->key) % numBuckets;
	    nextB = b->next;
	    b->next = buckets[hash];
	    buckets[hash] = b;
	    b = nextB;
	}
    }
    free(ht->buckets);
    ht->buckets = buckets;
    ht->numBuckets = numBuckets;
}

static void poolHashAddHEntry(poolHash ht, const char * key, unsigned int keyHash, rpmsid data)
{
    unsigned int hash = keyHash % ht->numBuckets;
    poolHashBucket b = ht->buckets[hash];

    if (b == NULL) {
	ht->bucketCount += 1;
    }

    while (b && strcmp(b->key, key)) {
	b = b->next;
    }

    if (b == NULL) {
	ht->keyCount += 1;
	b = xmalloc(sizeof(*b));
	b->key = key;
	b->data = data;
	b->next = ht->buckets[hash];
	ht->buckets[hash] = b;

	if (ht->keyCount > ht->numBuckets) {
	    poolHashResize(ht, ht->numBuckets * 2);
	}
    }
}

static void poolHashAddEntry(poolHash ht, const char * key, rpmsid data)
{
    poolHashAddHEntry(ht, key, rstrhash(key), data);
}

static void poolHashEmpty( poolHash ht)
{
    poolHashBucket b, n;
    int i;

    if (ht->bucketCount == 0) return;

    for (i = 0; i < ht->numBuckets; i++) {
	b = ht->buckets[i];
	if (b == NULL)
	    continue;
	ht->buckets[i] = NULL;

	do {
	    n = b->next;
	    b = _free(b);
	} while ((b = n) != NULL);
    }
    ht->bucketCount = 0;
    ht->keyCount = 0;
}

static poolHash poolHashFree(poolHash ht)
{
    if (ht==NULL)
        return ht;
    poolHashEmpty(ht);
    ht->buckets = _free(ht->buckets);
    ht = _free(ht);

    return NULL;
}

static rpmsid poolHashGetHEntry(poolHash ht, const char * key, unsigned int keyHash)
{
    unsigned int hash = keyHash % ht->numBuckets;
    poolHashBucket b = ht->buckets[hash];

    while (b && strcmp(b->key, key))
	b = b->next;

    return (b == NULL) ? 0 : b->data;
}

static void poolHashPrintStats(poolHash ht)
{
    int i;
    poolHashBucket bucket;

    int hashcnt=0, bucketcnt=0;
    int maxbuckets=0;

    for (i=0; i<ht->numBuckets; i++) {
        int buckets = 0;
        for (bucket=ht->buckets[i]; bucket; bucket=bucket->next){
	    buckets++;
	}
	if (maxbuckets < buckets) maxbuckets = buckets;
	if (buckets) hashcnt++;
	bucketcnt += buckets;
    }
    fprintf(stderr, "Hashsize: %i\n", ht->numBuckets);
    fprintf(stderr, "Hashbuckets: %i\n", hashcnt);
    fprintf(stderr, "Keys: %i\n", bucketcnt);
    fprintf(stderr, "Max Keys/Bucket: %i\n", maxbuckets);
}

struct rpmstrPool_s {
    size_t * offs;		/* offsets into data area */
    rpmsid offs_size;		/* largest offset index */;
    rpmsid offs_alloced;	/* offsets allocation size */
    char * data;		/* string data area */
    size_t data_size;		/* string data area size */
    size_t data_alloced;	/* string data area allocation size */
    poolHash hash;		/* string -> sid hash table */
    int frozen;			/* are new id additions allowed? */
    int nrefs;			/* refcount */
};

static void rpmstrPoolRehash(rpmstrPool pool)
{
    int sizehint;

    if (pool->offs_size < STRHASH_INITSIZE)
	sizehint = STRHASH_INITSIZE;
    else
	sizehint = pool->offs_size * 2;

    if (pool->hash)
	pool->hash = poolHashFree(pool->hash);

    pool->hash = poolHashCreate(sizehint);
    for (int i = 1; i < pool->offs_size; i++)
	poolHashAddEntry(pool->hash, rpmstrPoolStr(pool, i), i);
}

rpmstrPool rpmstrPoolCreate(void)
{
    rpmstrPool pool = xcalloc(1, sizeof(*pool));
    rpmstrPoolRehash(pool);
    pool->nrefs = 1;
    return pool;
}

rpmstrPool rpmstrPoolFree(rpmstrPool pool)
{
    if (pool) {
	if (pool->nrefs > 1) {
	    pool->nrefs--;
	} else {
	    if (pool_debug)
		poolHashPrintStats(pool->hash);
	    poolHashFree(pool->hash);
	    free(pool->offs);
	    free(pool->data);
	    free(pool);
	}
    }
    return NULL;
}

rpmstrPool rpmstrPoolLink(rpmstrPool pool)
{
    if (pool)
	pool->nrefs++;
    return pool;
}

void rpmstrPoolFreeze(rpmstrPool pool, int keephash)
{
    if (pool && !pool->frozen) {
	/*
	 * realloc() might require rehashing even when downsizing,
	 * dont bother unless we're also discarding the hash.
	 */
	if (!keephash) {
	    pool->hash = poolHashFree(pool->hash);
	    pool->data_alloced = pool->data_size;
	    pool->data = xrealloc(pool->data, pool->data_alloced);
	    pool->offs_alloced = pool->offs_size + 1;
	    pool->offs = xrealloc(pool->offs,
				  pool->offs_alloced * sizeof(*pool->offs));
	}
	pool->frozen = 1;
    }
}

void rpmstrPoolUnfreeze(rpmstrPool pool)
{
    if (pool) {
	if (pool->hash == NULL) {
	    rpmstrPoolRehash(pool);
	}
	pool->frozen = 0;
    }
}

static rpmsid rpmstrPoolPut(rpmstrPool pool, const char *s, size_t slen, unsigned int hash)
{
    const char *t = NULL;
    size_t ssize = slen + 1;

    if (ssize > pool->data_alloced - pool->data_size) {
	const char * prev_data = pool->data;
	size_t need = pool->data_size + ssize;
	size_t alloced = pool->data_alloced;

	while (alloced < need)
	    alloced += STRDATA_CHUNK;

	pool->data = xrealloc(pool->data, alloced);
	pool->data_alloced = alloced;

	/* ouch, need to rehash the whole lot if key addresses change */
	if (pool->offs_size > 0 && pool->data != prev_data) {
	    rpmstrPoolRehash(pool);
	}
    }

    pool->offs_size += 1;
    if (pool->offs_alloced <= pool->offs_size) {
	pool->offs_alloced += STROFFS_CHUNK;
	pool->offs = xrealloc(pool->offs,
			      pool->offs_alloced * sizeof(*pool->offs));
    }

    t = memcpy(pool->data + pool->data_size, s, ssize);
    pool->offs[pool->offs_size] = pool->data_size;
    pool->data_size += ssize;

    poolHashAddHEntry(pool->hash, t, hash, pool->offs_size);

    return pool->offs_size;
}

rpmsid rpmstrPoolIdn(rpmstrPool pool, const char *s, size_t slen, int create)
{
    rpmsid sid = 0;

    if (pool && pool->hash && s) {
	unsigned int hash = rstrhash(s);
	sid = poolHashGetHEntry(pool->hash, s, hash);
	if (sid == 0 && create && !pool->frozen) {
	    sid = rpmstrPoolPut(pool, s, slen, hash);
	}
    }

    return sid;
}

rpmsid rpmstrPoolId(rpmstrPool pool, const char *s, int create)
{
    return rpmstrPoolIdn(pool, s, strlen(s), create);
}

const char * rpmstrPoolStr(rpmstrPool pool, rpmsid sid)
{
    const char *s = NULL;
    if (pool && sid > 0 && sid <= pool->offs_size)
	s = pool->data + pool->offs[sid];
    return s;
}

size_t rpmstrPoolStrlen(rpmstrPool pool, rpmsid sid)
{
    size_t slen = 0;
    if (pool && sid <= pool->offs_size) {
	size_t end = (sid < pool->offs_size) ? pool->offs[sid + 1] :
					       pool->offs_size;
	slen = end - pool->offs[sid] - 1;
    }
    return slen;
}

int rpmstrPoolStreq(rpmstrPool poolA, rpmsid sidA,
		    rpmstrPool poolB, rpmsid sidB)
{
    if (poolA == poolB)
	return (sidA == sidB);
    else
	return rstreq(rpmstrPoolStr(poolA, sidA), rpmstrPoolStr(poolB, sidB));
}