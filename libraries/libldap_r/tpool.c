/* $OpenLDAP$ */
/*
 * Copyright 1998-2000 The OpenLDAP Foundation, Redwood City, California, USA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted only
 * as authorized by the OpenLDAP Public License.  A copy of this
 * license is available at http://www.OpenLDAP.org/license.html or
 * in file LICENSE in the top-level directory of the distribution.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/stdarg.h>
#include <ac/stdlib.h>
#include <ac/string.h>
#include <ac/time.h>

#include "ldap-int.h"
#include "ldap_pvt_thread.h"
#include "queue-compat.h"

#ifndef LDAP_THREAD_HAVE_TPOOL

enum ldap_int_thread_pool_state {
	LDAP_INT_THREAD_POOL_RUNNING,
	LDAP_INT_THREAD_POOL_FINISHING,
	LDAP_INT_THREAD_POOL_STOPPING
};

typedef struct ldap_int_thread_ctx_s {
	union {
	SLIST_ENTRY(ldap_int_thread_ctx_s) l;
	STAILQ_ENTRY(ldap_int_thread_ctx_s) q;
	} ltc_next;
	void *(*ltc_start_routine)( void *);
	void *ltc_arg;
} ldap_int_thread_ctx_t;

struct ldap_int_thread_pool_s {
	STAILQ_ENTRY(ldap_int_thread_pool_s) ltp_next;
	ldap_pvt_thread_mutex_t ltp_mutex;
	ldap_pvt_thread_cond_t ltp_cond;
	STAILQ_HEAD(tcq, ldap_int_thread_ctx_s) ltp_pending_list;
	long ltp_state;
	long ltp_max_count;
	long ltp_max_pending;
	long ltp_pending_count;
	long ltp_active_count;
	long ltp_open_count;
	long ltp_starting;
};

static STAILQ_HEAD(tpq, ldap_int_thread_pool_s)
	ldap_int_thread_pool_list =
	STAILQ_HEAD_INITIALIZER(ldap_int_thread_pool_list);

static SLIST_HEAD(tcl, ldap_int_thread_ctx_s)
	ldap_int_ctx_free_list = 
	SLIST_HEAD_INITIALIZER(ldap_int_ctx_free_list);

static ldap_pvt_thread_mutex_t ldap_pvt_thread_pool_mutex;
static ldap_pvt_thread_mutex_t ldap_pvt_ctx_free_mutex;

static void *ldap_int_thread_pool_wrapper(
	struct ldap_int_thread_pool_s *pool );

int
ldap_int_thread_pool_startup ( void )
{
	int rc = ldap_pvt_thread_mutex_init(&ldap_pvt_thread_pool_mutex);
	if (rc == 0)
		rc = ldap_pvt_thread_mutex_init(&ldap_pvt_ctx_free_mutex);
	return rc;
}

int
ldap_int_thread_pool_shutdown ( void )
{
	ldap_int_thread_ctx_t *ctx;
	struct ldap_int_thread_pool_s *pool;

	while ((pool = STAILQ_FIRST(&ldap_int_thread_pool_list)) != NULL) {
		STAILQ_REMOVE_HEAD(&ldap_int_thread_pool_list, ltp_next);
		ldap_pvt_thread_pool_destroy( &pool, 0);
	}
	while ((ctx = SLIST_FIRST(&ldap_int_ctx_free_list))) {
		SLIST_REMOVE_HEAD(&ldap_int_ctx_free_list, ltc_next.l);
		free(ctx);
	}
	ldap_pvt_thread_mutex_destroy(&ldap_pvt_ctx_free_mutex);
	ldap_pvt_thread_mutex_destroy(&ldap_pvt_thread_pool_mutex);
	return(0);
}

int
ldap_pvt_thread_pool_init (
	ldap_pvt_thread_pool_t *tpool,
	int max_threads,
	int max_pending )
{
	ldap_pvt_thread_pool_t pool;
	int rc;

	*tpool = NULL;
	pool = (ldap_pvt_thread_pool_t) LDAP_CALLOC(1,
		sizeof(struct ldap_int_thread_pool_s));

	if (pool == NULL) return(-1);

	rc = ldap_pvt_thread_mutex_init(&pool->ltp_mutex);
	if (rc != 0)
		return(rc);
	rc = ldap_pvt_thread_cond_init(&pool->ltp_cond);
	if (rc != 0)
		return(rc);
	pool->ltp_state = LDAP_INT_THREAD_POOL_RUNNING;
	pool->ltp_max_count = max_threads;
	pool->ltp_max_pending = max_pending;
	ldap_pvt_thread_mutex_lock(&ldap_pvt_thread_pool_mutex);
	if (STAILQ_EMPTY(&ldap_int_thread_pool_list)) {
		STAILQ_INSERT_HEAD(&ldap_int_thread_pool_list, pool, ltp_next);
	} else {
		STAILQ_INSERT_TAIL(&ldap_int_thread_pool_list, pool, ltp_next);
	}
	ldap_pvt_thread_mutex_unlock(&ldap_pvt_thread_pool_mutex);

#if 0
	/* THIS WILL NOT WORK on some systems.  If the process
	 * forks after starting a thread, there is no guarantee
	 * that the thread will survive the fork.  For example,
	 * slapd forks in order to daemonize, and does so after
	 * calling ldap_pvt_thread_pool_init.  On some systems,
	 * this initial thread does not run in the child process,
	 * but ltp_open_count == 1, so two things happen: 
	 * 1) the first client connection fails, and 2) when
	 * slapd is kill'ed, it never terminates since it waits
	 * for all worker threads to exit. */

	/* start up one thread, just so there is one. no need to
	 * lock the mutex right now, since no threads are running.
	 */
	pool->ltp_open_count++;

	ldap_pvt_thread_t thr;
	rc = ldap_pvt_thread_create( &thr, 1,
		(void *) ldap_int_thread_pool_wrapper, pool );

	if( rc != 0) {
		/* couldn't start one?  then don't start any */
		ldap_pvt_thread_mutex_lock(&ldap_pvt_thread_pool_mutex);
		STAILQ_REMOVE(ldap_int_thread_pool_list, pool, 
			ldap_int_thread_element_s, ltp_next);
		ldap_pvt_thread_mutex_unlock(&ldap_pvt_thread_pool_mutex);
		ldap_pvt_thread_cond_destroy(&pool->ltp_cond);
		ldap_pvt_thread_mutex_destroy(&pool->ltp_mutex);
		free(pool);
		return(-1);
	}
