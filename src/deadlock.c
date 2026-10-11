// deadlock.c -- Q3: deadlock detection and Banker's-algorithm avoidance
//
// A user-space simulation of a multi-resource allocator.  n processes and
// m resource types are described by static arrays:
//
//   Allocation[i][j]  units of resource j currently held by process i
//   Request[i][j]     units of resource j process i is waiting for NOW
//                     (used by deadlock DETECTION)
//   Max[i][j]         maximum units of resource j process i may ever claim
//                     (used by deadlock AVOIDANCE; Need = Max - Allocation)
//   Available[j]      free units of resource j
//
// Part a/b : detect_deadlock()        on a deadlocked and on a safe matrix
// Part d   : is_safe_state(), request_resources()  (Banker's algorithm)
// Part c   : a REAL deadlock between two xv6 processes using pipes, found
//            by feeding the observed state into detect_deadlock().
//            Run "deadlock pause" to get a 5 second pause in which Ctrl-P
//            shows both processes sleeping inside read().

#include "types.h"
#include "stat.h"
#include "user.h"

#define MAXP 5
#define MAXR 3

int n, m;                          // number of processes / resource types
int Allocation[MAXP][MAXR];
int Request[MAXP][MAXR];
int Max[MAXP][MAXR];
int Available[MAXR];
int deadlocked[MAXP];              // filled in by detect_deadlock()

// ------------------------------------------------------------ printing

static void
pvec(int *v, int k)
{
  int j;
  printf(1, "<");
  for(j = 0; j < k; j++)
    printf(1, "%d%s", v[j], j == k - 1 ? "" : " ");
  printf(1, ">");
}

// Print Available and Allocation, then either Request or Max/Need.
static void
print_state(int show_max)
{
  int i, j, need[MAXR];

  printf(1, "  Available = ");
  pvec(Available, m);
  printf(1, "\n");
  if(show_max)
    printf(1, "        Allocation   Max          Need\n");
  else
    printf(1, "        Allocation   Request\n");
  for(i = 0; i < n; i++){
    printf(1, "   P%d   ", i);
    pvec(Allocation[i], m);
    printf(1, "      ");
    if(show_max){
      pvec(Max[i], m);
      printf(1, "    ");
      for(j = 0; j < m; j++)
        need[j] = Max[i][j] - Allocation[i][j];
      pvec(need, m);
    } else
      pvec(Request[i], m);
    printf(1, "\n");
  }
}

// ------------------------------------------------------------ detection

// Deadlock detection algorithm.
//   Work = Available.  A process that holds nothing cannot be part of a
//   deadlock, so it starts out "finished".  Repeatedly find an unfinished
//   process whose Request <= Work, pretend it runs to completion and
//   returns everything it holds (Work += Allocation).  Whatever can never
//   finish is deadlocked.
// Returns the number of deadlocked processes; their indices are stored in
// deadlocked[0 .. count-1].
int
detect_deadlock(void)
{
  int work[MAXR], finish[MAXP];
  int i, j, progress, count;

  for(j = 0; j < m; j++)
    work[j] = Available[j];
  for(i = 0; i < n; i++){
    finish[i] = 1;
    for(j = 0; j < m; j++)
      if(Allocation[i][j] != 0)
        finish[i] = 0;
  }

  printf(1, "  detection: Work = ");
  pvec(work, m);
  printf(1, "\n");
  do {
    progress = 0;
    for(i = 0; i < n; i++){
      if(finish[i])
        continue;
      for(j = 0; j < m; j++)
        if(Request[i][j] > work[j])
          break;
      if(j == m){                          // Request[i] <= Work
        for(j = 0; j < m; j++)
          work[j] += Allocation[i][j];     // P(i) finishes, releases all
        finish[i] = 1;
        progress = 1;
        printf(1, "  P%d can finish -> Work = ", i);
        pvec(work, m);
        printf(1, "\n");
      }
    }
  } while(progress);

  count = 0;
  for(i = 0; i < n; i++)
    if(!finish[i])
      deadlocked[count++] = i;

  if(count == 0){
    printf(1, "  RESULT: no deadlock\n");
  } else {
    printf(1, "  RESULT: DEADLOCK among processes:");
    for(i = 0; i < count; i++)
      printf(1, " P%d", deadlocked[i]);
    printf(1, "\n");
  }
  return count;
}

// ------------------------------------------------------------ Banker's

