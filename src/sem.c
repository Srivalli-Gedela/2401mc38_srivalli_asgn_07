// sem.c -- Q2: counting semaphores (kernel side)
//
// A fixed table of NSEM semaphores.  The table itself (which slots are in
// use) is protected by semtable.lock; the counter of each individual
// semaphore is protected by that semaphore's own spinlock "lk".  Blocking
// and waking use xv6's sleep()/wakeup().
//
// Semantics (Dijkstra / "negative count" style, as in the assignment):
//   value >= 0 : number of units still available
//   value <  0 : -value processes are blocked waiting
//
//   sem_wait   : value--; if value < 0 -> block
//   sem_signal : value++; if value <= 0 -> wake ONE waiter
//
// xv6's wakeup() wakes EVERY process sleeping on a channel, so a plain
// wakeup() would let all waiters through on a single signal.  To release
// exactly one, signal() hands out a "wakeup ticket" (wakeups++), and a
// woken waiter may only proceed if it can take a ticket; the others find
// no ticket and go back to sleep.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define NSEM 10

struct semaphore {
  int value;                // see above
  int wakeups;              // tickets: waiters that have been released
  int inuse;                // slot has been initialised by sem_init()
  struct spinlock lk;       // protects value and wakeups
};

struct {
  struct spinlock lock;     // protects the inuse flags / table as a whole
  struct semaphore sem[NSEM];
} semtable;

void
seminit(void)
{
  int i;

  initlock(&semtable.lock, "semtable");
  for(i = 0; i < NSEM; i++){
    initlock(&semtable.sem[i].lk, "sem");
    semtable.sem[i].inuse = 0;
    semtable.sem[i].value = 0;
    semtable.sem[i].wakeups = 0;
  }
}

// Look up an initialised semaphore; returns 0 if id is invalid/unused.
static struct semaphore*
semget(int id)
{
  struct semaphore *s;

  if(id < 0 || id >= NSEM)
    return 0;
  acquire(&semtable.lock);
  s = &semtable.sem[id];
  if(!s->inuse)
    s = 0;
  release(&semtable.lock);
  return s;
}

// Initialise semaphore `id` to `value`.  Returns 0, or -1 if id is out of
// range, value is negative, or processes are still blocked on it.
int
sem_init(int id, int value)
{
  struct semaphore *s;

  if(id < 0 || id >= NSEM || value < 0)
    return -1;

  acquire(&semtable.lock);
  s = &semtable.sem[id];
  acquire(&s->lk);
  if(s->inuse && s->value < 0){      // somebody is blocked on it right now
    release(&s->lk);
    release(&semtable.lock);
    return -1;
  }
  s->value = value;
  s->wakeups = 0;
  s->inuse = 1;
  release(&s->lk);
  release(&semtable.lock);
  return 0;
}

// P / down / wait.  Returns 0, or -1 on a bad id (or if the caller was
// killed while blocked).
int
sem_wait(int id)
{
  struct semaphore *s = semget(id);

  if(s == 0)
    return -1;

  acquire(&s->lk);
  s->value--;
  if(s->value < 0){
    // Block until signal() leaves us a ticket.
    while(s->wakeups == 0){
      if(myproc()->killed){
        s->value++;                  // undo our decrement; we never got in
        release(&s->lk);
        return -1;
      }
      sleep(s, &s->lk);              // atomically releases s->lk
    }
    s->wakeups--;                    // consume our ticket
  }
  release(&s->lk);
  return 0;
}

// V / up / signal.  Returns 0, or -1 on a bad id.
int
sem_signal(int id)
{
  struct semaphore *s = semget(id);

  if(s == 0)
    return -1;

  acquire(&s->lk);
  s->value++;
  if(s->value <= 0){                 // there was at least one waiter
    s->wakeups++;                    // release exactly one of them
    wakeup(s);
  }
  release(&s->lk);
  return 0;
}
