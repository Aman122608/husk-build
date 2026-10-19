/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pthreads for guest code.
 *
 * bionic's synchronisation objects are small and must work from zeroed memory:
 * guests embed them in their own structures at bionic's sizes (a mutex is 40 bytes,
 * a condition variable 48, a read-write lock 56, a semaphore 32) and rely on an
 * all-zero object being a valid normal mutex. Darwin's objects are bigger and have
 * their own layouts, so each guest object here holds a small header -- a flags word,
 * a magic, and a pointer to a real Darwin object made on first use -- and fits in the
 * guest's bytes with room to spare.
 *
 * A zeroed object has magic 0 and is initialised on first use, racing safely: one
 * thread wins the transition to "initialising" and the others spin until it is done.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-bionic.h"
#include "husk-tl-ld.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* General-dynamic thread-local access: the code passes {module, offset}. */
void *tl_ld_tls_get_addr(uint64_t module, uint64_t offset);
static void *b___tls_get_addr(const uint64_t *ti) { return tl_ld_tls_get_addr(ti[0], ti[1]); }


#define MAGIC_READY 0x7468726du   /* "thrm" */
#define MAGIC_INIT  1u

typedef struct { uint32_t value; _Atomic uint32_t magic; void *host; } guest_obj;

/* The per-thread lock-wait marker (writer defined with the census table, further down): set while THIS thread is inside a
 * shim lock call, so the stall probe can tell a parked thread blocked INSIDE A GUEST LOCK from a merely idle one -- a
 * worker pool waiting for work looks identical to a crowd on a lock from outside. Forward-declared because the lock
 * shims below use it. The kinds are TL_GW_* (husk-tl-bionic.h), shared with the census reader. */
static void gt_wait_enter(int kind, const void *obj, void *lr);
static void gt_wait_leave(void);

/* Get (making if needed) the Darwin object behind a guest object. `make` builds one
 * from the guest's flags word. */
static void *obj_host(void *g, void *(*make)(uint32_t value))
{
    guest_obj *o = g;
    uint32_t m = atomic_load_explicit(&o->magic, memory_order_acquire);
    for (;;) {
        if (m == MAGIC_READY) return o->host;
        if (m == MAGIC_INIT) { sched_yield(); m = atomic_load_explicit(&o->magic, memory_order_acquire); continue; }
        if (atomic_compare_exchange_weak_explicit(&o->magic, &m, MAGIC_INIT, memory_order_acq_rel, memory_order_acquire)) {
            o->host = make(o->value);
            atomic_store_explicit(&o->magic, MAGIC_READY, memory_order_release);
            return o->host;
        }
    }
}

static int rc(int darwin) { return darwin ? tl_errno_to_guest(darwin) : 0; }

/* ---------------------------------------------------------------- mutexes */