// Safety algorithm.  Need = Max - Allocation.  Returns 1 if the current
// state is safe (some order exists in which every process can obtain its
// whole Need and finish), 0 if it is unsafe.
int
is_safe_state(void)
{
  int work[MAXR], finish[MAXP], seq[MAXP];
  int i, j, progress, cnt = 0;

  for(j = 0; j < m; j++)
    work[j] = Available[j];
  for(i = 0; i < n; i++)
    finish[i] = 0;

  do {
    progress = 0;
    for(i = 0; i < n; i++){
      if(finish[i])
        continue;
      for(j = 0; j < m; j++)
        if(Max[i][j] - Allocation[i][j] > work[j])
          break;
      if(j == m){                          // Need[i] <= Work
        for(j = 0; j < m; j++)
          work[j] += Allocation[i][j];
        finish[i] = 1;
        seq[cnt++] = i;
        progress = 1;
        printf(1, "  P%d can finish -> Work = ", i);
        pvec(work, m);
        printf(1, "\n");
      }
    }
  } while(progress);

  if(cnt == n){
    printf(1, "  SAFE state, safe sequence: <");
    for(i = 0; i < n; i++)
      printf(1, "P%d%s", seq[i], i == n - 1 ? "" : ", ");
    printf(1, ">\n");
    return 1;
  }
  printf(1, "  UNSAFE state: only %d of %d processes can be guaranteed to finish\n",
         cnt, n);
  return 0;
}

// Resource-request algorithm: may process pid be given req[] right now?
//  returns  1 granted (state updated),
//           0 refused: not enough Available (must wait) or would be UNSAFE
//             (state is rolled back),
//          -1 error: request exceeds the process's declared Max.
int
request_resources(int pid, int *req)
{
  int j;

  printf(1, "  P%d requests ", pid);
  pvec(req, m);
  printf(1, "\n");

  for(j = 0; j < m; j++)
    if(req[j] > Max[pid][j] - Allocation[pid][j]){
      printf(1, "  -> ERROR: exceeds P%d's maximum claim\n", pid);
      return -1;
    }
  for(j = 0; j < m; j++)
    if(req[j] > Available[j]){
      printf(1, "  -> not enough Available resources, P%d must wait\n", pid);
      return 0;
    }

  // Pretend to allocate ...
  for(j = 0; j < m; j++){
    Available[j] -= req[j];
    Allocation[pid][j] += req[j];
  }
  printf(1, "  pretend-allocate and test the new state:\n");
  if(is_safe_state()){
    printf(1, "  -> GRANTED\n");
    return 1;
  }
  // ... and undo it if the result is unsafe.
  for(j = 0; j < m; j++){
    Available[j] += req[j];
    Allocation[pid][j] -= req[j];
  }
  printf(1, "  -> DENIED: granting it would make the state unsafe (rolled back)\n");
  return 0;
}

// ------------------------------------------------------------ test data

static void
load(int np, int nr, int *a, int *r, int *mx, int *av)
{
  int i, j;

  n = np;
  m = nr;
  for(i = 0; i < np; i++)
    for(j = 0; j < nr; j++){
      Allocation[i][j] = a  ? a[i * nr + j]  : 0;
      Request[i][j]    = r  ? r[i * nr + j]  : 0;
      Max[i][j]        = mx ? mx[i * nr + j] : 0;
    }
  for(j = 0; j < nr; j++)
    Available[j] = av[j];
}

// Test 1: deadlocked (resource types A, B, C)
int t1_alloc[] = {0,1,0,  2,0,0,  3,0,3,  2,1,1,  0,0,2};
int t1_req[]   = {0,0,0,  2,0,2,  0,0,1,  1,0,0,  0,0,2};
int t1_avail[] = {0,0,0};

// Test 2: same allocation, but P2 is no longer asking for anything -> safe
int t2_req[]   = {0,0,0,  2,0,2,  0,0,0,  1,0,0,  0,0,2};

// Test 3: Banker's algorithm (classic 5 processes, A=10 B=5 C=7 instances)
int t3_alloc[] = {0,1,0,  2,0,0,  3,0,2,  2,1,1,  0,0,2};
int t3_max[]   = {7,5,3,  3,2,2,  9,0,2,  2,2,2,  4,3,3};
int t3_avail[] = {3,3,2};

static int failures = 0;

static void
check(char *what, int cond)
{
  printf(1, "  [%s] %s\n", cond ? "PASS" : "FAIL", what);
  if(!cond)
    failures++;
}

// ------------------------------------------------------------ pipe demo

struct report {
  int proc;        // 0 = A, 1 = B
  int holds;       // resource it holds
  int wants;       // resource it is waiting for
};

