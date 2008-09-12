
/*
 * s3backer - FUSE-based single file backing store via Amazon S3
 * 
 * Copyright 2008 Archie L. Cobbs <archie@dellroad.org>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * $Id$
 */

#include "s3backer.h"
#include "ec_protect.h"

/*
 * Written block information caching.
 *
 * The purpose of this is to minimize problems from the weak guarantees provided
 * by S3's "eventual consistency". We do this by:
 *
 *  (a) Enforcing a minimum delay between the completion of one PUT/DELETE
 *      of a block and the initiation of the next PUT/DELETE of the same block
 *  (b) Caching the MD5 checksum of every block written for some minimum time
 *      and verifying that data returned from subsequent GETs is correct.
 *
 * These are the relevant configuration parameters:
 *
 *  min_write_delay
 *      Minimum time delay after a PUT/DELETE completes before the next PUT/DELETE
 *      can be initiated.
 *  cache_time
 *      How long after writing a block we'll remember its MD5 checksum. This
 *      must be at least as long as min_write_delay.
 *  cache_size
 *      Maximum number of blocks we'll track at one time. When table
 *      is full, additional writes will block.
 *
 * Blocks we are currently tracking can be in the following states:
 *
 * State    Meaning                  Hash table  List  Other invariants
 * -----    -------                  ----------  ----  ----------------
 *
 * CLEAN    initial state            No          No
 * WRITING  currently being written  Yes         No    timestamp == 0, u.data valid
 * WRITTEN  written and MD5 cached   Yes         Yes   timestamp != 0, u.md5 valid
 *
 * The steady state for a block is CLEAN. WRITING means the block is currently
 * being sent; concurrent attempts to write will simply sleep until the first one
 * finishes. WRITTEN is where you go after successfully writing a block. The WRITTEN
 * state will timeout (and the entry revert to CLEAN) after cache_time.
 *
 * If another attempt to write a block in the WRITTEN state occurs occurs before
 * min_write_delay has elapsed, the second attempt will sleep.
 *
 * A separate thread periodically scans the table and removes expired WRITTENs
 *
 * In the WRITING state, we have the data still so any reads are local. In the WRITTEN
 * state we don't have the data but we do know its MD5, so therefore we can verify what
 * comes back; if it doesn't verify, we retry as we would with any other error.
 *
 * If we hit the 'cache_size' limit, we sleep a little while and then try again.
 *
 * We keep track of blocks in 'struct block_info' structures. These structures
 * are themselves tracked in both (a) a linked list and (b) a hash table.
 *
 * The hash table contains all structures, and is keyed by block number. This
 * is simply so we can quickly find the structure associated with a specific block.
 *
 * The linked list contains WRITTEN blocks, and is sorted in increasing order by timestamp,
 * so the entries that will expire first are at the front of the list.
 */
struct block_info {
    s3b_block_t             block_num;          // block number
    uint64_t                timestamp;          // time PUT/DELETE completed (if WRITTEN)
    TAILQ_ENTRY(block_info) link;               // list entry link
    union {
        const void      *data;                  // blocks actual content (if WRITING)
        u_char          md5[MD5_DIGEST_LENGTH]; // block's content MD5 (if WRITTEN)
    } u;
};

/* Internal state */
struct ec_protect_private {
    struct ec_protect_conf      *config;
    struct s3backer_store       *inner;
    struct ec_protect_stats     stats;
    GHashTable                  *hashtable;
    TAILQ_HEAD(, block_info)    list;
    pthread_mutex_t             mutex;
    pthread_cond_t              space_cond;     // signaled when cache space available
    pthread_cond_t              never_cond;     // never signaled; used for sleeping only
    char                        *zero_block;
};

/* s3backer_store functions */
static int ec_protect_read_block(struct s3backer_store *s3b, s3b_block_t block_num, void *dest, const u_char *expect_md5);
static int ec_protect_write_block(struct s3backer_store *s3b, s3b_block_t block_num, const void *src, const u_char *md5);
static int ec_protect_detect_sizes(struct s3backer_store *s3b, off_t *file_sizep, u_int *block_sizep);
static void ec_protect_destroy(struct s3backer_store *s3b);