/* bionic types: NORMAL 0, RECURSIVE 1, ERRORCHECK 2. Darwin's: NORMAL 0, ERRORCHECK 1, RECURSIVE 2. */
static void *make_mutex(uint32_t value)
{
    unsigned type = (value >> 14) & 3;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type == 1 ? PTHREAD_MUTEX_RECURSIVE : type == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_t *m = malloc(sizeof(*m));
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

static int b_mutex_init(void *g, const long *attr)
{
    guest_obj *o = g;
    memset(g, 0, 40);
    unsigned type = attr ? (unsigned)(*attr & 0xf) : 0;
    o->value = (type & 3) << 14;
    return 0;                       /* the Darwin object appears on first use */
}
static int b_mutex_destroy(void *g)
{
    guest_obj *o = g;
    if (atomic_load(&o->magic) == MAGIC_READY) { pthread_mutex_destroy(o->host); free(o->host); }
    uint32_t v = o->value;
    memset(g, 0, 40);
    o->value = v;
    return 0;
}
/*
 * The guest-mutex OWNER RECORD: remember who last locked each mutex (in the spare bytes of the guest's 40-byte object,
 * which the guest never touches) and from where it was locked, cleared on unlock. This is the cheap always-on part that
 * lets the STALL-TIME CENSUS name a held mutex and its holder -- tl_guest_mutex_waits() reads w[2]/w[3] at a stall. It is
 * two stores on acquisition (note_owner) and two on release (b_mutex_unlock), on the HELD-mutex path only, and NOTHING on
 * the contended path: b_mutex_lock below is a real block on the host pthread mutex, so a guest waiter SLEEPS on it -- it
 * never spins, never polls.
 *
 * WHY THERE IS NO PER-WAIT "has waited 5 s" LINE ANY MORE. That line could only be produced by watching a wait in
 * progress, which meant replacing the blocking lock with a trylock+usleep(200) poll (a 5 s wait cost ~25000 iterations of
 * it). This project's whole point is performance: an always-on spin loop on EVERY contended guest mutex taxes every run
 * forever to feed a diagnostic, on a device where a stalled title already gets 8 frames instead of thousands. A diagnostic
 * may not cost more than the bug it observes, so the poll -- and the line it fed -- is gone. The record above cannot report
 * a wait IN PROGRESS (nothing is written while a thread is blocked), but the same question ("which held guest mutex is a
 * live thread blocked on, and from where?") is answered AFTER the fact by the stall-time census from the recorded owner
 * alone: a mutex HELD with a live waiter still gets named with both sites, and that is the question being asked.
 */
static void note_owner(void *g, void *lr) { uint64_t *w = g; w[2] = (uint64_t)(uintptr_t)pthread_self(); w[3] = (uint64_t)(uintptr_t)lr; }
static int b_mutex_lock(void *g)
{
    tl_hp_bump(TL_HP_MUTEX_LOCK);
    pthread_mutex_t *m = obj_host(g, make_mutex);
    void *lr = __builtin_return_address(0);
    /* Mark the thread as inside a guest mutex operation for as long as it is here, so a probe can tell a thread blocked
     * in this lock from an idle one. Cleared on EVERY exit path below. */
    gt_wait_enter(TL_GW_MUTEX, g, lr);
    /* A real block: the waiter SLEEPS on the host mutex. No trylock, no usleep, no spin -- on the fast path this is one
     * host pthread_mutex_lock plus the two owner stores below. */
    int r = pthread_mutex_lock(m);
    if (r == 0) note_owner(g, lr);   /* the holder's tid + lock site, held until unlock clears it */
    gt_wait_leave();
    return rc(r);
}
static int b_mutex_trylock(void *g) { int r = pthread_mutex_trylock(obj_host(g, make_mutex)); if (r == 0) note_owner(g, __builtin_return_address(0)); return rc(r); }
static int b_mutex_unlock(void *g)  { tl_hp_bump(TL_HP_MUTEX_UNLOCK); ((uint64_t *)g)[2] = 0; return rc(pthread_mutex_unlock(obj_host(g, make_mutex))); }

static int b_mutexattr_init(long *a)    { *a = 0; return 0; }
static int b_mutexattr_destroy(long *a) { (void)a; return 0; }
static int b_mutexattr_settype(long *a, int type)
{
    if (type < 0 || type > 2) return 22;
    *a = (*a & ~0xfL) | type;
    return 0;
}
static int b_mutexattr_gettype(const long *a, int *type) { *type = (int)(*a & 0xf); return 0; }
/* process-shared: this is one process, so remembered and nothing more */
static int b_mutexattr_setpshared(long *a, int ps) { if (ps != 0 && ps != 1) return 22; *a = (*a & ~0x10L) | (ps ? 0x10L : 0); return 0; }
static int b_mutexattr_getpshared(const long *a, int *ps) { *ps = (*a & 0x10L) ? 1 : 0; return 0; }

/* ------------------------------------------------------ condition variables */

static void *make_cond(uint32_t value)
{
    (void)value;
    pthread_cond_t *c = malloc(sizeof(*c));
    pthread_cond_init(c, NULL);
    return c;
}

static int b_cond_init(void *g, const long *attr)
{
    guest_obj *o = g;
    memset(g, 0, 48);
    o->value = attr ? (uint32_t)(*attr & 3) : 0;
    return 0;
}
static int b_cond_destroy(void *g)
{
    guest_obj *o = g;
    if (atomic_load(&o->magic) == MAGIC_READY) { pthread_cond_destroy(o->host); free(o->host); }
    memset(g, 0, 48);
    return 0;
}
static int b_cond_signal(void *g)    { return rc(pthread_cond_signal(obj_host(g, make_cond))); }
static int b_cond_broadcast(void *g) { return rc(pthread_cond_broadcast(obj_host(g, make_cond))); }
static int b_cond_wait(void *g, void *m)
{
    tl_hp_bump(TL_HP_COND_WAIT);
    gt_wait_enter(TL_GW_COND, g, __builtin_return_address(0));
    int r = rc(pthread_cond_wait(obj_host(g, make_cond), obj_host(m, make_mutex)));
    gt_wait_leave();
    return r;
}

static int b_cond_timedwait(void *g, void *m, const struct timespec *abs)
{
    guest_obj *o = g;
    obj_host(g, make_cond);
    clockid_t clk = (o->value & 2) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    struct timespec now, rel;
    clock_gettime(clk, &now);
    rel.tv_sec = abs->tv_sec - now.tv_sec;
    rel.tv_nsec = abs->tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
    if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; }
    return rc(pthread_cond_timedwait_relative_np(o->host, obj_host(m, make_mutex), &rel));
}

/* Bionic's clock ids: 0 is CLOCK_REALTIME, 1 CLOCK_MONOTONIC. */
static int cond_wait_until(void *g, void *m, clockid_t clk, const struct timespec *abs)
{
    guest_obj *o = g;
    obj_host(g, make_cond);
    struct timespec now, rel;
    clock_gettime(clk, &now);
    rel.tv_sec = abs->tv_sec - now.tv_sec;
    rel.tv_nsec = abs->tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
    if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; }
    return rc(pthread_cond_timedwait_relative_np(o->host, obj_host(m, make_mutex), &rel));
}

static int b_cond_clockwait(void *g, void *m, int clock, const struct timespec *abs)
{
    if (clock != 0 && clock != 1) return 22;
    return cond_wait_until(g, m, clock == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, abs);
}

