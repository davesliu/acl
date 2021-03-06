#include "stdafx.h"
#include "common.h"

#ifdef SYS_UNIX

#if defined(__linux__)
# include <linux/version.h>
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
#  define	HAS_EVENTFD
#  include <sys/eventfd.h>
# else
# endif
#else
#endif
#include "fiber/libfiber.h"
#include "fiber.h"

struct ACL_FIBER_EVENT {
	RING             me;
	FIBER_BASE      *owner;
	ATOMIC          *atomic;
	long long        value;
	union {
		pthread_mutex_t   mutex;
		struct {
			ATOMIC   *atomic;
			long long value;
		} atomic;
	} lock;
	RING             waiters;
	unsigned long    tid;
	unsigned int     flag;
};

ACL_FIBER_EVENT *acl_fiber_event_create(unsigned flag)
{
	ACL_FIBER_EVENT *event = (ACL_FIBER_EVENT *)
		malloc(sizeof(ACL_FIBER_EVENT));

	ring_init(&event->me);
	event->owner = NULL;
	event->tid   = 0;
	event->flag  = flag;

	event->atomic = atomic_new();
	atomic_set(event->atomic, &event->value);
	atomic_int64_set(event->atomic, 0);

	if ((flag & FIBER_FLAG_USE_MUTEX)) {
		pthread_mutexattr_t attr;

		pthread_mutexattr_init(&attr);
		pthread_mutex_init(&event->lock.mutex, &attr);
		pthread_mutexattr_destroy(&attr);
	} else {
		event->lock.atomic.atomic = atomic_new();
		atomic_set(event->lock.atomic.atomic, &event->lock.atomic.value);
		atomic_int64_set(event->lock.atomic.atomic, 0);
	}

	ring_init(&event->waiters);
	return event;
}

void acl_fiber_event_free(ACL_FIBER_EVENT *event)
{
	atomic_free(event->atomic);
	if ((event->flag & FIBER_FLAG_USE_MUTEX)) {
		pthread_mutex_destroy(&event->lock.mutex);
	} else {
		atomic_free(event->lock.atomic.atomic);
	}

	free(event);
}