/* Data structure manipulation */
static struct block_info *ec_protect_hash_get(struct ec_protect_private *priv, s3b_block_t block_num);
static void ec_protect_hash_put(struct ec_protect_private *priv, struct block_info *binfo);
static void ec_protect_hash_remove(struct ec_protect_private *priv, s3b_block_t block_num);

/* Misc */
static uint64_t ec_protect_sleep_until(struct ec_protect_private *priv, pthread_cond_t *cond, uint64_t wake_time_millis);
static void ec_protect_scrub_expired_writtens(struct ec_protect_private *priv, uint64_t current_time);
static uint64_t ec_protect_get_time(void);
static void ec_protect_free_one(gpointer key, gpointer value, gpointer arg);

/* Invariants checking */
#ifndef NDEBUG
static void ec_protect_check_one(gpointer key, gpointer value, gpointer user_data);
static void ec_protect_check_invariants(struct ec_protect_private *priv);

#define EC_PROTECT_CHECK_INVARIANTS(priv)     ec_protect_check_invariants(priv)
#else
#define EC_PROTECT_CHECK_INVARIANTS(priv)     do { } while (0)
#endif

/* Special MD5 value signifying a zeroed block */
static const u_char zero_md5[MD5_DIGEST_LENGTH];

/*
 * Constructor
 *
 * On error, returns NULL and sets `errno'.
 */
struct s3backer_store *
ec_protect_create(struct ec_protect_conf *config, struct s3backer_store *inner)
{
    struct s3backer_store *s3b;
    struct ec_protect_private *priv;
    int r;

    /* Sanity check: we use block numbers as g_hash_table keys */
    if (sizeof(s3b_block_t) > sizeof(gpointer)) {
        (*config->log)(LOG_ERR, "sizeof(s3b_block_t) = %d is too big!", (int)sizeof(s3b_block_t));
        r = EINVAL;
        goto fail0;
    }

    /* Initialize structures */
    if ((s3b = calloc(1, sizeof(*s3b))) == NULL) {
        r = errno;
        goto fail0;
    }
    s3b->read_block = ec_protect_read_block;
    s3b->write_block = ec_protect_write_block;
    s3b->detect_sizes = ec_protect_detect_sizes;
    s3b->destroy = ec_protect_destroy;
    if ((priv = calloc(1, sizeof(*priv))) == NULL) {
        r = errno;
        goto fail1;
    }
    priv->config = config;
    priv->inner = inner;
    if ((r = pthread_mutex_init(&priv->mutex, NULL)) != 0)
        goto fail2;
    if ((r = pthread_cond_init(&priv->space_cond, NULL)) != 0)
        goto fail3;
    if ((r = pthread_cond_init(&priv->never_cond, NULL)) != 0)
        goto fail4;
    TAILQ_INIT(&priv->list);
    if ((priv->hashtable = g_hash_table_new(NULL, NULL)) == NULL) {
        r = errno;
        goto fail5;
    }
    s3b->data = priv;

    /* Done */
    EC_PROTECT_CHECK_INVARIANTS(priv);
    return s3b;

fail5:
    pthread_cond_destroy(&priv->never_cond);
fail4:
    pthread_cond_destroy(&priv->space_cond);
fail3:
    pthread_mutex_destroy(&priv->mutex);
fail2:
    free(priv);
fail1:
    free(s3b);
fail0:
    (*config->log)(LOG_ERR, "ec_protect creation failed: %s", strerror(r));
    errno = r;
    return NULL;
}

/*
 * Destructor
 */
static void
ec_protect_destroy(struct s3backer_store *const s3b)
{
    struct ec_protect_private *const priv = s3b->data;

    /* Grab lock and sanity check */
    pthread_mutex_lock(&priv->mutex);
    EC_PROTECT_CHECK_INVARIANTS(priv);

    /* Free structures */
    pthread_mutex_destroy(&priv->mutex);
    pthread_cond_destroy(&priv->space_cond);
    pthread_cond_destroy(&priv->never_cond);
    g_hash_table_foreach(priv->hashtable, ec_protect_free_one, NULL);
    g_hash_table_destroy(priv->hashtable);
    free(priv->zero_block);
    free(priv);
    free(s3b);
}