static int b_cond_timedwait_monotonic(void *g, void *m, const struct timespec *abs) { return cond_wait_until(g, m, CLOCK_MONOTONIC, abs); }

static int b_condattr_init(long *a)    { *a = 0; return 0; }
static int b_condattr_destroy(long *a) { (void)a; return 0; }
static int b_condattr_setpshared(long *a, int ps) { if (ps != 0 && ps != 1) return 22; *a = (*a & ~1L) | (ps ? 1L : 0); return 0; }
static int b_condattr_getpshared(const long *a, int *ps) { *ps = (int)(*a & 1L); return 0; }
static int b_condattr_setclock(long *a, int clock)
{
    if (clock == 0) *a &= ~2L;
    else if (clock == 1) *a |= 2L;
    else return 22;
    return 0;
}

/* -------------------------------------------------------- read-write locks */

static void *make_rwlock(uint32_t value)
{
    (void)value;
    pthread_rwlock_t *l = malloc(sizeof(*l));
    pthread_rwlock_init(l, NULL);
    return l;
}
static int b_rwlock_init(void *g, const void *attr) { (void)attr; memset(g, 0, 56); return 0; }
static int b_rwlock_destroy(void *g)
{
    guest_obj *o = g;
    if (atomic_load(&o->magic) == MAGIC_READY) { pthread_rwlock_destroy(o->host); free(o->host); }
    memset(g, 0, 56);
    return 0;
}
static int b_rwlock_rdlock(void *g)    { gt_wait_enter(TL_GW_RWLOCK, g, __builtin_return_address(0)); int r = rc(pthread_rwlock_rdlock(obj_host(g, make_rwlock))); gt_wait_leave(); return r; }
static int b_rwlock_wrlock(void *g)    { gt_wait_enter(TL_GW_RWLOCK, g, __builtin_return_address(0)); int r = rc(pthread_rwlock_wrlock(obj_host(g, make_rwlock))); gt_wait_leave(); return r; }
static int b_rwlock_tryrdlock(void *g) { return rc(pthread_rwlock_tryrdlock(obj_host(g, make_rwlock))); }
static int b_rwlock_trywrlock(void *g) { return rc(pthread_rwlock_trywrlock(obj_host(g, make_rwlock))); }
static int b_rwlock_unlock(void *g)    { return rc(pthread_rwlock_unlock(obj_host(g, make_rwlock))); }

/* ---------------------------------------------------------------- semaphores */

/* bionic's sem_t is 16 bytes on LP64 (a count and three reserved words): clearing more tramples the next field of whatever embeds it. */

typedef struct { pthread_mutex_t m; pthread_cond_t c; int count; } host_sem;

static void *make_sem(uint32_t value)
{
    host_sem *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->count = (int)value;
    return s;
}

static int b_sem_init(void *g, int pshared, unsigned value)
{
    (void)pshared;
    guest_obj *o = g;
    memset(g, 0, 16);
    o->host = make_sem(value);
    atomic_store(&o->magic, MAGIC_READY);
    return 0;
}
static int b_sem_destroy(void *g)
{
    guest_obj *o = g;
    if (atomic_load(&o->magic) == MAGIC_READY) {
        host_sem *s = o->host;
        pthread_cond_destroy(&s->c); pthread_mutex_destroy(&s->m); free(s);
    }
    memset(g, 0, 16);
    return 0;
}
static host_sem *hs(void *g) { return obj_host(g, make_sem); }
static int b_sem_post(void *g)
{
    host_sem *s = hs(g);
    pthread_mutex_lock(&s->m);
    s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return 0;
}
static int b_sem_wait(void *g)
{
    host_sem *s = hs(g);
    gt_wait_enter(TL_GW_SEM, g, __builtin_return_address(0));
    pthread_mutex_lock(&s->m);
    while (s->count <= 0) pthread_cond_wait(&s->c, &s->m);
    s->count--;
    pthread_mutex_unlock(&s->m);
    gt_wait_leave();
    return 0;
}
static int b_sem_trywait(void *g)
{
    host_sem *s = hs(g);
    int ok = 0;
    pthread_mutex_lock(&s->m);
    if (s->count > 0) { s->count--; ok = 1; }
    pthread_mutex_unlock(&s->m);
    if (!ok) { tl_set_guest_errno(11); return -1; }
    return 0;
}
static int b_sem_timedwait(void *g, const struct timespec *abs)
{
    host_sem *s = hs(g);
    struct timespec now, rel;
    clock_gettime(CLOCK_REALTIME, &now);
    rel.tv_sec = abs->tv_sec - now.tv_sec;
    rel.tv_nsec = abs->tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
    if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; }
    pthread_mutex_lock(&s->m);
    int r = 0;
    while (s->count <= 0 && r == 0) r = pthread_cond_timedwait_relative_np(&s->c, &s->m, &rel);
    if (r == 0) s->count--;
    pthread_mutex_unlock(&s->m);
    if (r) { tl_set_guest_errno(tl_errno_to_guest(r)); return -1; }
    return 0;
}
static int b_sem_getvalue(void *g, int *v)
{
    host_sem *s = hs(g);
    pthread_mutex_lock(&s->m);
    *v = s->count;
    pthread_mutex_unlock(&s->m);
    return 0;
}

