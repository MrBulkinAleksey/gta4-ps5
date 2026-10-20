/* [ps5 port] Emulated TLS for the PS5 link, with a fast path.
 *
 * The same emutls and C++ thread_local destructor support that
 * ps5-plume-triangle's tools/radv-link.sh writes as ps5-emutls-cxa.c (from
 * LLVM compiler-rt's emutls.c, Apache-2.0 WITH LLVM-exception), compiled over
 * it by ps5/tools/ps5_package.sh. What's added: every thread_local access is a
 * call to __emutls_get_address, and finding the thread's block took a
 * pthread_getspecific into libkernel each time; the game's main thread spent
 * ~8% of its time there (the guest's interlocked operations and critical
 * sections touch thread_locals on every call). Now a thread's block is found
 * by its thread control block's address (%fs:0, the x86-64 thread pointer) in
 * a small table, with no call; pthread_getspecific is only the slow path.
 *
 * The PS5 hands a finished thread's control block to the next new thread, so
 * a slot must never outlive its thread. It is filled in from the slow path,
 * emptied before the block is resized, and taken out of the table for good
 * when the thread starts ending: at the first of its thread-specific data
 * destructor calls (0.6b2 waited for the third, which the PS5 doesn't seem to
 * reach, and a new thread then used a dead thread's destroyed thread_locals),
 * and earlier still for the game's threads (ps5_emutls_thread_exit). A rex
 * thread also clears any slot under its control block when it starts
 * (ps5_emutls_thread_start), and the slow path counts any slot it finds naming
 * another block (ps5_emutls_stale_count, in the log), fixing it.
 * ps5_emutls_set_fast(0) turns the table off (the hub's Debug page).
 */
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Delay emutls deallocation and drain C++ thread_local destructors first
 * while emutls storage is still live on the exiting thread. */
#define EMUTLS_SKIP_DESTRUCTOR_ROUNDS 2

struct thread_destructor {
    void (*destructor)(void *);
    void *object;
    struct thread_destructor *next;
};

static pthread_key_t destructors_key;
static pthread_once_t destructors_once = PTHREAD_ONCE_INIT;
static bool destructors_ready;

static void
run_destructors(void *list)
{
    struct thread_destructor *entry = (struct thread_destructor *)list;
    while (entry) {
        struct thread_destructor *const next = entry->next;
        entry->destructor(entry->object);
        free(entry);
        struct thread_destructor *const added =
            (struct thread_destructor *)pthread_getspecific(destructors_key);
        if (added) {
            pthread_setspecific(destructors_key, NULL);
            run_destructors(added);
        }
        entry = next;
    }
}

static void
drain_thread_destructors(void)
{
    if (!destructors_ready)
        return;
    struct thread_destructor *const list =
        (struct thread_destructor *)pthread_getspecific(destructors_key);
    if (list) {
        pthread_setspecific(destructors_key, NULL);
        run_destructors(list);
    }
}

static void
setup_destructors(void)
{
    destructors_ready =
        pthread_key_create(&destructors_key, run_destructors) == 0 &&
        atexit(drain_thread_destructors) == 0;
}

__attribute__((visibility("hidden"))) int
ps5___cxa_thread_atexit_impl(void (*destructor)(void *), void *object, void *dso)
{
    (void)dso;
    pthread_once(&destructors_once, setup_destructors);
    if (!destructors_ready)
        return -1;
    struct thread_destructor *const entry =
        (struct thread_destructor *)malloc(sizeof(*entry));
    if (!entry)
        return -1;
    entry->destructor = destructor;
    entry->object = object;
    entry->next = (struct thread_destructor *)pthread_getspecific(destructors_key);
    if (pthread_setspecific(destructors_key, entry) != 0) {
        free(entry);
        return -1;
    }
    return 0;
}

typedef struct emutls_address_array {
    uintptr_t skip_destructor_rounds;
    uintptr_t fast;  /* may be published in the fast table */
    uintptr_t owner; /* the thread control block it was published for */
    uintptr_t size;
    void *data[];
} emutls_address_array;

typedef unsigned int gcc_word __attribute__((mode(word)));
typedef unsigned int gcc_pointer __attribute__((mode(pointer)));

