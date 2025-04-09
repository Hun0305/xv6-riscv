#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#include "sleeplock.h"
#include "fs.h"
#include "file.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

struct mmap_area ma[64] = { 0, };

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  // find corresponding mmap_area
  for (int j = 0; j < 64; j++) {
    if (ma[j].p == p) {
      // find empty mmap_area
      for (int k = 0; k < 64; k++) {
        if (ma[k].addr == 0) {
          if (ma[j].f != 0) {
            ma[k].f = ma[j].f;
            filedup(ma[j].f); // increment f ref count
            ma[k].f->off = ma[j].offset;  
          }
          ma[k].addr = ma[j].addr;
          ma[k].flags = ma[j].flags & ~MAP_POPULATE;
          ma[k].length = ma[j].length;
          ma[k].p = np;
          ma[k].prot = ma[j].prot;
          ma[k].offset = ma[j].offset;

          pte_t *pte;
          uint64 flags;
          uint64 ptr = 0;
          void *new_physical_page;

          for(ptr = ma[k].addr; ptr < ma[k].addr + ma[k].length; ptr += PGSIZE) {
            if((pte = walk(p->pagetable, ptr, 0)) == 0) {
              panic("fork: pte doesn't exist\n");
              return -1;
            }
              
            // if(!((uint64)pte & PTE_V)) {
            //   panic("fork: pte is NOT PRESENTED\n");
            //   return -1;
            // }

            flags = PTE_FLAGS((uint64)pte);

            // in uvmcopy(), copying flags of PTE doesn't work well,
            // so copied flags of PTE is only PTE_V. so uvmunmap() serve it NOT LEAF.
            // below code is making right PTE flags.
            if (ma[k].prot & PROT_READ) {
              flags |= PTE_R;
            }
            if (ma[k].prot & PROT_WRITE) {
              flags |= PTE_W;
            }

            if((new_physical_page = kalloc()) == 0) {
              panic("fork: kalloc failed");
              return -1;
            }

            memset(new_physical_page, 0, PGSIZE);

            if(mappages(np->pagetable, ptr, PGSIZE, (uint64)new_physical_page, flags) == -1) {
              panic("fork: mappages failed");
              return -1;
            }
          }

          break;
        }
      }
    }
  }

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      intr_on();
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

uint64
mmap(uint64 addr, int length, int prot, int flags, int fd, int offset) {
  struct proc *p = myproc();
  struct file *f = 0;

  if (fd != -1)
    f = p->ofile[fd];

  int read = 0;
  int write = 0;

  if (prot & PROT_READ)
    read = 1;
  if (prot & PROT_WRITE)
    write = 1;
  
  int anonymous = 0;
  int populate = 0;

  if (flags & MAP_ANONYMOUS)
    anonymous = 1;
  if (flags & MAP_POPULATE)
    populate = 1;
  
  if (anonymous == 0 && fd == -1) {
    panic("mmap: It's not anonymous, but when the fd is -1\n");
    return 0;
  }

  if (f != 0 && (read != f->readable || write != f->writable)) {
    panic("mmap: The protection of the file and the prot of the parameter are different\n");
    return 0;
  }
  
  int i = 0;
  for (i = 0; (ma[i].addr != 0) && i < 64; i++);
  if (i == 64) {
    panic("mmap: There are no empty space in mmap_area\n");
    return 0;
  }

  uint64 start_addr = MMAPBASE + addr;
  if (start_addr >= MAXVA) {
    panic("mmap: too high addr");
    return 0;
  }

  ma[i].f = 0;
  if (f != 0) {
    // increment file reference count
    filedup(f);

    ma[i].f = f;
  }

  ma[i].addr = start_addr;
  ma[i].flags = flags;
  ma[i].length = length;
  ma[i].prot = prot;
  ma[i].p = p;
  ma[i].offset = offset;

  // make permission
  int perm = 0;
  if (read == 1)
    perm = perm | PTE_R;
  if (write == 1)
    perm = perm | PTE_W;

  // just record its mapping area
  if (populate == 0) {
    return start_addr;
  }

  else if (populate == 1) {
    // file mapping
    if (anonymous == 0) { 
      f->off = offset;
      uint64 ptr = 0; 
      void *new_physical_page = 0;

      for (ptr = start_addr; ptr < start_addr + length; ptr += PGSIZE) {
        new_physical_page = kalloc();
        // printf("[mmap] kalloc returned: %p\n", new_physical_page);
        // printf("[mmap] free_pages after kalloc: %d\n", freemem());
        if (new_physical_page == 0) {
          panic("mmap: Failed to allocate physical page in file mapping\n");
          return 0;
        }
        
        // initialize page with 0
        memset(new_physical_page, 0, PGSIZE);
        
        // 직접 readi를 호출하여 커널 메모리에 데이터를 복사
        ilock(f->ip);
        int r = readi(f->ip, 0, (uint64)new_physical_page, f->off, PGSIZE);
        if(r > 0)
          f->off += r;
        iunlock(f->ip);

        // printf("text[0] in kernel: %d\n", ((char*)new_physical_page)[0]);

        if (mappages(p->pagetable, ptr, PGSIZE, (uint64)new_physical_page, perm | PTE_U) == -1) {
          panic("mmap: Failed to mappages in file mapping\n");
          return 0;
        }     
      }

      return start_addr;
    }


    // anonymous mapping
    else if (anonymous == 1) { 
      uint64 ptr = 0; 
      void *new_physical_page = 0;

      for (ptr = start_addr; ptr < start_addr + length; ptr += PGSIZE) {
        new_physical_page = kalloc();

        // printf("[mmap] kalloc returned: %p\n", new_physical_page);
        
        if (new_physical_page == 0) {
          panic("mmap: Failed to allocate physical page in anonymous mapping\n");
          return 0;
        }
        
        // allocatae page fiiled with 0
        memset(new_physical_page, 0, PGSIZE);

        if (mappages(p->pagetable, ptr, PGSIZE, (uint64)new_physical_page, perm | PTE_U) == -1) {
          panic("mmap: Failed to mappages in anonymous mapping\n");
          return 0;
        } 
      }

      return start_addr;
    }
    
  }

  // if populate is not 0/1, return fail.
  return 0;
}