void
ec_protect_get_stats(struct s3backer_store *s3b, struct ec_protect_stats *stats)
{
    struct ec_protect_private *const priv = s3b->data;

    pthread_mutex_lock(&priv->mutex);
    memcpy(stats, &priv->stats, sizeof(*stats));
    stats->current_cache_size = g_hash_table_size(priv->hashtable);
    pthread_mutex_unlock(&priv->mutex);
}

static int
ec_protect_detect_sizes(struct s3backer_store *s3b, off_t *file_sizep, u_int *block_sizep)
{
    struct ec_protect_private *const priv = s3b->data;

    return (*priv->inner->detect_sizes)(priv->inner, file_sizep, block_sizep);
}

static int
ec_protect_read_block(struct s3backer_store *const s3b, s3b_block_t block_num, void *dest, const u_char *expect_md5)
{
    struct ec_protect_private *const priv = s3b->data;
    struct ec_protect_conf *const config = priv->config;
    u_char md5[MD5_DIGEST_LENGTH];
    struct block_info *binfo;

    /* Grab lock and sanity check */
    pthread_mutex_lock(&priv->mutex);
    EC_PROTECT_CHECK_INVARIANTS(priv);

    /* Scrub the list of WRITTENs */
    ec_protect_scrub_expired_writtens(priv, ec_protect_get_time());

    /* Find info for this block */
    if ((binfo = ec_protect_hash_get(priv, block_num)) != NULL) {

        /* In WRITING state: we have the data already! */
        if (binfo->timestamp == 0) {
            if (binfo->u.data == NULL)
                memset(dest, 0, config->block_size);
            else
                memcpy(dest, binfo->u.data, config->block_size);
            priv->stats.cache_data_hits++;
            pthread_mutex_unlock(&priv->mutex);
            return 0;
        }

        /* In WRITTEN state: special case: zero block */
        if (memcmp(binfo->u.md5, zero_md5, MD5_DIGEST_LENGTH) == 0) {
            memset(dest, 0, config->block_size);
            priv->stats.cache_data_hits++;
            pthread_mutex_unlock(&priv->mutex);
            return 0;
        }

        /* In WRITTEN state: we know the expected MD5 */
        memcpy(md5, binfo->u.md5, MD5_DIGEST_LENGTH);
        if (expect_md5 != NULL && memcmp(md5, expect_md5, MD5_DIGEST_LENGTH) != 0)
            (*config->log)(LOG_ERR, "ec_protect_read_block(): impossible expected MD5?");
        expect_md5 = md5;
    }

    /* Release lock */
    pthread_mutex_unlock(&priv->mutex);

    /* Read block normally */
    return (*priv->inner->read_block)(priv->inner, block_num, dest, expect_md5);
}