typedef struct __emutls_control {
    gcc_word size;
    gcc_word align;
    union {
        uintptr_t index;
        void *address;
    } object;
    void *value;
} __emutls_control;

static pthread_mutex_t emutls_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t emutls_pthread_key;
static size_t emutls_num_object = 0;

/* The thread's emutls key once its block is freed at exit: 1, then one more
 * each destructor round, up to 4 (as many rounds as libthr runs at most, but
 * bounded here either way). A block made meanwhile (a later destructor
 * touching a thread_local) is made unpublished. */
#define EMUTLS_EXITED_ROUNDS 4u
static __inline bool
emutls_exited(const void *value)
{
    return (uintptr_t)value - 1 < EMUTLS_EXITED_ROUNDS;
}

/* ---- the fast table: thread control block address -> the thread's block ---- */

#define FAST_SLOTS 1024u /* a power of two */
#define FAST_PROBE 8u

struct fast_slot {
    uintptr_t tcb;
    emutls_address_array *array;
};
static struct fast_slot fast_slots[FAST_SLOTS] __attribute__((aligned(64)));
static int fast_enabled = 1;
static unsigned long fast_stale_count = 0;
/* Destructor calls on a thread's block, by round (first, second, third: the
 * one that frees it). */
static unsigned long exit_rounds[3] = {0, 0, 0};