/* --------------------------------------------------------------- once, keys */

static int b_once(_Atomic int *once, void (*fn)(void))
{
    int expect = 0;
    if (atomic_compare_exchange_strong(once, &expect, 2)) {
        fn();
        atomic_store(once, 1);
    } else {
        while (atomic_load(once) == 2) sched_yield();
    }
    return 0;
}

/* bionic's pthread_key_t is an int; Darwin's is an unsigned long. */
static int b_key_create(int *key, void (*dtor)(void *))
{
    pthread_key_t k;
    int r = pthread_key_create(&k, dtor);
    if (r) return rc(r);
    *key = (int)k;
    return 0;
}
static int b_key_delete(int key) { return rc(pthread_key_delete((pthread_key_t)key)); }
static void *b_getspecific(int key) { return pthread_getspecific((pthread_key_t)key); }
static int b_setspecific(int key, const void *v) { return rc(pthread_setspecific((pthread_key_t)key, v)); }

/* ---------------------------------------------------------------- threads */

/* bionic's pthread_attr_t is 56 bytes on LP64: flags, stack_base, stack_size, guard_size, sched... */
typedef struct { uint32_t flags; uint32_t pad; void *stack_base; size_t stack_size; size_t guard_size; int32_t policy, priority; char reserved[16]; } guest_attr;
_Static_assert(sizeof(guest_attr) == 56, "bionic pthread_attr_t is 56 bytes on LP64");
_Static_assert(sizeof(guest_obj) <= 16, "guest_obj must fit bionic's smallest object, sem_t");
#define GATTR_DETACHED 1u
#define DEFAULT_STACK (1024u * 1024u)

static int b_attr_init(guest_attr *a)
{
    memset(a, 0, sizeof(*a));
    a->stack_size = DEFAULT_STACK;
    a->guard_size = 16384;
    return 0;
}
static int b_attr_destroy(guest_attr *a) { (void)a; return 0; }
static int b_attr_setdetachstate(guest_attr *a, int s)
{
    if (s != 0 && s != 1) return 22;
    a->flags = (a->flags & ~GATTR_DETACHED) | (s ? GATTR_DETACHED : 0);
    return 0;
}
/* upstream 3.0's attr getter: it reads the DETACHED bit back out of the guest's own flags word, so no host
 * pthread_attr_t is ever consulted (the size-mismatch hazard the whole attr family is shimmed for). */
static int b_attr_getdetachstate(const guest_attr *a, int *s)
{
    if (s) *s = (a->flags & GATTR_DETACHED) ? 1 : 0;
    return 0;
}
static int b_attr_setstacksize(guest_attr *a, size_t n)
{
    if (n < 16384) return 22;
    a->stack_size = n;
    return 0;
}
/* bionic's pthread_attr_setstack: the caller wants a thread with `size` bytes of stack placed at `addr`. A host thread
 * cannot be pinned to a guest address, so the size is honoured (exactly as pthread_attr_setstacksize) and the address
 * recorded but not used -- what the callers in practice want is the size: a big stack for a guest thread, which b_create
 * then gives it. EINVAL below bionic's minimum, as the real one answers. */
static int b_attr_setstack(guest_attr *a, void *addr, size_t n)
{
    if (n < 16384) return 22;
    a->stack_base = addr;
    a->stack_size = n;
    return 0;
}
static int b_attr_getstacksize(const guest_attr *a, size_t *n) { *n = a->stack_size; return 0; }
/* Scheduling attributes: a thread on this host runs at the system's own priority; the guest is told the default (SCHED_OTHER, priority 0) and its requests are accepted. */
static int b_attr_getschedparam(const guest_attr *a, int *prio) { (void)a; *prio = 0; return 0; }
static int b_attr_getschedpolicy(const guest_attr *a, int *policy) { (void)a; *policy = 0; return 0; }
/* the inherit-scheduling choice: kept, as nothing here schedules threads differently */
static int b_attr_setinheritsched(void *a, int v) { (void)a; return v == 0 || v == 1 ? 0 : 22; }
static int b_attr_getinheritsched(const void *a, int *v) { (void)a; if (v) *v = 0; return 0; }
static int b_attr_setschedpolicy(guest_attr *a, int policy) { (void)a; (void)policy; return 0; }
static int b_attr_getguardsize(const guest_attr *a, size_t *n) { *n = a->guard_size; return 0; }
static int b_attr_setguardsize(guest_attr *a, size_t n) { a->guard_size = n; return 0; }
static int b_attr_setscope(guest_attr *a, int scope) { (void)a; return scope == 0 ? 0 : 95; }    /* only PTHREAD_SCOPE_SYSTEM, as bionic */
static int b_attr_getscope(const guest_attr *a, int *scope) { (void)a; *scope = 0; return 0; }
static int b_attr_getstack(const guest_attr *a, void **base, size_t *size) { *base = a->stack_base; *size = a->stack_size; return 0; }

static int b_getattr_np(pthread_t t, guest_attr *a)
{
    b_attr_init(a);
    size_t size = pthread_get_stacksize_np(t);
    uint8_t *top = pthread_get_stackaddr_np(t);
    a->stack_size = size;
    a->stack_base = top - size;
    a->flags = 0;
    return 0;
}