static int
ec_protect_write_block(struct s3backer_store *const s3b, s3b_block_t block_num, const void *src, const u_char *md5)
{
    struct ec_protect_private *const priv = s3b->data;
    struct ec_protect_conf *const config = priv->config;
    u_char md5buf[MD5_DIGEST_LENGTH];
    struct block_info *binfo;
    uint64_t current_time;
    MD5_CTX md5ctx;
    uint64_t delay;
    int r;

    /* Sanity check */
    if (config->block_size == 0)
        return EINVAL;

    /* Allocate zero block if necessary */
    if (priv->zero_block == NULL) {
        pthread_mutex_lock(&priv->mutex);
        if ((priv->zero_block = calloc(1, config->block_size)) == NULL) {
            priv->stats.out_of_memory_errors++;
            pthread_mutex_unlock(&priv->mutex);
            return ENOMEM;
        }
        pthread_mutex_unlock(&priv->mutex);
    }

    /* Special case handling for all-zeroes blocks */
    if (src == NULL || memcmp(src, priv->zero_block, config->block_size) == 0) {
        src = NULL;
        md5 = zero_md5;
    }

    /* Compute MD5 of block (if not already provided) */
    if (src != NULL && md5 == NULL) {
        MD5_Init(&md5ctx);
        MD5_Update(&md5ctx, src, config->block_size);
        MD5_Final(md5buf, &md5ctx);
        md5 = md5buf;
    }

    /* Grab lock */
    pthread_mutex_lock(&priv->mutex);

again:
    /* Sanity check */
    EC_PROTECT_CHECK_INVARIANTS(priv);

    /* Scrub the list of WRITTENs */
    current_time = ec_protect_get_time();
    ec_protect_scrub_expired_writtens(priv, current_time);

    /* Find info for this block */
    binfo = ec_protect_hash_get(priv, block_num);

    /* CLEAN case: add new entry in state WRITING and write the block */
    if (binfo == NULL) {

        /* If we have reached max cache capacity, wait until there's more room */
        if (g_hash_table_size(priv->hashtable) >= config->cache_size) {
            if ((binfo = TAILQ_FIRST(&priv->list)) != NULL)
                delay = ec_protect_sleep_until(priv, &priv->space_cond, binfo->timestamp + config->cache_time);
            else
                delay = ec_protect_sleep_until(priv, &priv->space_cond, 0);
            priv->stats.cache_full_delay += delay;
            goto again;
        }

        /* Create new entry in WRITING state */
        if ((binfo = calloc(1, sizeof(*binfo))) == NULL) {
            priv->stats.out_of_memory_errors++;
            pthread_mutex_unlock(&priv->mutex);
            return ENOMEM;
        }
        binfo->block_num = block_num;
        binfo->u.data = src;
        ec_protect_hash_put(priv, binfo);

writeit:
        /* Write the block */
        pthread_mutex_unlock(&priv->mutex);
        r = (*priv->inner->write_block)(priv->inner, block_num, src, md5);
        pthread_mutex_lock(&priv->mutex);
        EC_PROTECT_CHECK_INVARIANTS(priv);

        /* If there was an error, just return it and forget */
        if (r != 0) {
            ec_protect_hash_remove(priv, block_num);
            pthread_cond_signal(&priv->space_cond);
            pthread_mutex_unlock(&priv->mutex);
            free(binfo);
            return r;
        }

        /* Move to state WRITTEN */
        binfo->timestamp = ec_protect_get_time();
        memcpy(binfo->u.md5, md5, MD5_DIGEST_LENGTH);
        TAILQ_INSERT_TAIL(&priv->list, binfo, link);
        pthread_mutex_unlock(&priv->mutex);
        return 0;
    }

    /*
     * WRITING case: wait until current write completes (hmm, why is kernel doing overlapping writes?).
     * Since we know after current write completes we'll have to wait another 'min_write_time' milliseconds
     * anyway, we conservatively just wait exactly that long now. There may be an extra wakeup or two,
     * but that's OK.
     */
    if (binfo->timestamp == 0) {
        delay = ec_protect_sleep_until(priv, NULL, current_time + config->min_write_delay);
        priv->stats.repeated_write_delay += delay;
        goto again;
    }

    /*
     * WRITTEN case: wait until at least 'min_write_time' milliseconds has passed since previous write.
     */
    if (current_time < binfo->timestamp + config->min_write_delay) {
        delay = ec_protect_sleep_until(priv, NULL, binfo->timestamp + config->min_write_delay);
        priv->stats.repeated_write_delay += delay;
        goto again;
    }

    /*
     * WRITTEN case: 'min_write_time' milliseconds have indeed passed, so go back to WRITING.
     */
    binfo->timestamp = 0;
    binfo->u.data = src;
    TAILQ_REMOVE(&priv->list, binfo, link);
    goto writeit;
}

/*
 * Return current time in milliseconds.
 */
