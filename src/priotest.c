// priotest.c -- Q4: priority scheduling with aging
//
// Forks 4 CPU-bound children with distinct priorities (2, 6, 12 and the
// very low 20).  The parent runs at priority 1 so it can sleep, wake up
// and sample every child's priority and wait-tick counter while they are
// busy; it prints those samples as a table over time, then the order in
// which the children completed.
//
// Usage:  priotest            run the experiment
//         priotest <millions> use <millions> million loop iterations per
//                             child (default NMILLION)
//         priotest calib      time one child-sized job on an idle system
//
// For clean results boot xv6 with ONE cpu:   make qemu-nox CPUS=1

#include "types.h"
#include "stat.h"
#include "user.h"

#define NCHILD   4
#define NMILLION 200         // loop iterations per child, in millions
#define SAMPLE   20          // parent samples every SAMPLE ticks

static int prios[NCHILD] = { 2, 6, 12, 20 };

struct rec {
  int pid;
  int init_prio;
  int final_prio;
  int finish;                // ticks since the experiment started
};

// A purely CPU-bound loop.  volatile keeps the compiler from deleting it.
static void
cpuwork(int million)
{
  volatile unsigned x = 1;
  int m;
  unsigned i;

  for(m = 0; m < million; m++)
    for(i = 0; i < 1000000; i++)
      x = x * 1103515245u + 12345u;
}

// print v right-aligned in a field of width w
static void
pn(int v, int w)
{
  int digits = 1, t = v;

  while(t >= 10){ digits++; t /= 10; }
  while(w-- > digits)
    printf(1, " ");
  printf(1, "%d", v);
}

int
main(int argc, char *argv[])
{
  int million = NMILLION;
  int pids[NCHILD];
  struct rec r[NCHILD];
  int i, j, alive, start, t, pr, wt;
  int rp[2];

  if(argc > 1 && strcmp(argv[1], "calib") == 0){
    start = uptime();
    cpuwork(million);
    printf(1, "priotest calib: %d million iterations took %d ticks\n",
           million, uptime() - start);
    exit();
  }
  if(argc > 1)
    million = atoi(argv[1]);

  printf(1, "priotest: %d CPU-bound children, %d million iterations each\n",
         NCHILD, million);
  printf(1, "          priorities (1 = highest, 20 = lowest):");
  for(i = 0; i < NCHILD; i++)
    printf(1, " %d", prios[i]);
  printf(1, "\n          aging: +1 priority after more than 30 ticks waiting\n\n");

  if(pipe(rp) < 0){
    printf(1, "priotest: pipe failed\n");
    exit();
  }
  set_priority(getpid(), 1);            // parent = observer, highest priority
  start = uptime();

  for(i = 0; i < NCHILD; i++){
    int pid = fork();
    if(pid < 0){
      printf(1, "priotest: fork failed\n");
      exit();
    }
    if(pid == 0){
      struct rec me;
      // (priority is set by the parent right after fork; setting it again
      //  here would undo any aging boost earned while waiting to run)
      me.pid = getpid();
      me.init_prio = prios[i];
      cpuwork(million);
      me.final_prio = get_priority(getpid());
      me.finish = uptime() - start;
      write(rp[1], &me, sizeof(me));
      exit();
    }
    pids[i] = pid;
    set_priority(pid, prios[i]);        // parent also sets it, so it is
  }                                     // in force before the child runs

  // ---- sample priority / wait ticks of every child while they compute
  printf(1, " tick |");
  for(i = 0; i < NCHILD; i++){
    printf(1, " pid ");
    pn(pids[i], 2);
    printf(1, " start ");
    pn(prios[i], 2);
    printf(1, " |");
  }
  printf(1, "\n      |");
  for(i = 0; i < NCHILD; i++)
    printf(1, "   prio / wait   |");
  printf(1, "\n");
  for(;;){
    sleep(SAMPLE);
    t = uptime() - start;
    alive = 0;
    pn(t, 5);
    printf(1, " |");
    for(i = 0; i < NCHILD; i++){
      pr = get_priority(pids[i]);
      wt = get_waitticks(pids[i]);
      if(pr < 0){
        printf(1, "     finished    |");
      } else {
        alive++;
        printf(1, "     ");
        pn(pr, 2);
        printf(1, " / ");
        pn(wt, 2);
        printf(1, "     |");
      }
    }
    printf(1, "\n");
    if(alive == 0)
      break;
  }

  // ---- collect the completion records (in completion order)
  for(i = 0; i < NCHILD; i++)
    if(read(rp[0], &r[i], sizeof(struct rec)) != sizeof(struct rec)){
      printf(1, "priotest: lost a record\n");
      exit();
    }
  for(i = 0; i < NCHILD; i++)
    wait();

  printf(1, "\ncompletion order:\n");
  printf(1, "  #  pid  start-prio  final-prio  finished-at-tick\n");
  for(i = 0; i < NCHILD; i++){
    printf(1, "  %d ", i + 1);
    pn(r[i].pid, 4);
    printf(1, " ");
    pn(r[i].init_prio, 10);
    printf(1, "  ");
    pn(r[i].final_prio, 10);
    printf(1, "  ");
    pn(r[i].finish, 16);
    printf(1, "\n");
  }

  // Did the order follow the starting priorities?
  for(i = 0, j = 1; i < NCHILD - 1; i++)
    if(r[i].init_prio > r[i + 1].init_prio)
      j = 0;
  printf(1, "\nfinished in order of starting priority: %s\n", j ? "yes" : "no");
  for(i = 0; i < NCHILD; i++)
    if(r[i].init_prio == 20)
      printf(1, "lowest-priority process (started at 20) completed %s, "
                "aged to priority %d\n",
             i == NCHILD - 1 ? "last" : "before the end", r[i].final_prio);
  exit();
}