static __inline uintptr_t
current_tcb(void)
{
    uintptr_t tcb;
    __asm__("movq %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

static __inline uint32_t
fast_hash(uintptr_t tcb)
{
    return (uint32_t)(((uint64_t)tcb * 0x9E3779B97F4A7C15ull) >> 54) & (FAST_SLOTS - 1);
}

/* Empties every slot of the calling thread (there is normally one). */
static void
fast_unpublish(void)
{
    const uintptr_t tcb = current_tcb();
    uint32_t i = fast_hash(tcb);
    for (uint32_t n = 0; n < FAST_PROBE; ++n, i = (i + 1) & (FAST_SLOTS - 1)) {
        if (__atomic_load_n(&fast_slots[i].tcb, __ATOMIC_RELAXED) == tcb) {
            __atomic_store_n(&fast_slots[i].array, (emutls_address_array *)NULL, __ATOMIC_RELAXED);
            __atomic_store_n(&fast_slots[i].tcb, (uintptr_t)0, __ATOMIC_RELEASE);
        }
    }
}

/* Points the calling thread's slot at its block, taking a free slot first if
 * it has none. A thread whose %fs:0 isn't a thread pointer (it always is on
 * x86-64: the word at the thread pointer is its own address) or that finds no
 * free slot near its hash stays on the slow path. */
static void
fast_publish(emutls_address_array *array)
{
    if (!array->fast || !__atomic_load_n(&fast_enabled, __ATOMIC_RELAXED))
        return;
    const uintptr_t tcb = current_tcb();
    if (!tcb || *(const uintptr_t *)tcb != tcb) {
        array->fast = 0;
        return;
    }
    array->owner = tcb;
    const uint32_t first = fast_hash(tcb);
    uint32_t i = first;
    for (uint32_t n = 0; n < FAST_PROBE; ++n, i = (i + 1) & (FAST_SLOTS - 1)) {
        if (__atomic_load_n(&fast_slots[i].tcb, __ATOMIC_ACQUIRE) == tcb) {
            __atomic_store_n(&fast_slots[i].array, array, __ATOMIC_RELAXED);
            return;
        }
    }
    i = first;
    for (uint32_t n = 0; n < FAST_PROBE; ++n, i = (i + 1) & (FAST_SLOTS - 1)) {
        uintptr_t owner = __atomic_load_n(&fast_slots[i].tcb, __ATOMIC_ACQUIRE);
        if (owner == 0 &&
            !__atomic_compare_exchange_n(&fast_slots[i].tcb, &owner, tcb, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
            owner != tcb)
            continue;
        if (owner != 0 && owner != tcb)
            continue;
        __atomic_store_n(&fast_slots[i].array, array, __ATOMIC_RELAXED);
        /* A signal handler on this thread may have taken another slot for it
         * in between (the profiler's reads a thread_local): keep just one. */
        uint32_t j = first;
        for (uint32_t m = 0; m < FAST_PROBE; ++m, j = (j + 1) & (FAST_SLOTS - 1)) {
            if (j != i && __atomic_load_n(&fast_slots[j].tcb, __ATOMIC_RELAXED) == tcb) {
                __atomic_store_n(&fast_slots[j].array, (emutls_address_array *)NULL,
                                 __ATOMIC_RELAXED);
                __atomic_store_n(&fast_slots[j].tcb, (uintptr_t)0, __ATOMIC_RELEASE);
            }
        }
        return;
    }
    array->fast = 0;
}

/* ---- emutls ---- */

static __inline void *
emutls_memalign_alloc(size_t align, size_t size)
{
    if (align < sizeof(void *))
        align = sizeof(void *);
    const size_t extra = align - 1 + sizeof(void *);
    char *const object = (char *)malloc(extra + size);
    if (!object)
        abort();
    void *const base =
        (void *)(((uintptr_t)(object + extra)) & ~(uintptr_t)(align - 1));
    ((void **)base)[-1] = object;
    return base;
}

static __inline void
emutls_memalign_free(void *base)
{
    free(((void **)base)[-1]);
}

static __inline void
emutls_setspecific(emutls_address_array *value)
{
    pthread_setspecific(emutls_pthread_key, (void *)value);
}

static __inline emutls_address_array *
emutls_getspecific(void)
{
    return (emutls_address_array *)pthread_getspecific(emutls_pthread_key);
}

static void
emutls_shutdown(emutls_address_array *array)
{
    if (array) {
        for (uintptr_t i = 0; i < array->size; ++i) {
            if (array->data[i]) {
                void *const ptr = array->data[i];
                array->data[i] = NULL;
                emutls_memalign_free(ptr);
            }
        }
    }
}

static void
emutls_key_destructor(void *ptr)
{
    if (emutls_exited(ptr)) {
        /* Kept through the next rounds, so a block made by a later destructor
         * is still made unpublished. */
        if ((uintptr_t)ptr < EMUTLS_EXITED_ROUNDS)
            emutls_setspecific((emutls_address_array *)((uintptr_t)ptr + 1));
        return;
    }
    emutls_address_array *array = (emutls_address_array *)ptr;
    {
        /* The thread is ending: no signal handler runs on it from here (one
         * touching a thread_local when the key reads NULL would publish a new
         * block), and its block leaves the table now, whatever number of
         * destructor rounds the system goes on to run. Its thread_locals are
         * reached through the key until the block is freed. */
        sigset_t all;
        sigfillset(&all);
        pthread_sigmask(SIG_BLOCK, &all, NULL);
        array->fast = 0;
        fast_unpublish();
        /* (how many rounds the system runs: the log reports these) */
        __atomic_fetch_add(&exit_rounds[array->skip_destructor_rounds >= EMUTLS_SKIP_DESTRUCTOR_ROUNDS
                                            ? 0
                                            : array->skip_destructor_rounds ? 1 : 2],
                           1, __ATOMIC_RELAXED);
    }
    if (destructors_ready && pthread_getspecific(destructors_key) != NULL) {
        emutls_setspecific(array);
        drain_thread_destructors();
        /* A destructor may have grown (moved) the block. */
        array = emutls_getspecific();
        ptr = array;
        array->fast = 0;
        fast_unpublish();
    }
    if (array->skip_destructor_rounds > 0) {
        array->skip_destructor_rounds--;
        emutls_setspecific(array);
    } else {
        /* From here on no signal handler runs on this exiting thread: one
         * touching a thread_local after the block is gone (or after libthr's
         * last round, when the key reads NULL again) would publish a block
         * for this thread control block that nothing unpublishes. */
        sigset_t all;
        sigfillset(&all);
        pthread_sigmask(SIG_BLOCK, &all, NULL);
        array->fast = 0;
        fast_unpublish(); /* (also a block a handler published meanwhile) */
        emutls_setspecific((emutls_address_array *)(uintptr_t)1);
        emutls_shutdown(array);
        free(ptr);
    }
}

static int emutls_pthread_key_ready = 0;

static void
emutls_init(void)
{
    if (pthread_key_create(&emutls_pthread_key, emutls_key_destructor) != 0)
        abort();
    __atomic_store_n(&emutls_pthread_key_ready, 1, __ATOMIC_RELEASE);
}

static __inline void
emutls_init_once(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, emutls_init);
}

static __inline void *
emutls_allocate_object(__emutls_control *control)
{
    size_t size = control->size;
    size_t align = control->align;
    if (align < sizeof(void *))
        align = sizeof(void *);
    if ((align & (align - 1)) != 0)
        abort();
    void *base = emutls_memalign_alloc(align, size);
    if (control->value)
        memcpy(base, control->value, size);
    else
        memset(base, 0, size);
    return base;
}

static __inline uintptr_t
emutls_get_index(__emutls_control *control)
{
    uintptr_t index = __atomic_load_n(&control->object.index, __ATOMIC_ACQUIRE);
    if (!index) {
        emutls_init_once();
        pthread_mutex_lock(&emutls_mutex);
        index = control->object.index;
        if (!index) {
            index = ++emutls_num_object;
            __atomic_store_n(&control->object.index, index, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&emutls_mutex);
    }
    return index;
}

static __inline void
emutls_check_array_set_size(emutls_address_array *array, uintptr_t size)
{
    if (array == NULL)
        abort();
    array->size = size;
    emutls_setspecific(array);
}

static __inline uintptr_t
emutls_new_data_array_size(uintptr_t index)
{
    uintptr_t header_words = sizeof(emutls_address_array) / sizeof(void *);
    return ((index + header_words + 15) & ~((uintptr_t)15)) - header_words;
}

static __inline uintptr_t
emutls_asize(uintptr_t N)
{
    return N * sizeof(void *) + sizeof(emutls_address_array);
}

static __inline emutls_address_array *
emutls_get_address_array(uintptr_t index)
{
    emutls_address_array *array = emutls_getspecific();
    if (array == NULL || emutls_exited(array)) {
        const bool exited = array != NULL;
        uintptr_t new_size = emutls_new_data_array_size(index);
        array = (emutls_address_array *)malloc(emutls_asize(new_size));
        if (array) {
            memset(array->data, 0, new_size * sizeof(void *));
            /* After the thread's block was freed: free this one at the next
             * round, and never publish it. */
            array->skip_destructor_rounds = exited ? 0 : EMUTLS_SKIP_DESTRUCTOR_ROUNDS;
            array->fast = !exited;
            array->owner = 0;
        }
        emutls_check_array_set_size(array, new_size);
    } else if (index > array->size) {
        uintptr_t orig_size = array->size;
        uintptr_t new_size = emutls_new_data_array_size(index);
        /* Out of the table while it moves (a signal handler on this thread
         * takes the slow path meanwhile). */
        /* No handler on this thread between taking the block out of the
         * table and the key naming the moved one. */
        sigset_t all, old;
        sigfillset(&all);
        pthread_sigmask(SIG_BLOCK, &all, &old);
        if (array->fast)
            fast_unpublish();
        array = (emutls_address_array *)realloc(array, emutls_asize(new_size));
        if (array)
            memset(array->data + orig_size, 0,
                   (new_size - orig_size) * sizeof(void *));
        emutls_check_array_set_size(array, new_size);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
    }
    return array;
}

__attribute__((noinline)) static void *
emutls_get_address_slow(__emutls_control *control)
{
    uintptr_t index = emutls_get_index(control);
    emutls_address_array *array = emutls_get_address_array(index--);
    if (array->data[index] == NULL)
        array->data[index] = emutls_allocate_object(control);
    void *const object = array->data[index];
    /* A slot under this thread's control block naming another block: left by
     * a thread that ended without leaving the table. Counted (for the log);
     * the publish below points it at this thread's own block. */
    {
        const uintptr_t tcb = current_tcb();
        uint32_t i = fast_hash(tcb);
        for (uint32_t n = 0; n < FAST_PROBE; ++n, i = (i + 1) & (FAST_SLOTS - 1)) {
            if (__atomic_load_n(&fast_slots[i].tcb, __ATOMIC_RELAXED) != tcb)
                continue;
            emutls_address_array *const named =
                __atomic_load_n(&fast_slots[i].array, __ATOMIC_RELAXED);
            if (named && named != array) {
                __atomic_fetch_add(&fast_stale_count, 1, __ATOMIC_RELAXED);
                if (!array->fast) {
                    __atomic_store_n(&fast_slots[i].array, (emutls_address_array *)NULL,
                                     __ATOMIC_RELAXED);
                    __atomic_store_n(&fast_slots[i].tcb, (uintptr_t)0, __ATOMIC_RELEASE);
                }
            }
        }
    }
    fast_publish(array);
    return object;
}

/* A thread starting (before its first thread_local): any slot under its
 * control block is a finished thread's. */
__attribute__((visibility("hidden"))) void
ps5_emutls_thread_start(void)
{
    const uintptr_t tcb = current_tcb();
    uint32_t i = fast_hash(tcb);
    for (uint32_t n = 0; n < FAST_PROBE; ++n, i = (i + 1) & (FAST_SLOTS - 1)) {
        if (__atomic_load_n(&fast_slots[i].tcb, __ATOMIC_RELAXED) == tcb) {
            __atomic_fetch_add(&fast_stale_count, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&fast_slots[i].array, (emutls_address_array *)NULL, __ATOMIC_RELAXED);
            __atomic_store_n(&fast_slots[i].tcb, (uintptr_t)0, __ATOMIC_RELEASE);
        }
    }
}

/* A thread finishing (the game's threads, from rex's thread exit): out of the
 * table; its thread_locals are reached through the key from here on. */
__attribute__((visibility("hidden"))) void
ps5_emutls_thread_exit(void)
{
    if (__atomic_load_n(&emutls_pthread_key_ready, __ATOMIC_ACQUIRE)) {
        emutls_address_array *const array = emutls_getspecific();
        if (array && !emutls_exited(array))
            array->fast = 0;
    }
    fast_unpublish();
}

/* The hub's Fast Thread Lookups off: the table empties, every lookup takes
 * the slow path. Set at start, before the game's threads run. */
__attribute__((visibility("hidden"))) void
ps5_emutls_set_fast(int on)
{
    __atomic_store_n(&fast_enabled, on ? 1 : 0, __ATOMIC_RELEASE);
    if (on)
        return;
    for (uint32_t i = 0; i < FAST_SLOTS; ++i) {
        __atomic_store_n(&fast_slots[i].array, (emutls_address_array *)NULL, __ATOMIC_RELAXED);
        __atomic_store_n(&fast_slots[i].tcb, (uintptr_t)0, __ATOMIC_RELEASE);
    }
}

__attribute__((visibility("hidden"))) unsigned long
ps5_emutls_stale_count(void)
{
    return __atomic_load_n(&fast_stale_count, __ATOMIC_RELAXED);
}

/* Thread exits seen by round: [0] first destructor call, [1] second, [2] the
 * third (the block freed). */
__attribute__((visibility("hidden"))) void
ps5_emutls_exit_rounds(unsigned long out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = __atomic_load_n(&exit_rounds[i], __ATOMIC_RELAXED);
}

__attribute__((visibility("hidden"))) void *
__emutls_get_address(__emutls_control *control)
{
    const uintptr_t index = __atomic_load_n(&control->object.index, __ATOMIC_ACQUIRE);
    if (__builtin_expect(index != 0, 1)) {
        const uintptr_t tcb = current_tcb();
        uint32_t i = fast_hash(tcb);
        for (uint32_t n = 0; n < FAST_PROBE; ++n, i = (i + 1) & (FAST_SLOTS - 1)) {
            if (__atomic_load_n(&fast_slots[i].tcb, __ATOMIC_RELAXED) == tcb) {
                emutls_address_array *const array =
                    __atomic_load_n(&fast_slots[i].array, __ATOMIC_RELAXED);
                if (array && array->owner == tcb && index <= array->size) {
                    void *const object = array->data[index - 1];
                    if (__builtin_expect(object != NULL, 1))
                        return object;
                }
                break;
            }
        }
    }
    return emutls_get_address_slow(control);
}
