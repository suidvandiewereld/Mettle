#include "runtime/parallel.h"
#include <stddef.h>
#include <stdint.h>

extern char *getenv(const char *name);

#define MTP_CACHE_LINE 64u
#define MTP_VECTOR_BYTES 16u
#define MTP_CHUNKS_PER_THREAD 4u
#define MTP_SPIN_ROUNDS 4096u
#define MTP_YIELD_ROUNDS 16384u
#define MTP_WAKE_FANOUT 2u
#define MTP_MEMORY_CHUNK_BYTES 262144ll
#define MTP_STREAM_THRESHOLD_BYTES (8ll << 20)
#define MTP_TICKET_NEXT_SHIFT 32u

typedef struct {
  MettleParallelBody body;
  MettleParallelSlotBody slot_body;
  void *ctx;
  long long lo;
  long long hi;
  long long span;
} MtpJob;

typedef struct {
  uint64_t completed __attribute__((aligned(MTP_CACHE_LINE)));
} MtpSlot;

typedef struct {
  MtpJob job __attribute__((aligned(MTP_CACHE_LINE)));
  uint64_t ticket __attribute__((aligned(MTP_CACHE_LINE)));
  uint32_t generation __attribute__((aligned(MTP_CACHE_LINE)));
  int32_t parked __attribute__((aligned(MTP_CACHE_LINE)));
  int32_t wake_budget __attribute__((aligned(MTP_CACHE_LINE)));
  int32_t busy __attribute__((aligned(MTP_CACHE_LINE)));
  unsigned threads;
  unsigned workers;
  int spawned;
  uint64_t dispatched;
} MtpPool;

static MtpPool g_pool;
static MtpSlot g_slots[METTLE_PARALLEL_MAX_THREADS];

static void mtp_worker(unsigned index);

static void mtp_cpu_pause(void) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

#if defined(_WIN32) || defined(_WIN64)

typedef struct {
  unsigned long dwOemId;
  unsigned long dwPageSize;
  void *lpMinimumApplicationAddress;
  void *lpMaximumApplicationAddress;
  unsigned long long dwActiveProcessorMask;
  unsigned long dwNumberOfProcessors;
  unsigned long dwProcessorType;
  unsigned long dwAllocationGranularity;
  unsigned short wProcessorLevel;
  unsigned short wProcessorRevision;
} MtpSystemInfo;

__declspec(dllimport) void __stdcall GetSystemInfo(MtpSystemInfo *info);
__declspec(dllimport) void *__stdcall CreateThread(
    void *attributes, size_t stack_size,
    unsigned long(__stdcall *start)(void *), void *argument,
    unsigned long flags, unsigned long *thread_id);
__declspec(dllimport) int __stdcall CloseHandle(void *handle);
__declspec(dllimport) int __stdcall SwitchToThread(void);
__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);
__declspec(dllimport) void *__stdcall CreateSemaphoreW(
    void *attributes, long initial, long maximum, const unsigned short *name);
__declspec(dllimport) int __stdcall ReleaseSemaphore(void *semaphore,
                                                     long count,
                                                     long *previous);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(
    void *handle, unsigned long milliseconds);

static void *g_semaphore;

static unsigned long __stdcall mtp_thread_entry(void *argument) {
  mtp_worker((unsigned)(size_t)argument);
  return 0;
}

static unsigned mtp_os_hardware_threads(void) {
  MtpSystemInfo info;
  info.dwNumberOfProcessors = 0;
  GetSystemInfo(&info);
  return info.dwNumberOfProcessors ? (unsigned)info.dwNumberOfProcessors : 1u;
}

static void mtp_os_init(void) {
  g_semaphore = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
}

static int mtp_os_spawn(unsigned index) {
  void *handle =
      CreateThread(NULL, 0, mtp_thread_entry, (void *)(size_t)index, 0, NULL);
  if (!handle) {
    return 0;
  }
  (void)CloseHandle(handle);
  return 1;
}

static void mtp_os_yield(void) { (void)SwitchToThread(); }