typedef struct { void *(*fn)(void *); void *arg; } start_ctx;

const char *tl_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr);

static int g_thread_trace = -1;
/* A stack of its own for fault handlers, for the calling thread: a crash that leaves the thread's stack unusable (a guest's runaway
 * recursion) is then still reported -- macOS kills a thread it can not deliver a signal to with SIGILL, and nothing is said. */
void tl_thread_altstack(void)
{
    stack_t old;
    if (sigaltstack(NULL, &old) == 0 && !(old.ss_flags & SS_DISABLE)) return;
    stack_t ss = { .ss_sp = malloc(64 << 10), .ss_size = 64 << 10, .ss_flags = 0 };
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}
/*
 * How many guest threads the shim has started and that have not returned. There is no registry to walk -- a
 * guest pthread is pthread_create'd onto start_thunk and its own frames are free to run guest code -- so a
 * counter is the most a session close can honestly keep: it does not let us stop or suspend a guest thread, only
 * count the ones a close could not stop. It is for reporting, and that is the whole of its use.
 */
static atomic_int g_guest_threads;
int tl_guest_thread_live_count(void) { return atomic_load(&g_guest_threads); }

/*
 * WHO the live guest threads are, not only how many. The counter can say a close left two threads running and cannot
 * say which; a swapped-in app that then hangs on its first frame has nothing else to point at. This table records, per
 * live thread, its Darwin identity, the entry it was created on and the session EPOCH it started in, so a stall probe can
 * tell a thread from an EARLIER session (parked from the app that was swapped out) from this session's own, and name it.
 *
 * Lock-free on purpose: the probe reads it from the heartbeat thread and must never block on a mutex a parked thread
 * could be holding. A slot is reserved with a CAS to CLAIMING, filled, then published as PRESENT, and a reader includes a
 * slot only once PRESENT. Nothing here suspends, ends or joins a thread -- it is for reporting, as the counter is.
 */
enum { GT_FREE = 0, GT_CLAIMING = 1, GT_PRESENT = 2 };
#define TL_GT_SLOTS 128
static struct { _Atomic int live; _Atomic unsigned long tid, entry; _Atomic int epoch; _Atomic unsigned long wait_kind, wait_obj, wait_site; _Atomic unsigned long last_kind, last_ns; } g_gt[TL_GT_SLOTS];
static _Atomic int g_epoch;
int  tl_guest_thread_epoch(void) { return atomic_load(&g_epoch); }
void tl_guest_thread_new_epoch(void) { atomic_fetch_add(&g_epoch, 1); }

/* This thread's OWN census slot, cached in TLS by gt_enter, so the lock-wait marker is written from the thread itself with
 * no scan of the table (the mutex shim is hot). -1 for a thread that is not a guest thread (the layer's own threads),
 * which then skip the marker entirely. */
static __thread int g_my_slot = -1;

/* Monotonic nanoseconds, for the census: how long a thread has been inside the shim it is in (or since the last one it
 * entered). CLOCK_MONOTONIC on Darwin; the value is only ever subtracted from another reading of the same clock. */
static unsigned long gt_now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000000ul + (unsigned long)ts.tv_nsec;
}

/* See the forward declaration. Record WHY this thread is inside a lock, then clear the CURRENT marker when it leaves.
 * wait_obj/wait_site are stored BEFORE wait_kind is published (release), so a reader that acquire-loads a non-zero
 * wait_kind sees them valid. last_kind/last_ns PERSIST across the return (last_site is wait_site, which is not cleared):
 * they are the last shim this thread entered and when, so a stall can say what a thread that is not blocked right now was
 * last doing inside a shim. TL_GW_* is husk-tl-bionic.h's. Never blocks, never allocates. */
static void gt_wait_enter(int kind, const void *obj, void *lr)
{
    int s = g_my_slot;
    if (s < 0) return;
    unsigned long ns = gt_now_ns();
    atomic_store_explicit(&g_gt[s].wait_obj, (unsigned long)(uintptr_t)obj, memory_order_relaxed);
    atomic_store_explicit(&g_gt[s].wait_site, (unsigned long)(uintptr_t)lr, memory_order_relaxed);
    atomic_store_explicit(&g_gt[s].last_ns, ns, memory_order_relaxed);
    atomic_store_explicit(&g_gt[s].last_kind, (unsigned long)kind, memory_order_release);
    atomic_store_explicit(&g_gt[s].wait_kind, (unsigned long)kind, memory_order_release);
}
static void gt_wait_leave(void)
{
    int s = g_my_slot;
    if (s < 0) return;
    atomic_store_explicit(&g_gt[s].wait_kind, TL_GW_NONE, memory_order_release);
}