#endif

	*tpool = pool;
	return(0);
}

int
ldap_pvt_thread_pool_submit (
	ldap_pvt_thread_pool_t *tpool,
	void *(*start_routine)( void * ), void *arg )
{
	struct ldap_int_thread_pool_s *pool;
	ldap_int_thread_ctx_t *ctx;
	int need_thread = 0;
	ldap_pvt_thread_t thr;

	if (tpool == NULL)
		return(-1);

	pool = *tpool;

	if (pool == NULL)
		return(-1);

	ldap_pvt_thread_mutex_lock(&pool->ltp_mutex);
	if (pool->ltp_state != LDAP_INT_THREAD_POOL_RUNNING
		|| (pool->ltp_max_pending > 0
			&& pool->ltp_pending_count >= pool->ltp_max_pending))
	{
		ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);
		return(-1);
	}
	ldap_pvt_thread_mutex_lock(&ldap_pvt_ctx_free_mutex);
	ctx = SLIST_FIRST(&ldap_int_ctx_free_list);
	if (ctx) {
		SLIST_REMOVE_HEAD(&ldap_int_ctx_free_list, ltc_next.l);
		ldap_pvt_thread_mutex_unlock(&ldap_pvt_ctx_free_mutex);
	} else {
		ldap_pvt_thread_mutex_unlock(&ldap_pvt_ctx_free_mutex);
		ctx = (ldap_int_thread_ctx_t *) LDAP_MALLOC(
			sizeof(ldap_int_thread_ctx_t));
		if (ctx == NULL) {
			ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);
			return(-1);
		}
	}

	ctx->ltc_start_routine = start_routine;
	ctx->ltc_arg = arg;

	pool->ltp_pending_count++;
	if (STAILQ_EMPTY(&pool->ltp_pending_list)) {
		STAILQ_INSERT_HEAD(&pool->ltp_pending_list, ctx, ltc_next.q);
	} else {
		STAILQ_INSERT_TAIL(&pool->ltp_pending_list, ctx, ltc_next.q);
	}
	ldap_pvt_thread_cond_signal(&pool->ltp_cond);
	if ((pool->ltp_open_count <= 0
			|| pool->ltp_pending_count > 1
			|| pool->ltp_open_count == pool->ltp_active_count)
		&& (pool->ltp_max_count <= 0
			|| pool->ltp_open_count < pool->ltp_max_count))
	{
		pool->ltp_open_count++;
		pool->ltp_starting++;
		need_thread = 1;
	}
	ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);

	if (need_thread) {
		int rc = ldap_pvt_thread_create( &thr, 1,
			(void *)ldap_int_thread_pool_wrapper, pool );
		ldap_pvt_thread_mutex_lock(&pool->ltp_mutex);
		if (rc == 0) {
			pool->ltp_starting--;
		} else {
			/* couldn't create thread.  back out of
			 * ltp_open_count and check for even worse things.
			 */
			pool->ltp_open_count--;
			pool->ltp_starting--;
			if (pool->ltp_open_count == 0) {
				/* no open threads at all?!?
				 */
				ldap_int_thread_ctx_t *ptr;
				STAILQ_FOREACH(ptr, &pool->ltp_pending_list, ltc_next.q)
					if (ptr == ctx) break;
				if (ptr == ctx) {
					/* no open threads, context not handled, so
					 * back out of ltp_pending_count, free the context,
					 * report the error.
					 */
					STAILQ_REMOVE(&pool->ltp_pending_list, ctx, 
						ldap_int_thread_ctx_s, ltc_next.q);
					pool->ltp_pending_count++;
					ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);
					free(ctx);
					return(-1);
				}
			}
			/* there is another open thread, so this
			 * context will be handled eventually.
			 * continue on and signal that the context
			 * is waiting.
			 */
		}
		ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);
	}

	return(0);
}