static void mtp_os_wait(uint32_t *word, uint32_t seen) {
  if (!g_semaphore) {
    Sleep(1);
    return;
  }
  if (__atomic_load_n(word, __ATOMIC_SEQ_CST) == seen) {
    (void)WaitForSingleObject(g_semaphore, 0xffffffffu);
  }
}

static void mtp_os_wake(uint32_t *word, unsigned count) {
  (void)word;
  if (g_semaphore) {
    (void)ReleaseSemaphore(g_semaphore, (long)count, NULL);
  }
}

#elif defined(MT_HOSTED)

#include "runtime/hosted_posix.h"

#define MTP_THREAD_STACK_BYTES (1024u * 1024u)

static void *mtp_thread_entry(void *argument) {
  mtp_worker((unsigned)(size_t)argument);
  return NULL;
}

static unsigned mtp_os_hardware_threads(void) {
  return mt_hosted_hardware_threads();
}

static void mtp_os_init(void) {}

static int mtp_os_spawn(unsigned index) {
  return mt_hosted_spawn_detached(mtp_thread_entry, (void *)(size_t)index,
                                  MTP_THREAD_STACK_BYTES);
}

static void mtp_os_yield(void) { mt_hosted_yield(); }

static void mtp_os_wait(uint32_t *word, uint32_t seen) {
  (void)mt_hosted_wait_on((volatile int *)word, (int)seen,
                          MT_HOSTED_INFINITE);
}

static void mtp_os_wake(uint32_t *word, unsigned count) {
  (void)count;
  mt_hosted_wake((volatile int *)word);
}

#else

#if defined(__x86_64__)
#define MTP_SYS_SCHED_YIELD 24L
#define MTP_SYS_FUTEX 202L
#define MTP_SYS_SCHED_GETAFFINITY 204L
#elif defined(__aarch64__)
#define MTP_SYS_SCHED_YIELD 124L
#define MTP_SYS_FUTEX 98L
#define MTP_SYS_SCHED_GETAFFINITY 123L
#endif

#define MTP_FUTEX_WAIT_PRIVATE 128L
#define MTP_FUTEX_WAKE_PRIVATE 129L

extern int pthread_create(unsigned long *thread, const void *attributes,
                          void *(*start)(void *), void *argument);
extern int pthread_detach(unsigned long thread);

static long mtp_syscall(long number, long first, long second, long third,
                        long fourth) {
#if defined(__x86_64__)
  long result;
  register long r10 __asm__("r10") = fourth;
  __asm__ __volatile__("syscall"
                       : "=a"(result)
                       : "a"(number), "D"(first), "S"(second), "d"(third),
                         "r"(r10)
                       : "rcx", "r11", "memory");
  return result;
#elif defined(__aarch64__)
  register long x8 __asm__("x8") = number;
  register long x0 __asm__("x0") = first;
  register long x1 __asm__("x1") = second;
  register long x2 __asm__("x2") = third;
  register long x3 __asm__("x3") = fourth;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                       : "memory", "cc");
  return x0;
#else
  (void)number;
  (void)first;
  (void)second;
  (void)third;
  (void)fourth;
  return -1;
#endif
}

static void *mtp_thread_entry(void *argument) {
  mtp_worker((unsigned)(size_t)argument);
  return NULL;
}

static unsigned mtp_os_hardware_threads(void) {
#if defined(MTP_SYS_SCHED_GETAFFINITY)
  unsigned long long mask[16] = {0};
  long bytes = mtp_syscall(MTP_SYS_SCHED_GETAFFINITY, 0L, (long)sizeof(mask),
                           (long)(size_t)mask, 0L);
  unsigned count = 0;
  for (long i = 0; i < bytes / 8 && i < 16; i++) {
    count += (unsigned)__builtin_popcountll(mask[i]);
  }
  return count ? count : 1u;
#else
  return 1u;
#endif
}

static void mtp_os_init(void) {}

