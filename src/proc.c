#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;

#define DEFPRIORITY 10   // Q4: default priority
#define MINPRIORITY 20   // Q4: numerically largest = lowest priority
#define MAXPRIORITY 1    // Q4: numerically smallest = highest priority
#define AGETICKS    30   // Q4: wait more than this many ticks -> priority boost
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
static int reapthreads(struct proc *leader);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->priority = DEFPRIORITY;   // Q4: every new process starts at priority 10
  p->waitticks = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait() (or join(), if we are a thread).
  wakeup1(curproc->parent);
  // Q1: if we are a thread, the group leader may be a zombie whose own
  // parent is sleeping in wait() until every thread has gone away.
  if(curproc->isthread && curproc->leader)
    wakeup1(curproc->leader->parent);

  // Q1: if we are an ordinary process (a thread-group leader), the other
  // threads of the group cannot outlive the address space they share.
  // Mark them killed; they will call exit() the next time they trap.
  if(!curproc->isthread){
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->isthread && p->leader == curproc && p->state != UNUSED &&
         p->state != ZOMBIE){
        p->killed = 1;
        if(p->state == SLEEPING)
          p->state = RUNNABLE;
      }
    }
  }

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      if(p->isthread){
        // Threads are reaped by their leader (wait/join), never by init.
        p->parent = p->leader;
      } else {
        p->parent = initproc;
        if(p->state == ZOMBIE)
          wakeup1(initproc);
      }
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      // Q1: threads are reaped with join(), not wait(): freeing their
      // pgdir here would destroy the address space of the whole group.
      if(p->isthread)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Q1: a process may only release its address space once no live
        // thread still runs in it.  Zombie threads are reaped here.
        if(reapthreads(p) == 0)
          continue;          // live threads remain; keep sleeping below
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Q1 helper (ptable.lock held).  Reap every ZOMBIE thread whose group
// leader is `leader`.  Returns 1 if no live (non-zombie) thread of that
// group remains afterwards, 0 otherwise.
static int
reapthreads(struct proc *leader)
{
  struct proc *t;
  int live = 0;

  for(t = ptable.proc; t < &ptable.proc[NPROC]; t++){
    if(!t->isthread || t->leader != leader || t->state == UNUSED)
      continue;
    if(t->state == ZOMBIE){
      kfree(t->kstack);
      t->kstack = 0;
      // NOTE: no freevm() -- the page directory belongs to the leader.
      t->pgdir = 0;
      t->pid = 0;
      t->parent = 0;
      t->leader = 0;
      t->isthread = 0;
      t->name[0] = 0;
      t->killed = 0;
      t->state = UNUSED;
    } else
      live = 1;
  }
  return !live;
}

// Q1: create a thread.  The new "process" shares the caller's page
// directory (so all memory is shared), but has its own kernel stack,
// trap frame, and pid.  It starts at fcn(arg) on the user supplied stack.
// `stack` is the LOWEST address of a PGSIZE-byte user stack region; the
// thread's stack pointer starts at stack+PGSIZE and grows downward.
// Returns the new thread's pid in the caller, -1 on error.
int
clone(void(*fcn)(void*), void *arg, void *stack)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();
  uint sp;

  if(fcn == 0 || stack == 0)
    return -1;
  // The whole stack page must lie inside the shared address space.
  if((uint)stack < PGSIZE || (uint)stack + PGSIZE > curproc->sz ||
     (uint)stack + PGSIZE < (uint)stack)
    return -1;

  if((np = allocproc()) == 0)
    return -1;

  np->pgdir = curproc->pgdir;           // SHARED address space
  np->sz = curproc->sz;
  np->parent = curproc;
  np->isthread = 1;
  np->leader = curproc->isthread ? curproc->leader : curproc;
  *np->tf = *curproc->tf;

  // Build the initial user stack:  [fake return pc][arg]  (top of stack)
  sp = (uint)stack + PGSIZE;
  sp -= 4;
  *(uint*)sp = (uint)arg;               // argument for fcn
  sp -= 4;
  *(uint*)sp = 0xffffffff;              // fake return address: a thread
                                        // function must call exit()
  np->tf->esp = sp;
  np->tf->eip = (uint)fcn;
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;
  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);
  return pid;
}

// Q1: wait for a thread created (directly) by the caller to finish.
// Frees only the thread's kernel stack and proc slot -- the shared page
// directory is left alone.  Returns the thread's pid, or -1 if the caller
// has no threads.
int
join(void)
{
  struct proc *p;
  int havethreads, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    havethreads = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc || !p->isthread)
        continue;
      havethreads = 1;
      if(p->state == ZOMBIE){
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        p->pgdir = 0;           // do NOT freevm(): shared with the group
        p->pid = 0;
        p->parent = 0;
        p->leader = 0;
        p->isthread = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }
    if(!havethreads || curproc->killed){
      release(&ptable.lock);
      return -1;
    }
    sleep(curproc, &ptable.lock);
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p, *best;
  struct cpu *c = mycpu();
  int i, idx;
  int last = NPROC - 1;     // table index of the process this CPU ran last
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);

    // Q4: pick the RUNNABLE process with the numerically LOWEST priority
    // value (= highest priority).  The scan starts just after the process
    // that ran last and wraps around, and only a strictly better priority
    // replaces the current candidate, so processes of equal priority are
    // served in round-robin order.
    best = 0;
    for(i = 1; i <= NPROC; i++){
      idx = (last + i) % NPROC;
      p = &ptable.proc[idx];
      if(p->state != RUNNABLE)
        continue;
      if(best == 0 || p->priority < best->priority)
        best = p;
    }

    if(best != 0){
      p = best;
      last = p - ptable.proc;
      p->waitticks = 0;           // it is being scheduled: its wait is over

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Q4: aging.  Called once per timer tick (from trap.c, on CPU 0).
// Every RUNNABLE process that has now waited more than AGETICKS ticks
// without being scheduled gets its priority raised by one step (priority
// number decremented, never below 1) and its wait counter reset, so a
// low-priority process cannot be starved forever.
void
agetick(void)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNABLE){
      p->waitticks++;
      if(p->waitticks > AGETICKS){
        if(p->priority > MAXPRIORITY)
          p->priority--;
        p->waitticks = 0;
      }
    } else
      p->waitticks = 0;           // running, sleeping, ...: not waiting for a CPU
  }
  release(&ptable.lock);
}

// Q4: set_priority(pid, priority).  0 on success, -1 if the priority is
// outside 1..20 or no such live process exists.
int
set_priority(int pid, int priority)
{
  struct proc *p;

  if(priority < MAXPRIORITY || priority > MINPRIORITY)
    return -1;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid && p->state != UNUSED && p->state != ZOMBIE){
      p->priority = priority;
      p->waitticks = 0;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Q4: get_priority(pid): the priority, or -1 if there is no live process.
int
get_priority(int pid)
{
  struct proc *p;
  int prio = -1;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid && p->state != UNUSED && p->state != ZOMBIE){
      prio = p->priority;
      break;
    }
  }
  release(&ptable.lock);
  return prio;
}

// Q4 (observation aid): get_waitticks(pid): how many ticks the process has
// currently been waiting RUNNABLE, or -1 if there is no live process.
int
get_waitticks(int pid)
{
  struct proc *p;
  int w = -1;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid && p->state != UNUSED && p->state != ZOMBIE){
      w = p->waitticks;
      break;
    }
  }
  release(&ptable.lock);
  return w;
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