int
pfh(uint64 addr, uint64 err) {
  struct proc *p = myproc();

  int ma_idx = -1;

  for (int i = 0; i < 64; i++) {
    if ((ma[i].addr <= addr) && (addr <= (ma[i].addr + ma[i].length)) && (ma[i].p == p)) {
        ma_idx = i;
        break;
    }
  }

  // if (ma[ma_idx].flags & MAP_POPULATE) {
  //   printf("pfh: MAP_POPULATE is already set\n");
  //   return -1;
  // }

  if (ma_idx == -1) {
    panic("Page Fault: Corresponding mmap_area is not found\n");
    return -1;
  }

  // read prot check
  if ((err&0xff) == 13 && (ma[ma_idx].prot & PROT_READ) != PROT_READ) {
    panic("Page Fault: READ - prot in mmap_area are not same\n");
    return -1;
  }

  // write prot check
  if ((err&0xff) == 15 && (ma[ma_idx].prot & PROT_WRITE) != PROT_WRITE) {
    panic("Page Fault: WRITE - prot in mmap_area are not same\n");
    return -1;
  }

  // printf("read: %d, write: %d\n", read, write);
  // printf("ma_idx: %d, prot_read: %d, prot_write:%d\n", ma_idx, ma[ma_idx].prot & PROT_READ, ma[ma_idx].prot & PROT_WRITE);

  int anonymous = 0;
  if (ma[ma_idx].flags & MAP_ANONYMOUS)
    anonymous = 1;

  // make permission
  int perm = 0;
  if ((ma[ma_idx].prot & PROT_READ) == PROT_READ)
    perm = perm | PTE_R;
  if ((ma[ma_idx].prot & PROT_WRITE) == PROT_WRITE)
    perm = perm | PTE_W;

  // file mapping
  if (anonymous == 0) { 
    struct file *f = ma[ma_idx].f;
    f->off = ma[ma_idx].offset;

    uint64 ptr = 0; 
    void *new_physical_page = 0;

    for (ptr = ma[ma_idx].addr; ptr < ma[ma_idx].addr + ma[ma_idx].length; ptr += PGSIZE) {
      new_physical_page = kalloc();
      // printf("[pfh] kalloc returned: %p\n", new_physical_page);
      if (new_physical_page == 0) {
        panic("mmap: Failed to allocate physical page in file mapping\n");
        return -1;
      }
      
      // initialize page with 0
      memset(new_physical_page, 0, PGSIZE);
      
      // 직접 readi를 호출하여 커널 메모리에 데이터를 복사
      ilock(f->ip);
      int r = readi(f->ip, 0, (uint64)new_physical_page, f->off, PGSIZE);
      if(r > 0)
        f->off += r;
      iunlock(f->ip);

      if (mappages(p->pagetable, ptr, PGSIZE, (uint64)new_physical_page, perm | PTE_U) == -1) {
        panic("mmap: Failed to mappages in file mapping\n");
        return -1;
      }     
    }

    ma[ma_idx].flags |= MAP_POPULATE;

    return 0;
  }


  // anonymous mapping
  else if (anonymous == 1) { 
    uint64 ptr = 0; 
    void *new_physical_page = 0;

    for (ptr = ma[ma_idx].addr; ptr < ma[ma_idx].addr + ma[ma_idx].length; ptr += PGSIZE) {
      new_physical_page = kalloc();
      // printf("[pfh] kalloc returned: %p\n", new_physical_page);
      if (new_physical_page == 0) {
        panic("mmap: Failed to allocate physical page in anonymous mapping\n");
        return 0;
      }
      
      // allocatae page fiiled with 0
      memset(new_physical_page, 0, PGSIZE);

      if (mappages(p->pagetable, ptr, PGSIZE, (uint64)new_physical_page, perm | PTE_U) == -1) {
        panic("mmap: Failed to mappages in anonymous mapping\n");
        return -1;
      } 
    }

    ma[ma_idx].flags |= MAP_POPULATE;

    return 0;
  }

  panic("Page Fault: unexpected error\n");
  return -1;
}

int
munmap(uint64 addr) {
  struct proc *p = myproc();
  int ma_idx = -1;
 
  for (int i = 0; i < 64; i++) {
    if (ma[i].addr == addr && ma[i].p == p) {
      ma_idx = i;
      break;
    }
  }
  
  if (ma_idx == -1) {
    panic("munmap: Corresponding mmap_area is not found\n");
    return -1;
  }
  
  if (ma[ma_idx].flags & MAP_POPULATE) {
    // length가 PGSIZE의 배수라고 가정
    int npages = ma[ma_idx].length / PGSIZE;
    // uvmunmap()을 이용하여 해당 영역의 매핑을 해제하면서 물리 페이지도 free하도록 한다.
    uvmunmap(p->pagetable, ma[ma_idx].addr, npages, 1);
  
    // mmap 영역의 정보를 초기화한다.
    ma[ma_idx].f = 0;
    ma[ma_idx].addr = 0;
    ma[ma_idx].length = 0;
    ma[ma_idx].offset = 0;
    ma[ma_idx].prot = 0;
    ma[ma_idx].flags = 0;
    ma[ma_idx].p = 0;
  } else {
    set_free_pages();
  }
 
  return 1;
}


int
freemem() {
  return get_free_pages();
}