int
ldap_pvt_thread_pool_maxthreads ( ldap_pvt_thread_pool_t *tpool, int max_threads )
{
	struct ldap_int_thread_pool_s *pool;

	if (tpool == NULL)
		return(-1);

	pool = *tpool;

	if (pool == NULL)
		return(-1);

	ldap_pvt_thread_mutex_lock(&pool->ltp_mutex);
	pool->ltp_max_count = max_threads;
	ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);
	return(0);
}

int
ldap_pvt_thread_pool_backload ( ldap_pvt_thread_pool_t *tpool )
{
	struct ldap_int_thread_pool_s *pool;
	int count;

	if (tpool == NULL)
		return(-1);

	pool = *tpool;

	if (pool == NULL)
		return(0);

	ldap_pvt_thread_mutex_lock(&pool->ltp_mutex);
	count = pool->ltp_pending_count + pool->ltp_active_count;
	ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);
	return(count);
}

int
ldap_pvt_thread_pool_destroy ( ldap_pvt_thread_pool_t *tpool, int run_pending )
{
	struct ldap_int_thread_pool_s *pool, *pptr;
	long waiting;
	ldap_int_thread_ctx_t *ctx;

	if (tpool == NULL)
		return(-1);

	pool = *tpool;

	if (pool == NULL) return(-1);

	ldap_pvt_thread_mutex_lock(&ldap_pvt_thread_pool_mutex);
	STAILQ_FOREACH(pptr, &ldap_int_thread_pool_list, ltp_next)
		if (pptr == pool) break;
	if (pptr == pool)
		STAILQ_REMOVE(&ldap_int_thread_pool_list, pool,
			ldap_int_thread_pool_s, ltp_next);
	ldap_pvt_thread_mutex_unlock(&ldap_pvt_thread_pool_mutex);

	if (pool != pptr) return(-1);

	ldap_pvt_thread_mutex_lock(&pool->ltp_mutex);
	pool->ltp_state = run_pending
		? LDAP_INT_THREAD_POOL_FINISHING
		: LDAP_INT_THREAD_POOL_STOPPING;
	waiting = pool->ltp_open_count;

	/* broadcast could be used here, but only after
	 * it is fixed in the NT thread implementation
	 */
	while (--waiting >= 0) {
		ldap_pvt_thread_cond_signal(&pool->ltp_cond);
	}
	ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);

	do {
		ldap_pvt_thread_yield();
		ldap_pvt_thread_mutex_lock(&pool->ltp_mutex);
		waiting = pool->ltp_open_count;
		ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);
	} while (waiting > 0);

	while ((ctx = STAILQ_FIRST(&pool->ltp_pending_list)) != NULL)
	{
		STAILQ_REMOVE_HEAD(&pool->ltp_pending_list, ltc_next.q);
		free(ctx);
	}

	ldap_pvt_thread_cond_destroy(&pool->ltp_cond);
	ldap_pvt_thread_mutex_destroy(&pool->ltp_mutex);
	free(pool);
	return(0);
}