static uint64_t
ec_protect_get_time(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/*
 * Remove expired WRITTEN entries from the list.
 * This assumes the mutex is held.
 */
static void
ec_protect_scrub_expired_writtens(struct ec_protect_private *priv, uint64_t current_time)
{
    struct ec_protect_conf *const config = priv->config;
    struct block_info *binfo;
    int num_removed = 0;

    while ((binfo = TAILQ_FIRST(&priv->list)) != NULL && current_time >= binfo->timestamp + config->cache_time) {
        TAILQ_REMOVE(&priv->list, binfo, link);
        ec_protect_hash_remove(priv, binfo->block_num);
        free(binfo);
        num_removed++;
    }
    switch (num_removed) {
    case 0:
        break;
    case 1:
        pthread_cond_signal(&priv->space_cond);
        break;
    default:
        pthread_cond_broadcast(&priv->space_cond);
        break;
    }
}

/*
 * Sleep until specified time (if non-zero) or condition (if non-NULL).
 * Note: in rare cases there can be spurious early wakeups.
 * Returns number of milliseconds slept.
 */
static uint64_t
ec_protect_sleep_until(struct ec_protect_private *priv, pthread_cond_t *cond, uint64_t wake_time_millis)
{
    uint64_t time_before;
    uint64_t time_after;

    assert(cond != NULL || wake_time_millis != 0);
    if (cond == NULL)
        cond = &priv->never_cond;
    time_before = ec_protect_get_time();
    if (wake_time_millis != 0) {
        struct timespec wake_time;

        wake_time.tv_sec = wake_time_millis / 1000;
        wake_time.tv_nsec = (wake_time_millis % 1000) * 1000000;
        if (pthread_cond_timedwait(cond, &priv->mutex, &wake_time) == ETIMEDOUT)
            time_after = wake_time_millis;
        else
            time_after = ec_protect_get_time();
    } else {
        pthread_cond_wait(cond, &priv->mutex);
        time_after = ec_protect_get_time();
    }
    return time_after - time_before;
}

static void
ec_protect_free_one(gpointer key, gpointer value, gpointer arg)
{
    struct block_info *const binfo = value;

    free(binfo);
}

#ifndef NDEBUG

/* Accounting structure */
struct check_info {
    u_int   num_in_list;
    u_int   written;
    u_int   writing;
};

static void
ec_protect_check_one(gpointer key, gpointer value, gpointer arg)
{
    struct block_info *const binfo = value;
    struct check_info *const info = arg;

    if (binfo->timestamp == 0)
        info->writing++;
    else
        info->written++;
}

static void
ec_protect_check_invariants(struct ec_protect_private *priv)
{
    struct block_info *binfo;
    struct check_info info;

    memset(&info, 0, sizeof(info));
    for (binfo = TAILQ_FIRST(&priv->list); binfo != NULL; binfo = TAILQ_NEXT(binfo, link)) {
        assert(binfo->timestamp != 0);
        assert(ec_protect_hash_get(priv, binfo->block_num) == binfo);
        info.num_in_list++;
    }
    g_hash_table_foreach(priv->hashtable, ec_protect_check_one, &info);
    assert(info.written == info.num_in_list);
    assert(info.written + info.writing == g_hash_table_size(priv->hashtable));
}
#endif

/*
 * Find a 'struct block_info' in the hash table.
 */
static struct block_info *
ec_protect_hash_get(struct ec_protect_private *priv, s3b_block_t block_num)
{
    gconstpointer key = (gpointer)block_num;

    return (struct block_info *)g_hash_table_lookup(priv->hashtable, key);
}

/*
 * Add a 'struct block_info' to the hash table.
 */
static void
ec_protect_hash_put(struct ec_protect_private *priv, struct block_info *binfo)
{
    gpointer key = (gpointer)binfo->block_num;
#ifndef NDEBUG
    int size = g_hash_table_size(priv->hashtable);
#endif

    g_hash_table_replace(priv->hashtable, key, binfo);
#ifndef NDEBUG
    assert(g_hash_table_size(priv->hashtable) == size + 1);
#endif
}

/*
 * Remove a 'struct block_info' from the hash table.
 */
static void
ec_protect_hash_remove(struct ec_protect_private *priv, s3b_block_t block_num)
{
    gconstpointer key = (gpointer)block_num;
#ifndef NDEBUG
    int size = g_hash_table_size(priv->hashtable);
#endif

    g_hash_table_remove(priv->hashtable, key);
#ifndef NDEBUG
    assert(g_hash_table_size(priv->hashtable) == size - 1);
#endif
}