// Resource 0 = write end of pipe 1, resource 1 = write end of pipe 2.
// A keeps pipe1's write end and then blocks reading pipe2;
// B keeps pipe2's write end and then blocks reading pipe1.
// A read() on an empty pipe only returns when data arrives or when EVERY
// write end is closed -- but each write end is held by the other process.
static void
pipe_deadlock_demo(int pause)
{
  int p1[2], p2[2], st[2];
  int pidA, pidB, i;
  struct report rep[2], r;
  char c;

  printf(1, "\n=== Part c: REAL deadlock with two processes and two pipes ===\n");
  printf(1, "  resource 0 = write end of pipe1, resource 1 = write end of pipe2\n");
  if(pipe(p1) < 0 || pipe(p2) < 0 || pipe(st) < 0){
    printf(1, "  pipe() failed\n");
    exit();
  }

  pidA = fork();
  if(pidA == 0){                    // ---- process A (P0)
    close(p1[0]); close(p2[1]); close(st[0]);   // keep p1[1], p2[0]
    r.proc = 0; r.holds = 0; r.wants = 1;
    write(st[1], &r, sizeof(r));
    read(p2[0], &c, 1);             // blocks "forever" (until killed)
    exit();
  }
  pidB = fork();
  if(pidB == 0){                    // ---- process B (P1)
    close(p2[0]); close(p1[1]); close(st[0]);   // keep p2[1], p1[0]
    r.proc = 1; r.holds = 1; r.wants = 0;
    write(st[1], &r, sizeof(r));
    read(p1[0], &c, 1);             // blocks "forever" (until killed)
    exit();
  }

  // ---- monitor (parent): close every pipe end, then collect the reports
  close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]); close(st[1]);
  for(i = 0; i < 2; i++){
    if(read(st[0], &rep[i], sizeof(struct report)) != sizeof(struct report)){
      printf(1, "  monitor: lost a report\n");
      exit();
    }
  }
  sleep(30);                        // give both time to block for good
  for(i = 0; i < 2; i++)
    printf(1, "  %c (pid %d) holds pipe%d.write and is blocked reading pipe%d\n",
           'A' + rep[i].proc, rep[i].proc ? pidB : pidA,
           rep[i].holds + 1, rep[i].wants + 1);
  if(pause){
    printf(1, "  (paused 5 s: press Ctrl-P now to see both processes asleep)\n");
    sleep(500);
  }

  // Build the resource-allocation state from the reports.
  load(2, 2, 0, 0, 0, (int[]){0, 0});
  for(i = 0; i < 2; i++){
    Allocation[rep[i].proc][rep[i].holds] = 1;
    Request[rep[i].proc][rep[i].wants] = 1;
  }
  print_state(0);
  if(detect_deadlock() == 2)
    printf(1, "  circular wait confirmed: P0 -> pipe2 (held by P1) -> "
              "pipe1 (held by P0)\n");
  check("real pipe deadlock detected (P0 and P1)",
        deadlocked[0] == 0 && deadlocked[1] == 1);

  // Break the deadlock so the demo can finish.
  kill(pidA);
  kill(pidB);
  wait();
  wait();
  close(st[0]);
}

// ------------------------------------------------------------ main

int
main(int argc, char *argv[])
{
  int req1[] = {1, 0, 2};   // P1 asks for A=1 C=2
  int req2[] = {3, 3, 0};   // P4 asks for A=3 B=3
  int req3[] = {0, 2, 0};   // P0 asks for B=2

  printf(1, "deadlock: detection and avoidance in xv6 user space\n");

  printf(1, "\n=== Test 1: deadlock detection, DEADLOCKED matrix ===\n");
  load(5, 3, t1_alloc, t1_req, 0, t1_avail);
  print_state(0);
  check("P1 P2 P3 P4 are deadlocked, P0 is not",
        detect_deadlock() == 4 && deadlocked[0] == 1 && deadlocked[3] == 4);

  printf(1, "\n=== Test 2: deadlock detection, SAFE matrix ===\n");
  load(5, 3, t1_alloc, t2_req, 0, t1_avail);
  print_state(0);
  check("no deadlock", detect_deadlock() == 0);

  printf(1, "\n=== Test 3: Banker's algorithm ===\n");
  load(5, 3, t3_alloc, 0, t3_max, t3_avail);
  printf(1, "-- 3a: initial state\n");
  print_state(1);
  check("initial state is safe", is_safe_state() == 1);

  printf(1, "\n-- 3b: P1 requests <1 0 2> (can be granted)\n");
  check("P1 <1 0 2> granted", request_resources(1, req1) == 1);
  print_state(1);

  printf(1, "\n-- 3c: P4 requests <3 3 0> (more than Available)\n");
  check("P4 <3 3 0> must wait", request_resources(4, req2) == 0);

  printf(1, "\n-- 3d: P0 requests <0 2 0> (available, but UNSAFE)\n");
  check("P0 <0 2 0> denied as unsafe", request_resources(0, req3) == 0);
  printf(1, "  state after the denied request (unchanged):\n");
  print_state(1);
  check("state still safe after rollback", is_safe_state() == 1);

  pipe_deadlock_demo(argc > 1);

  printf(1, "\ndeadlock: %s\n", failures == 0 ? "ALL CHECKS PASSED"
                                              : "SOME CHECKS FAILED");
  exit();
}
