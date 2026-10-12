// threadtest.c -- Q1: kernel threads with clone()/join()
//
// Creates 3 threads with clone().  Every thread increments one shared
// global counter 100,000 times.  There is NO synchronisation, so the
// read-modify-write of "counter" races and the final value is normally
// LESS than 300,000.  synctest.c (Q2) fixes this with semaphores.

#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"
#include "mmu.h"          // PGSIZE

#define NTHREADS 3
#define NITER    100000

// volatile forces a real load / add / store on every iteration so the
// compiler cannot collapse the loop into a single "counter += NITER".
volatile int counter = 0;

void
worker(void *arg)
{
  int i;
  volatile int d;

  (void)arg;                         // thread number, unused here

  for(i = 0; i < NITER; i++){
    // counter++ written as an explicit load / add / store.  Two threads
    // that load the same value both store value+1, losing one update.
    int tmp = counter;               // load
    tmp = tmp + 1;                   // modify
    for(d = 0; d < 20; d++)          // small delay widens the window between
      ;                              // load and store, so the race also shows
                                     // up on a single CPU (timer preemption)
    counter = tmp;                   // store     <-- the race
  }
  exit();                            // a thread function must end with exit()
}

int
main(int argc, char *argv[])
{
  int i, pid;
  char *stacks[NTHREADS];            // malloc'ed blocks (to free later)
  char *stack;

  printf(1, "threadtest: %d threads x %d increments, expected %d\n",
         NTHREADS, NITER, NTHREADS * NITER);

  // Create the threads.  Each gets its own PGSIZE-byte user stack carved
  // out of a malloc'ed block; the address space itself is shared, which is
  // why every thread sees (and races on) the same global "counter".
  for(i = 0; i < NTHREADS; i++){
    stacks[i] = malloc(2 * PGSIZE);
    if(stacks[i] == 0){
      printf(1, "threadtest: malloc failed\n");
      exit();
    }
    stack = (char*)PGROUNDUP((uint)stacks[i]);   // PGSIZE bytes inside block
    pid = clone(worker, (void*)i, stack);
    if(pid < 0){
      printf(1, "threadtest: clone failed\n");
      exit();
    }
    printf(1, "  created thread %d with pid %d\n", i, pid);
  }

  for(i = 0; i < NTHREADS; i++){
    pid = join();
    printf(1, "  joined thread pid %d\n", pid);
  }
  printf(1, "  join() with no threads left returns %d\n", join());

  for(i = 0; i < NTHREADS; i++)
    free(stacks[i]);

  printf(1, "threadtest: final counter = %d (expected %d)\n",
         counter, NTHREADS * NITER);
  if(counter == NTHREADS * NITER)
    printf(1, "threadtest: no race observed this run (try again)\n");
  else
    printf(1, "threadtest: RACE CONDITION: lost %d updates\n",
           NTHREADS * NITER - counter);
  exit();
}