static int mtp_os_spawn(unsigned index) {
  unsigned long thread = 0;
  if (pthread_create(&thread, NULL, mtp_thread_entry, (void *)(size_t)index)) {
    return 0;
  }
  (void)pthread_detach(thread);
  return 1;
}

static void mtp_os_yield(void) {
#if defined(MTP_SYS_SCHED_YIELD)
  (void)mtp_syscall(MTP_SYS_SCHED_YIELD, 0L, 0L, 0L, 0L);
#else
  mtp_cpu_pause();
#endif
}

static void mtp_os_wait(uint32_t *word, uint32_t seen) {
#if defined(MTP_SYS_FUTEX)
  (void)mtp_syscall(MTP_SYS_FUTEX, (long)(size_t)word, MTP_FUTEX_WAIT_PRIVATE,
                    (long)seen, 0L);
#else
  (void)word;
  (void)seen;
  mtp_os_yield();
#endif
}

static void mtp_os_wake(uint32_t *word, unsigned count) {
#if defined(MTP_SYS_FUTEX)
  (void)mtp_syscall(MTP_SYS_FUTEX, (long)(size_t)word, MTP_FUTEX_WAKE_PRIVATE,
                    (long)count, 0L);
#else
  (void)word;
  (void)count;
#endif
}

#endif

static long mtp_parse_count(const char *text) {
  long value = 0;
  if (!text) {
    return 0;
  }
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  while (*text >= '0' && *text <= '9') {
    value = value * 10 + (*text - '0');
    if (value > (long)METTLE_PARALLEL_MAX_THREADS) {
      return (long)METTLE_PARALLEL_MAX_THREADS;
    }
    text++;
  }
  return value;
}

static unsigned mtp_threads(void) {
  unsigned threads = __atomic_load_n(&g_pool.threads, __ATOMIC_RELAXED);
  if (!threads) {
    long requested = mtp_parse_count(getenv("METTLE_PARALLEL_THREADS"));
    if (requested < 1) {
      requested = (long)mtp_os_hardware_threads();
    }
    if (requested > (long)METTLE_PARALLEL_MAX_THREADS) {
      requested = (long)METTLE_PARALLEL_MAX_THREADS;
    }
    threads = (unsigned)requested;
    __atomic_store_n(&g_pool.threads, threads, __ATOMIC_RELAXED);
  }
  return threads;
}

static unsigned mtp_ticket_count(uint64_t ticket) {
  return (unsigned)ticket;
}

static unsigned mtp_ticket_next(uint64_t ticket) {
  return (unsigned)(ticket >> MTP_TICKET_NEXT_SHIFT);
}

static int mtp_ticket_exhausted(uint64_t ticket) {
  return mtp_ticket_next(ticket) >= mtp_ticket_count(ticket);
}

static int mtp_claim(unsigned *chunk) {
  uint64_t ticket = __atomic_load_n(&g_pool.ticket, __ATOMIC_ACQUIRE);
  if (mtp_ticket_exhausted(ticket)) {
    return 0;
  }
  ticket = __atomic_fetch_add(&g_pool.ticket, 1ull << MTP_TICKET_NEXT_SHIFT,
                              __ATOMIC_ACQ_REL);
  if (mtp_ticket_exhausted(ticket)) {
    return 0;
  }
  *chunk = mtp_ticket_next(ticket);
  return 1;
}

static void mtp_run(const MtpJob *job, long long lo, long long hi,
                    unsigned slot) {
  if (job->slot_body) {
    job->slot_body(job->ctx, lo, hi, (long long)slot);
  } else {
    job->body(job->ctx, lo, hi);
  }
}

static void mtp_work(unsigned slot) {
  unsigned chunk;
  while (mtp_claim(&chunk)) {
    const MtpJob *job = &g_pool.job;
    long long start = job->lo + job->span * (long long)chunk;
    long long end = start + job->span;
    mtp_run(job, start, end < job->hi ? end : job->hi, slot);
    __atomic_store_n(&g_slots[slot].completed, g_slots[slot].completed + 1u,
                     __ATOMIC_RELEASE);
  }
}