static void channel_open(FIBER_BASE *fbase)
{
#if defined(HAS_EVENTFD)
	int flags = 0;
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
	flags |= FD_CLOEXEC;
# endif
	flags = 0;
	if (fbase->event_in == -1) {
		fbase->event_in  = eventfd(0, flags);
		fbase->event_out = fbase->event_in;
	}
#else
	int fds[2];

	if (fbase->event_in >= 0) {
		assert(fbase->event_out >= 0);
		return;
	}

	if (sane_socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
		msg_fatal("%s(%d), %s: acl_duplex_pipe error %s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}
	fbase->event_in  = fds[0];
	fbase->event_out = fds[1];
#endif
}

void fbase_event_close(FIBER_BASE *fbase)
{
	if (fbase->event_in >= 0) {
		close(fbase->event_in);
	}
	if (fbase->event_out != fbase->event_in && fbase->event_out >= 0) {
		close(fbase->event_out);
	}
	fbase->event_in  = -1;
	fbase->event_out = -2;
	atomic_int64_set(fbase->atomic, 0);
}

static int fbase_event_wait(FIBER_BASE *fbase)
{
	long long n;

	assert(fbase->event_in >= 0);
	if (read(fbase->event_in, &n, sizeof(n)) != sizeof(n)) {
		msg_error("%s(%d), %s: read error %s, in=%d",
			__FILE__, __LINE__, __FUNCTION__,
			last_serror(), fbase->event_in);
		return -1;
	}
	/*
	if (atomic_int64_cas(fbase->atomic, 1, 0) != 1) {
		msg_fatal("%s(%d), %s: atomic corrupt",
			__FILE__, __LINE__, __FUNCTION__);
	}
	*/
	return 0;
}

static int fbase_event_wakeup(FIBER_BASE *fbase)
{
	long long n = 1;

	/*
	if (LIKELY(atomic_int64_cas(fbase->atomic, 0, 1) != 0)) {
		return 0;
	}
	*/

	assert(fbase->event_out >= 0);
	if (write(fbase->event_out, &n, sizeof(n)) != sizeof(n)) {
		msg_error("%s(%d), %s: write error %s, out=%d",
			__FILE__, __LINE__, __FUNCTION__,
			last_serror(), fbase->event_out);
		return -1;
	}

	return 0;
}

static inline void __ll_lock(ACL_FIBER_EVENT *event)
{
	if ((event->flag & FIBER_FLAG_USE_MUTEX)) {
		assert(pthread_mutex_lock(&event->lock.mutex) == 0);
	} else {
		while (atomic_int64_cas(event->lock.atomic.atomic, 0, 1)) {}
	}
}

static inline void __ll_unlock(ACL_FIBER_EVENT *event)
{
	if ((event->flag & FIBER_FLAG_USE_MUTEX)) {
		assert(pthread_mutex_unlock(&event->lock.mutex) == 0);
	} else if (atomic_int64_cas(event->lock.atomic.atomic, 1, 0) != 1) {
		msg_fatal("%s(%d), %s: lock corrupt",
			__FILE__, __LINE__, __FUNCTION__);
	}
}

int acl_fiber_event_wait(ACL_FIBER_EVENT *event)
{
	ACL_FIBER  *fiber = acl_fiber_running();
	FIBER_BASE *fbase;
	unsigned    wakeup;

	if (LIKELY(atomic_int64_cas(event->atomic, 0, 1) == 0)) {
		event->owner = fiber ? &fiber->base : NULL;
		event->tid   = __pthread_self();
		return 0;
	}

	// FIBER_BASE obj will be created if is not in fiber scheduled
	fbase = fiber ? &fiber->base : fbase_alloc();
	channel_open(fbase);

	wakeup = 0;
	__ll_lock(event);

	ring_prepend(&event->waiters, &fbase->event_waiter);

	while (1) {
		if (atomic_int64_cas(event->atomic, 0, 1) == 0) {
			if (!wakeup) {
				ring_detach(&fbase->event_waiter);
			}

			__ll_unlock(event);
			event->owner = fbase;
			event->tid   = __pthread_self();
			break;
		}

		if (wakeup) {
			ring_prepend(&event->waiters, &fbase->event_waiter);
		}
		__ll_unlock(event);

		if (fbase_event_wait(fbase) == -1) {
			fbase_event_close(fbase);
			if (fbase->flag & FBASE_F_BASE) {
				fbase_free(fbase);
			}

			msg_fatal("%s(%d), %s: event wait error %s", __FILE__,
				__LINE__, __FUNCTION__, last_serror());
			return -1;
		}

		// overflow ?
		if (++wakeup == 0) {
			wakeup = 1;
		}
		__ll_lock(event);
	}

	fbase_event_close(fbase);
	if (fbase->flag & FBASE_F_BASE) {
		event->owner = NULL;
		fbase_free(fbase);
	}
	return 0;
}

int acl_fiber_event_trywait(ACL_FIBER_EVENT *event)
{
	if (atomic_int64_cas(event->atomic, 0, 1) == 0) {
		ACL_FIBER *fiber = acl_fiber_running();
		event->owner     = fiber ? &fiber->base : NULL;
		event->tid       = __pthread_self();
		return 0;
	}
	return -1;
}

int acl_fiber_event_notify(ACL_FIBER_EVENT *event)
{
	ACL_FIBER  *curr  = acl_fiber_running();
	FIBER_BASE *owner = curr ? &curr->base : NULL, *waiter;
	RING       *head;

	if (UNLIKELY(event->owner != owner)) {
		msg_fatal("%s(%d), %s: fiber(%p) is not the owner(%p)",
			__FILE__, __LINE__, __FUNCTION__, owner, event->owner);
		return -1;
	} else if (UNLIKELY(event->owner == NULL
		&& event->tid != __pthread_self())) {

		msg_fatal("%s(%d), %s: tid(%lu) is not the owner(%lu)",
			__FILE__, __LINE__, __FUNCTION__,
			event->tid, __pthread_self());
		return -1;
	}

	__ll_lock(event);

	head = ring_pop_head(&event->waiters);
	if (head) {
		waiter = RING_TO_APPL(head, FIBER_BASE, event_waiter);
	} else {
		waiter = NULL;
	}

	__ll_unlock(event);
	if (atomic_int64_cas(event->atomic, 1, 0) != 1) {
		msg_fatal("%s(%d), %s: atomic corrupt",
			__FILE__, __LINE__, __FUNCTION__);
	}

	if (waiter && fbase_event_wakeup(waiter) == -1) {
		msg_fatal("%s(%d), %s: wakup waiter error=%s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
		return -1;
	}
	return 0;
}

#endif // SYS_UNIX
