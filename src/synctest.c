// synctest.c -- Q2: synchronisation with semaphores
//
// Part 1: the racy counter of threadtest.c, now protected by a binary
//         semaphore used as a mutex.  The result must be exactly 300,000.
// Part 2: classic bounded-buffer Producer-Consumer (buffer size 5) using
//         three semaphores: empty, full and mutex.
//
// Both parts use threads created with clone() (Q1).

#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmu.h"          // PGSIZE, PGROUNDUP

#define NTHREADS 3
#define NITER    100000

// semaphore ids (the kernel has 10)
#define SEM_COUNTER 0     // mutex for the shared counter
#define SEM_EMPTY   1     // counts empty slots in the buffer
#define SEM_FULL    2     // counts full slots in the buffer
#define SEM_MUTEX   3     // mutual exclusion on the buffer itself

// ---------------------------------------------------------------- helpers

static char *stackmem[8];     // malloc blocks of the threads we started
static int  nstacks = 0;

// Start fn(arg) as a thread on a fresh page-sized stack.
static int
spawn(void (*fn)(void*), void *arg)
{
  char *blk = malloc(2 * PGSIZE);
  int pid;

  if(blk == 0){
    printf(1, "synctest: out of memory\n");
    exit();
  }
  stackmem[nstacks++] = blk;
  pid = clone(fn, arg, (char*)PGROUNDUP((uint)blk));
  if(pid < 0){
    printf(1, "synctest: clone failed\n");
    exit();
  }
  return pid;
}

static void
joinall(int n)
{
  int i;
  for(i = 0; i < n; i++)
    join();
  for(i = 0; i < nstacks; i++)
    free(stackmem[i]);
  nstacks = 0;
}

// ---------------------------------------------------------------- part 1

volatile int counter = 0;

void
counter_worker(void *arg)
{
  int i;

  for(i = 0; i < NITER; i++){
    sem_wait(SEM_COUNTER);           // ---- enter critical section
    int tmp = counter;               // the same load/add/store as threadtest
    tmp = tmp + 1;
    counter = tmp;
    sem_signal(SEM_COUNTER);         // ---- leave critical section
  }
  exit();
}

static int
part1(void)
{
  int i;

  printf(1, "\n=== Part 1: shared counter protected by a semaphore ===\n");
  printf(1, "%d threads x %d increments, expected %d\n",
         NTHREADS, NITER, NTHREADS * NITER);
  if(sem_init(SEM_COUNTER, 1) < 0){
    printf(1, "sem_init failed\n");
    exit();
  }
  counter = 0;
  for(i = 0; i < NTHREADS; i++)
    spawn(counter_worker, 0);
  joinall(NTHREADS);

  printf(1, "final counter = %d (expected %d)  -> %s\n", counter,
         NTHREADS * NITER, counter == NTHREADS * NITER ? "CORRECT" : "WRONG");
  printf(1, "(threadtest without semaphores loses ~130000 updates)\n");
  return counter == NTHREADS * NITER;
}

// ---------------------------------------------------------------- part 2

#define BUFSZ  5
#define NITEMS 15

int buf[BUFSZ];
int in_idx = 0, out_idx = 0;
int count = 0;                       // items in buffer (checked, not used)
int maxcount = 0, mincount = 0;
int bad = 0;                         // set on any overflow / underflow / mismatch
int consumed_ok = 0;

void
producer(void *arg)
{
  int item;

  for(item = 1; item <= NITEMS; item++){
    sem_wait(SEM_EMPTY);             // wait for a free slot
    sem_wait(SEM_MUTEX);
    buf[in_idx] = item;
    count++;
    if(count > BUFSZ) bad = 1;       // overflow would be detected here
    if(count > maxcount) maxcount = count;
    printf(1, "  [producer] put item %d in slot %d   (buffer holds %d)\n",
           item, in_idx, count);
    in_idx = (in_idx + 1) % BUFSZ;
    sem_signal(SEM_MUTEX);
    sem_signal(SEM_FULL);            // one more full slot
  }
  exit();
}

void
consumer(void *arg)
{
  int i, item;

  sleep(20);                         // start late: lets the producer fill the
                                     // buffer (5 slots) and block on "empty"
  for(i = 1; i <= NITEMS; i++){
    sem_wait(SEM_FULL);              // wait for an item
    sem_wait(SEM_MUTEX);
    item = buf[out_idx];
    count--;
    if(count < 0) bad = 1;           // underflow would be detected here
    if(count < mincount) mincount = count;
    if(item != i) bad = 1;           // items must come out in FIFO order
    else consumed_ok++;
    printf(1, "  [consumer] got item %d from slot %d (buffer holds %d)\n",
           item, out_idx, count);
    out_idx = (out_idx + 1) % BUFSZ;
    sem_signal(SEM_MUTEX);
    sem_signal(SEM_EMPTY);           // one more empty slot
    sleep(1);                        // consumer is slower -> buffer fills up
  }
  exit();
}

static int
part2(void)
{
  printf(1, "\n=== Part 2: Producer-Consumer, buffer size %d, %d items ===\n",
         BUFSZ, NITEMS);
  if(sem_init(SEM_EMPTY, BUFSZ) < 0 || sem_init(SEM_FULL, 0) < 0 ||
     sem_init(SEM_MUTEX, 1) < 0){
    printf(1, "sem_init failed\n");
    exit();
  }
  spawn(producer, 0);
  spawn(consumer, 0);
  joinall(2);

  printf(1, "produced %d, consumed in order %d, max fill %d, min fill %d, "
            "left in buffer %d\n", NITEMS, consumed_ok, maxcount, mincount, count);
  printf(1, "producer-consumer: %s\n",
         (!bad && consumed_ok == NITEMS && count == 0 && maxcount <= BUFSZ)
         ? "PASSED (no overflow, no underflow)" : "FAILED");
  return !bad && consumed_ok == NITEMS && count == 0;
}

int
main(int argc, char *argv[])
{
  int ok1 = part1();
  int ok2 = part2();

  printf(1, "\nsynctest: %s\n", ok1 && ok2 ? "ALL PASSED" : "SOME TESTS FAILED");
  exit();
}