static void mtp_relax(unsigned *rounds) {
  if (*rounds < MTP_SPIN_ROUNDS) {
    mtp_cpu_pause();
  } else {
    mtp_os_yield();
  }
  *rounds += 1u;
}

static void mtp_wake_some(void) {
  int32_t budget = __atomic_load_n(&g_pool.wake_budget, __ATOMIC_ACQUIRE);
  while (budget > 0) {
    int32_t share =
        budget > (int32_t)MTP_WAKE_FANOUT ? (int32_t)MTP_WAKE_FANOUT : budget;
    if (__atomic_compare_exchange_n(&g_pool.wake_budget, &budget,
                                    budget - share, 1, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
      mtp_os_wake(&g_pool.generation, (unsigned)share);
      return;
    }
  }
}

static void mtp_park(uint32_t seen) {
  __atomic_add_fetch(&g_pool.parked, 1, __ATOMIC_SEQ_CST);
  mtp_os_wait(&g_pool.generation, seen);
  __atomic_sub_fetch(&g_pool.parked, 1, __ATOMIC_SEQ_CST);
  mtp_wake_some();
}

static uint32_t mtp_await_generation(uint32_t seen) {
  unsigned rounds = 0;
  uint32_t generation;
  while ((generation = __atomic_load_n(&g_pool.generation, __ATOMIC_ACQUIRE)) ==
         seen) {
    if (rounds < MTP_SPIN_ROUNDS + MTP_YIELD_ROUNDS) {
      mtp_relax(&rounds);
    } else {
      mtp_park(seen);
    }
  }
  return generation;
}

static void mtp_worker(unsigned index) {
  uint32_t seen = 0;
  for (;;) {
    seen = mtp_await_generation(seen);
    mtp_work(index);
  }
}

static void mtp_pool_release(void) {
  __atomic_store_n(&g_pool.busy, 0, __ATOMIC_RELEASE);
}

static void mtp_pool_spawn_once(void) {
  unsigned threads = mtp_threads();
  if (g_pool.spawned) {
    return;
  }
  g_pool.spawned = 1;
  mtp_os_init();
  while (g_pool.workers + 1u < threads && mtp_os_spawn(g_pool.workers)) {
    g_pool.workers++;
  }
}

static int mtp_pool_acquire(void) {
  int32_t idle = 0;
  if (!__atomic_compare_exchange_n(&g_pool.busy, &idle, 1, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    return 0;
  }
  mtp_pool_spawn_once();
  if (!g_pool.workers) {
    mtp_pool_release();
    return 0;
  }
  return 1;
}

static void mtp_publish(unsigned count) {
  uint32_t generation =
      __atomic_load_n(&g_pool.generation, __ATOMIC_RELAXED) + 1u;
  int32_t parked;
  g_pool.dispatched += count;
  __atomic_store_n(&g_pool.ticket, (uint64_t)count, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pool.generation, generation, __ATOMIC_SEQ_CST);
  parked = __atomic_load_n(&g_pool.parked, __ATOMIC_SEQ_CST);
  if (parked > 0) {
    __atomic_store_n(&g_pool.wake_budget, parked, __ATOMIC_RELEASE);
    mtp_wake_some();
  }
}

static uint64_t mtp_completed(void) {
  uint64_t total = 0;
  for (unsigned slot = 0; slot <= g_pool.workers; slot++) {
    total += __atomic_load_n(&g_slots[slot].completed, __ATOMIC_ACQUIRE);
  }
  return total;
}

static void mtp_join(void) {
  unsigned rounds = 0;
  while (mtp_completed() != g_pool.dispatched) {
    mtp_relax(&rounds);
  }
}

static unsigned mtp_plan(long long total, long long min_chunk,
                         long long *span) {
  long long limit = (long long)mtp_threads() * MTP_CHUNKS_PER_THREAD;
  long long count = total / (min_chunk < 1 ? 1 : min_chunk);
  if (count > limit) {
    count = limit;
  }
  if (count < 2) {
    *span = total;
    return 1u;
  }
  *span = (total + count - 1) / count;
  return (unsigned)((total + *span - 1) / *span);
}

static void mtp_dispatch(MtpJob *job, long long min_chunk) {
  long long total = job->hi - job->lo;
  unsigned count;
  if (total <= 0) {
    return;
  }
  count = mtp_plan(total, min_chunk, &job->span);
  if (count < 2u || !mtp_pool_acquire()) {
    mtp_run(job, job->lo, job->hi, 0);
    return;
  }
  g_pool.job = *job;
  mtp_publish(count);
  mtp_work(g_pool.workers);
  mtp_join();
  mtp_pool_release();
}

void mettle_parallel_range(MettleParallelBody body, void *ctx, long long lo,
                           long long hi, long long min_chunk) {
  MtpJob job = {body, NULL, ctx, lo, hi, 0};
  if (body) {
    mtp_dispatch(&job, min_chunk);
  }
}

void mettle_parallel_range_slots(MettleParallelSlotBody body, void *ctx,
                                 long long lo, long long hi,
                                 long long min_chunk) {
  MtpJob job = {NULL, body, ctx, lo, hi, 0};
  if (body) {
    mtp_dispatch(&job, min_chunk);
  }
}

long long mettle_parallel_threads(void) { return (long long)mtp_threads(); }

typedef long long MtpVector __attribute__((vector_size(16), may_alias));
typedef MtpVector MtpUnalignedVector __attribute__((aligned(1)));

typedef struct {
  unsigned char *destination;
  const unsigned char *source;
  unsigned char pattern[16] __attribute__((aligned(16)));
  unsigned period;
  int stream;
} MtpMemoryJob;

static MtpVector mtp_load(const unsigned char *at) {
  return *(const MtpUnalignedVector *)at;
}

static void mtp_store(unsigned char *at, MtpVector value) {
  *(MtpUnalignedVector *)at = value;
}

static void mtp_stream(unsigned char *at, MtpVector value) {
#if defined(__x86_64__)
  __asm__ __volatile__("movntdq %1, %0" : "=m"(*(MtpVector *)at) : "x"(value));
#else
  *(MtpVector *)at = value;
#endif
}

static void mtp_stream_fence(void) {
#if defined(__x86_64__)
  __asm__ __volatile__("sfence" ::: "memory");
#endif
}

static long long mtp_bytes_to_alignment(const unsigned char *at) {
  return (long long)((MTP_VECTOR_BYTES - (uintptr_t)at % MTP_VECTOR_BYTES) %
                     MTP_VECTOR_BYTES);
}

static void mtp_fill_span(unsigned char *d, long long n,
                          const MtpMemoryJob *job) {
  unsigned mask = job->period - 1u;
  long long head = mtp_bytes_to_alignment(d);
  unsigned char block[16] __attribute__((aligned(16)));
  MtpVector value;
  long long i;
  if (head > n) {
    head = n;
  }
  for (i = 0; i < head; i++) {
    d[i] = job->pattern[(unsigned)i & mask];
  }
  for (i = 0; i < 16; i++) {
    block[i] = job->pattern[(unsigned)(head + i) & mask];
  }
  value = *(const MtpVector *)block;
  d += head;
  n -= head;
  if (job->stream) {
    for (; n >= 64; d += 64, n -= 64) {
      mtp_stream(d, value);
      mtp_stream(d + 16, value);
      mtp_stream(d + 32, value);
      mtp_stream(d + 48, value);
    }
    for (; n >= 16; d += 16, n -= 16) {
      mtp_stream(d, value);
    }
    mtp_stream_fence();
  } else {
    for (; n >= 64; d += 64, n -= 64) {
      mtp_store(d, value);
      mtp_store(d + 16, value);
      mtp_store(d + 32, value);
      mtp_store(d + 48, value);
    }
    for (; n >= 16; d += 16, n -= 16) {
      mtp_store(d, value);
    }
  }
  for (i = 0; i < n; i++) {
    d[i] = job->pattern[(unsigned)(head + i) & mask];
  }
}

static void mtp_copy_span(unsigned char *d, const unsigned char *s,
                          long long n, int stream) {
  long long head = mtp_bytes_to_alignment(d);
  long long i;
  if (head > n) {
    head = n;
  }
  for (i = 0; i < head; i++) {
    d[i] = s[i];
  }
  d += head;
  s += head;
  n -= head;
  if (stream) {
    for (; n >= 64; d += 64, s += 64, n -= 64) {
      mtp_stream(d, mtp_load(s));
      mtp_stream(d + 16, mtp_load(s + 16));
      mtp_stream(d + 32, mtp_load(s + 32));
      mtp_stream(d + 48, mtp_load(s + 48));
    }
    for (; n >= 16; d += 16, s += 16, n -= 16) {
      mtp_stream(d, mtp_load(s));
    }
    mtp_stream_fence();
  } else {
    for (; n >= 64; d += 64, s += 64, n -= 64) {
      mtp_store(d, mtp_load(s));
      mtp_store(d + 16, mtp_load(s + 16));
      mtp_store(d + 32, mtp_load(s + 32));
      mtp_store(d + 48, mtp_load(s + 48));
    }
    for (; n >= 16; d += 16, s += 16, n -= 16) {
      mtp_store(d, mtp_load(s));
    }
  }
  for (i = 0; i < n; i++) {
    d[i] = s[i];
  }
}

static void mtp_move_overlapping(unsigned char *d, const unsigned char *s,
                                 long long n) {
  long long i;
  if (d < s) {
    if (s - d >= 16) {
      mtp_copy_span(d, s, n, 0);
      return;
    }
    for (i = 0; i < n; i++) {
      d[i] = s[i];
    }
    return;
  }
  if (d - s >= 16) {
    for (; n >= 16; n -= 16) {
      mtp_store(d + n - 16, mtp_load(s + n - 16));
    }
  }
  for (i = n; i > 0; i--) {
    d[i - 1] = s[i - 1];
  }
}

static void mtp_fill_body(void *ctx, long long lo, long long hi) {
  const MtpMemoryJob *job = (const MtpMemoryJob *)ctx;
  long long period = (long long)job->period;
  mtp_fill_span(job->destination + lo * period, (hi - lo) * period, job);
}

static void mtp_copy_body(void *ctx, long long lo, long long hi) {
  const MtpMemoryJob *job = (const MtpMemoryJob *)ctx;
  mtp_copy_span(job->destination + lo, job->source + lo, hi - lo,
                job->stream);
}

void mettle_parallel_fill(void *destination, long long count, long long value,
                          int element_size) {
  MtpMemoryJob job;
  unsigned period = 1u;
  if (element_size == 2 || element_size == 4 || element_size == 8) {
    period = (unsigned)element_size;
  }
  if (!destination || count <= 0) {
    return;
  }
  for (unsigned i = 0; i < 16u; i++) {
    job.pattern[i] = (unsigned char)(value >> (8u * (i & (period - 1u))));
  }
  job.destination = (unsigned char *)destination;
  job.source = NULL;
  job.period = period;
  job.stream = count * (long long)period >= MTP_STREAM_THRESHOLD_BYTES;
  mettle_parallel_range(mtp_fill_body, &job, 0, count,
                        MTP_MEMORY_CHUNK_BYTES / (long long)period);
}

void mettle_parallel_copy_bytes(void *destination, const void *source,
                                long long bytes) {
  MtpMemoryJob job;
  unsigned char *d = (unsigned char *)destination;
  const unsigned char *s = (const unsigned char *)source;
  if (!destination || !source || bytes <= 0 || d == s) {
    return;
  }
  if (d < s + bytes && s < d + bytes) {
    mtp_move_overlapping(d, s, bytes);
    return;
  }
  job.destination = d;
  job.source = s;
  job.period = 1u;
  job.stream = bytes >= MTP_STREAM_THRESHOLD_BYTES;
  mettle_parallel_range(mtp_copy_body, &job, 0, bytes, MTP_MEMORY_CHUNK_BYTES);
}