static void *
ldap_int_thread_pool_wrapper ( 
	struct ldap_int_thread_pool_s *pool )
{
	ldap_int_thread_ctx_t *ctx;

	if (pool == NULL)
		return NULL;

	ldap_pvt_thread_mutex_lock(&pool->ltp_mutex);

	while (pool->ltp_state != LDAP_INT_THREAD_POOL_STOPPING) {
		ctx = STAILQ_FIRST(&pool->ltp_pending_list);
		if (ctx) {
			STAILQ_REMOVE_HEAD(&pool->ltp_pending_list, ltc_next.q);
		} else {
			if (pool->ltp_state == LDAP_INT_THREAD_POOL_FINISHING)
				break;
			if (pool->ltp_max_count > 0
				&& pool->ltp_open_count > pool->ltp_max_count)
			{
				/* too many threads running (can happen if the
				 * maximum threads value is set during ongoing
				 * operation using ldap_pvt_thread_pool_maxthreads)
				 * so let this thread die.
				 */
				break;
			}

			/* we could check an idle timer here, and let the
			 * thread die if it has been inactive for a while.
			 * only die if there are other open threads (i.e.,
			 * always have at least one thread open).  the check
			 * should be like this:
			 *   if (pool->ltp_open_count > 1 && pool->ltp_starting == 0)
			 *       check timer, leave thread (break;)
			 */

			if (pool->ltp_state == LDAP_INT_THREAD_POOL_RUNNING)
				ldap_pvt_thread_cond_wait(&pool->ltp_cond, &pool->ltp_mutex);

			continue;
		}

		pool->ltp_pending_count--;
		pool->ltp_active_count++;
		ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);

		(ctx->ltc_start_routine)(ctx->ltc_arg);
		ldap_pvt_thread_mutex_lock(&ldap_pvt_ctx_free_mutex);
		SLIST_INSERT_HEAD(&ldap_int_ctx_free_list, ctx, ltc_next.l);
		ldap_pvt_thread_mutex_unlock(&ldap_pvt_ctx_free_mutex);
		ldap_pvt_thread_yield();

		/* if we use an idle timer, here's
		 * a good place to update it
		 */

		ldap_pvt_thread_mutex_lock(&pool->ltp_mutex);
		pool->ltp_active_count--;
	}

	pool->ltp_open_count--;
	ldap_pvt_thread_mutex_unlock(&pool->ltp_mutex);

	ldap_pvt_thread_exit(NULL);
	return(NULL);
}
#endif /* LDAP_HAVE_THREAD_POOL */