static void gt_enter(const void *entry)
{
    for (int i = 0; i < TL_GT_SLOTS; i++) {
        int expect = GT_FREE;
        if (!atomic_compare_exchange_strong(&g_gt[i].live, &expect, GT_CLAIMING)) continue;
        atomic_store(&g_gt[i].tid, (unsigned long)(uintptr_t)pthread_self());
        atomic_store(&g_gt[i].entry, (unsigned long)(uintptr_t)entry);
        atomic_store(&g_gt[i].epoch, atomic_load(&g_epoch));
        atomic_store_explicit(&g_gt[i].wait_kind, TL_GW_NONE, memory_order_relaxed);
        atomic_store_explicit(&g_gt[i].last_kind, TL_GW_NONE, memory_order_relaxed);
        atomic_store_explicit(&g_gt[i].last_ns, 0, memory_order_relaxed);
        g_my_slot = i;
        atomic_store_explicit(&g_gt[i].live, GT_PRESENT, memory_order_release);
        return;
    }
}
static void gt_leave(void)
{
    unsigned long me = (unsigned long)(uintptr_t)pthread_self();
    for (int i = 0; i < TL_GT_SLOTS; i++) {
        if (atomic_load_explicit(&g_gt[i].live, memory_order_acquire) != GT_PRESENT) continue;
        if (atomic_load(&g_gt[i].tid) == me) {
            atomic_store_explicit(&g_gt[i].wait_kind, TL_GW_NONE, memory_order_relaxed);
            atomic_store_explicit(&g_gt[i].live, GT_FREE, memory_order_release);
            g_my_slot = -1;
            return;
        }
    }
}
int tl_guest_thread_census(tl_guest_thread_info *out, int max)
{
    int n = 0;
    for (int i = 0; i < TL_GT_SLOTS; i++) {
        if (atomic_load_explicit(&g_gt[i].live, memory_order_acquire) != GT_PRESENT) continue;
        if (out && n < max) {
            out[n].tid   = atomic_load(&g_gt[i].tid);
            out[n].entry = atomic_load(&g_gt[i].entry);
            out[n].epoch = atomic_load(&g_gt[i].epoch);
            /* Pair the release in gt_wait_enter: read wait_kind first, then the object/site it names. */
            out[n].wait_kind = atomic_load_explicit(&g_gt[i].wait_kind, memory_order_acquire);
            out[n].wait_obj  = atomic_load(&g_gt[i].wait_obj);
            out[n].wait_site = atomic_load(&g_gt[i].wait_site);
            out[n].last_kind = atomic_load_explicit(&g_gt[i].last_kind, memory_order_acquire);
            out[n].last_site = out[n].wait_site;   /* not cleared on leave: it is the last shim's site */
            out[n].last_ns   = atomic_load(&g_gt[i].last_ns);
        }
        n++;
    }
    return n;
}

/*
 * Every guest mutex currently HELD that has a live waiter, with both sites -- read from the live-thread census at a
 * detected stall, so a stall names the app's own guest-lock deadlock from the recorded owner alone (there is no per-wait
 * "5 s" line any more; it would have required polling the wait, which is a permanent CPU cost on every contended lock). A
 * thread in TL_GW_MUTEX is blocked in b_mutex_lock on the
 * guest mutex in wait_obj; b_mutex_lock wrote the holder's thread and lock site into the mutex's spare bytes (w[2]/w[3],
 * note_owner). Deduped by the mutex object: one entry per held mutex, the first live waiter reported for it. holder_tid is
 * 0 when the holder was never recorded. Never blocks, never allocates; reads guest memory the same way the shim wrote it.
 */
int tl_guest_mutex_waits(tl_guest_mutex_wait *out, int max)
{
    int n = 0;
    for (int i = 0; i < TL_GT_SLOTS; i++) {
        if (atomic_load_explicit(&g_gt[i].live, memory_order_acquire) != GT_PRESENT) continue;
        if (atomic_load_explicit(&g_gt[i].wait_kind, memory_order_acquire) != TL_GW_MUTEX) continue;
        unsigned long obj = atomic_load(&g_gt[i].wait_obj);
        if (!obj) continue;
        int dup = 0;
        for (int k = 0; out && k < n && k < max; k++) if (out[k].mutex == obj) { dup = 1; break; }
        if (!dup && out && n < max) {
            const uint64_t *w = (const uint64_t *)(uintptr_t)obj;   /* the same 40-byte object the shim's note_owner wrote */
            out[n].mutex       = obj;
            out[n].waiter_tid  = atomic_load(&g_gt[i].tid);
            out[n].waiter_entry = atomic_load(&g_gt[i].entry);
            out[n].waiter_site = atomic_load(&g_gt[i].wait_site);
            out[n].holder_tid  = w[2];
            out[n].holder_site = w[3];
        }
        n++;
    }
    return n;
}

static void *start_thunk(void *p)
{
    start_ctx c = *(start_ctx *)p;
    free(p);
    atomic_fetch_add(&g_guest_threads, 1);
    gt_enter((const void *)c.fn);
    /* This guest thread may run Java through the interpreter (Unity's Java-side HandlerThreads, an app's own Threads do).
     * Register its Java frame stack ONCE, at entry -- so the cold stall probe can name its Java frames from outside
     * without the running path paying anything. Declared here so this file needs no DVM header. */
    void dvm_thread_register_frames(void); void dvm_thread_unregister_frames(void);
    dvm_thread_register_frames();

    /* a stack of its own for fault handlers: a crash that leaves the thread's stack unusable is then still reported (macOS kills a
       thread it can not deliver a signal to with SIGILL, and nothing is said) */
    tl_thread_altstack();
    if (g_thread_trace < 0) g_thread_trace = getenv("TL_THREAD_TRACE") ? 1 : 0;
    if (g_thread_trace) {
        const char *ln = NULL; const void *sa = NULL;
        const char *sym = tl_ld_symbol_at((const void *)c.fn, &ln, &sa);
        tl_log_line("thread: started, entry %s %s+%#lx", ln ? ln : "?", sym ? sym : "?", sa ? (unsigned long)((const char *)c.fn - (const char *)sa) : 0ul);
    }
    void *r = c.fn(c.arg);
    if (g_thread_trace) { char nm[32] = ""; pthread_getname_np(pthread_self(), nm, sizeof(nm)); tl_log_line("thread: '%s' EXITED", nm); }
    gt_leave();
    dvm_thread_unregister_frames();          /* leaving: its TLS is about to go, so forget the frame-stack address */
    atomic_fetch_sub(&g_guest_threads, 1);
    return r;
}

static int b_create(pthread_t *out, const guest_attr *attr, void *(*fn)(void *), void *arg)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    size_t stack = attr && attr->stack_size ? attr->stack_size : DEFAULT_STACK;
    /* at least 16 MB (address space; pages are only used as touched): a native thread that calls into Java runs the interpreter,
       whose calls take far more stack than ART's */
    if (stack < (16u << 20)) stack = 16u << 20;
    stack = (stack + 16383) & ~(size_t)16383;
    pthread_attr_setstacksize(&a, stack);
    if (attr && (attr->flags & GATTR_DETACHED)) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    start_ctx *c = malloc(sizeof(*c));
    c->fn = fn; c->arg = arg;
    pthread_t t;
    int r = pthread_create(&t, &a, start_thunk, c);
    pthread_attr_destroy(&a);
    if (r) { free(c); return rc(r); }
    *out = t;
    return 0;
}

static int b_kill(pthread_t t, int sig)
{
    if (getenv("TL_SIGNAL_TRACE")) { char nm[32] = "", tn[32] = ""; pthread_getname_np(pthread_self(), nm, sizeof(nm)); pthread_getname_np(t, tn, sizeof(tn)); tl_log_line("signal[%s]: pthread_kill(%s, %d)", nm, tn, sig); }
    int d = tl_signal_to_darwin(sig);
    if (d < 0) return 22;
    return rc(pthread_kill(t, d));
}

static int b_setname_np(pthread_t t, const char *name)
{
    if (pthread_equal(t, pthread_self())) pthread_setname_np(name);
    /* The engine's own threads name themselves ("UnityMain", "il2cpp", "Loading", a job worker...). One line
     * per distinct name is the cheapest proof that the engine's main thread was created at all: in the
     * zero-frames runs, nothing else says whether UnityMain ever started, and the crash runs only named it
     * after they had already faulted. */
    if (name && name[0]) {
        static char said[8][32]; static int nsaid;
        int known = 0;
        for (int i = 0; i < nsaid; i++) if (!strcmp(said[i], name)) { known = 1; break; }
        if (!known) {
            if (nsaid < 8) snprintf(said[nsaid++], sizeof(said[0]), "%s", name);
            tl_log_line("thread: a guest thread named itself '%s' (an engine thread entry)", name);
        }
    }
    return 0;
}

static int b_join(pthread_t t, void **ret) { return rc(pthread_join(t, ret)); }
static int b_detach(pthread_t t) { return rc(pthread_detach(t)); }

/* sigset_t is one 64-bit word on bionic, bit (sig-1); Darwin's is a 32-bit mask. */
static uint32_t sigset_to_darwin(uint64_t g)
{
    uint32_t d = 0;
    for (int s = 1; s < 32; s++) if (g & (1ull << (s - 1))) { int ds = tl_signal_to_darwin(s); if (ds > 0) d |= 1u << (ds - 1); }
    return d;
}
static uint64_t sigset_from_darwin(uint32_t d)
{
    uint64_t g = 0;
    for (int s = 1; s < 32; s++) if (d & (1u << (s - 1))) { int gs = tl_signal_from_darwin(s); if (gs > 0 && gs < 64) g |= 1ull << (gs - 1); }
    return g;
}
static int b_sigmask(int how, const uint64_t *set, uint64_t *old)
{
    sigset_t ds, od;
    int dh = how == 0 ? SIG_BLOCK : how == 1 ? SIG_UNBLOCK : SIG_SETMASK;
    if (set) { uint32_t m = sigset_to_darwin(*set); memcpy(&ds, &m, sizeof(m)); }
    int r = pthread_sigmask(dh, set ? &ds : NULL, old ? &od : NULL);
    if (old) { uint32_t m; memcpy(&m, &od, sizeof(m)); *old = sigset_from_darwin(m); }
    return rc(r);
}

const tl_bionic_entry tl_tab_pthread[] = {
    TL_WRAP("pthread_mutex_init", b_mutex_init), TL_WRAP("pthread_mutex_destroy", b_mutex_destroy),
    TL_WRAP("pthread_mutex_lock", b_mutex_lock), TL_WRAP("pthread_mutex_trylock", b_mutex_trylock),
    TL_WRAP("pthread_mutex_unlock", b_mutex_unlock),
    TL_WRAP("pthread_mutexattr_init", b_mutexattr_init), TL_WRAP("pthread_mutexattr_destroy", b_mutexattr_destroy),
    TL_WRAP("pthread_mutexattr_settype", b_mutexattr_settype), TL_WRAP("pthread_mutexattr_setpshared", b_mutexattr_setpshared), TL_WRAP("pthread_mutexattr_getpshared", b_mutexattr_getpshared), TL_WRAP("pthread_mutexattr_gettype", b_mutexattr_gettype),
    TL_WRAP("pthread_cond_init", b_cond_init), TL_WRAP("pthread_cond_destroy", b_cond_destroy),
    TL_WRAP("pthread_cond_signal", b_cond_signal), TL_WRAP("pthread_cond_broadcast", b_cond_broadcast),
    TL_WRAP("pthread_cond_wait", b_cond_wait), TL_WRAP("pthread_cond_timedwait", b_cond_timedwait), TL_WRAP("pthread_cond_clockwait", b_cond_clockwait),
    TL_WRAP("pthread_cond_timedwait_monotonic_np", b_cond_timedwait_monotonic), TL_WRAP("pthread_cond_timedwait_monotonic", b_cond_timedwait_monotonic),
    TL_WRAP("pthread_condattr_init", b_condattr_init), TL_WRAP("pthread_condattr_destroy", b_condattr_destroy),
    TL_WRAP("pthread_condattr_setclock", b_condattr_setclock), TL_WRAP("pthread_condattr_setpshared", b_condattr_setpshared), TL_WRAP("pthread_condattr_getpshared", b_condattr_getpshared),
    TL_WRAP("pthread_rwlock_init", b_rwlock_init), TL_WRAP("pthread_rwlock_destroy", b_rwlock_destroy),
    TL_WRAP("pthread_rwlock_rdlock", b_rwlock_rdlock), TL_WRAP("pthread_rwlock_wrlock", b_rwlock_wrlock),
    TL_WRAP("pthread_rwlock_tryrdlock", b_rwlock_tryrdlock), TL_WRAP("pthread_rwlock_trywrlock", b_rwlock_trywrlock),
    TL_WRAP("pthread_rwlock_unlock", b_rwlock_unlock),
    TL_WRAP("sem_init", b_sem_init), TL_WRAP("sem_destroy", b_sem_destroy), TL_WRAP("sem_post", b_sem_post),
    TL_WRAP("sem_wait", b_sem_wait), TL_WRAP("sem_trywait", b_sem_trywait), TL_WRAP("sem_timedwait", b_sem_timedwait),
    TL_WRAP("sem_getvalue", b_sem_getvalue),
    TL_WRAP("pthread_once", b_once),
    TL_WRAP("__tls_get_addr", b___tls_get_addr),
    TL_WRAP("pthread_key_create", b_key_create), TL_WRAP("pthread_key_delete", b_key_delete),
    TL_WRAP("pthread_getspecific", b_getspecific), TL_WRAP("pthread_setspecific", b_setspecific),
    TL_WRAP("pthread_attr_init", b_attr_init), TL_WRAP("pthread_attr_destroy", b_attr_destroy),
    TL_WRAP("pthread_attr_setdetachstate", b_attr_setdetachstate), TL_WRAP("pthread_attr_getdetachstate", b_attr_getdetachstate), TL_WRAP("pthread_attr_setstacksize", b_attr_setstacksize),
    TL_WRAP("pthread_attr_setstack", b_attr_setstack),
    TL_WRAP("pthread_attr_getstacksize", b_attr_getstacksize), TL_WRAP("pthread_attr_getguardsize", b_attr_getguardsize), TL_WRAP("pthread_attr_setguardsize", b_attr_setguardsize), TL_WRAP("pthread_attr_setscope", b_attr_setscope), TL_WRAP("pthread_attr_getscope", b_attr_getscope), TL_WRAP("pthread_attr_getstack", b_attr_getstack),
    TL_WRAP("pthread_attr_getschedparam", b_attr_getschedparam),
    TL_WRAP("pthread_attr_getschedpolicy", b_attr_getschedpolicy), TL_WRAP("pthread_attr_setinheritsched", b_attr_setinheritsched), TL_WRAP("pthread_attr_getinheritsched", b_attr_getinheritsched), TL_WRAP("pthread_attr_setschedpolicy", b_attr_setschedpolicy),
    TL_WRAP("pthread_getattr_np", b_getattr_np),
    TL_WRAP("pthread_create", b_create), TL_WRAP("pthread_join", b_join), TL_WRAP("pthread_detach", b_detach),
    TL_DIRECT(pthread_exit), TL_DIRECT(pthread_self), TL_DIRECT(pthread_equal),
    TL_WRAP("pthread_kill", b_kill), TL_WRAP("pthread_setname_np", b_setname_np),
    TL_WRAP("pthread_sigmask", b_sigmask), TL_WRAP("pthread_sigmask64", b_sigmask),
    TL_END
};
