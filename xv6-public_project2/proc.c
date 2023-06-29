#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "elf.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

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

  // 부모 프로세스의 memory limit과 stacksize를 복사함
  np->mlimit = curproc->mlimit;
  np->stacksize = curproc->stacksize;

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

  // 프로세스의 mainThread는 자신으로 설정
  np->mainThread = np;

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
  // 종료하려는 프로세스가 Thread일 경우, thread_exit으로 종료
  if(curproc->isThread == 1){
    thread_exit(0);
  }
  // 프로세스일 경우
  else{
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

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
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
  struct proc *curproc = myproc();
  // 스레드일 경우
  if(curproc->isThread == 1){
    return thread_join(curproc->tid, (void **)&(curproc->retval));  
  }

  // 프로세스일 경우
  struct proc *p;
  int havekids, pid;
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
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

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.

// 스레드는 결국 하나의 프로세스기 때문에 스케줄러는 그대로 이용
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

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
  int cnt = 0;
  acquire(&ptable.lock);
  // 모든 스레드가 종료될 수 있도록 ptable을 순회하며 kill
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      // 모든 스레드가 종료될 수 있도록 하는 변수
      cnt++;
    }
  }
  release(&ptable.lock);
  if(cnt == 0){
    return -1;
  }
  return 0;
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

// 프로세스의 스텍페이지를 지정해서 할당해주는 함수
int
exec2(char *path, char **argv, int stacksize)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();

  // 파일 시스템에 대한 잠금
  begin_op();
  // namei : path를 통해 실행 파일을 찾아서 반환
  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  // 찾은 파일을 잠금해서 다른 프로세스가 해당 파일을 변경하지 못하게 함
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  // 실행 파일의 헤더를 읽어옴
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;
  // 새로운 프로세스를 위한 페이지 디렉토리 설정
  if((pgdir = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  // 파일 및 파일 시스템 잠금 해제
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.

  // 페이지 크기로 맞춰주는 작업
  sz = PGROUNDUP(sz);
  
  // stack page(stack size) + guard page(1) 만큼 할당
  if((sz = allocuvm(pgdir, sz, sz + (stacksize + 1)*PGSIZE)) == 0)
    goto bad;

  // 이 주소를 가리키는 페이지 테이블 엔트리를 초기화하는 작업 (guard page에 접근할 수 없도록)
  clearpteu(pgdir, (char*)(sz - (stacksize + 1)*PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;  // main
  curproc->tf->esp = sp;
  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

 bad:
  cprintf("2\n");
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// wrapper function
int 
sys_exec2(void){

  char *path, *argv[MAXARG];
  int stacksize, i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0 || argint(2, &stacksize)){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  
  return exec2(path, argv, stacksize);
}


// 메모리 크기를 제한하는 함수
int 
setmemorylimit(int pid, int limit)
{
  struct proc *p;
  
  // 프로세스 탐색
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->pid == pid) break;
  }

  // 조건에 맞지 않으면 -1 반환
  if(p->pid != pid || limit < p->sz) return -1;
  
  // mlimit 변수 변경
  acquire(&ptable.lock); 
  p->mlimit = limit;
  release(&ptable.lock);
  return 0;
}

// wrapper function
int
sys_setmemorylimit(void)
{
  int pid, limit;

  if(argint(0, &pid) < 0 || argint(1, &limit) < 0){
    return -1;
  }
  return setmemorylimit(pid, limit);
}

// 현재 실행중인 프로세스들의 정보를 출력하는 함수
void
processInfo(void)
{
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNING || p->state == RUNNABLE || p->state == SLEEPING){
      cprintf("process name : %s, pid : %d, stacksize : %d, memory size : %d, memory limitation : %d\n", 
      p->name, p->pid, p->stacksize, p->sz, p->mlimit);
    } 
  }
}

// wrapper function
int 
sys_processInfo(void){
  processInfo();
  return 0;
}


// Thread
// 스레드를 생성하는 함수
int 
thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg)
{

  int i;
  uint sp, ustack[2];
  struct proc *np;
  struct proc *curproc = myproc();
  
  // 새 프로세스 할당
  if((np = allocproc()) == 0){
    return -1;
  }

  acquire(&ptable.lock);

  // 현재 프로세스 페이지 디렉토리 복사  
  np->pgdir = curproc->pgdir;
  
  // 스레드를 위핸 페이지 추가 할당 (stack page : 1 / guard page : 1)
  if((curproc->sz=allocuvm(np->pgdir,curproc->sz,curproc->sz+2*PGSIZE))==0){
    return -1;
  }

  // 추가 할당한 페이지 테이블 엔트리 초기화
  clearpteu(np->pgdir, (char*)(curproc->sz - 2*PGSIZE));

  np->sz = curproc->sz;
  sp = np->sz;
  
  // 스택 상단엔 반환주소 입력
  ustack[0] = 0xffffffff;
  // 다음 위치에 인자 저장
  ustack[1] = (uint)arg; 
  sp -= 8;

  // 스택 복사
  if(copyout(np->pgdir, sp, ustack, 8) < 0)
    return -1;

  // 같은 프로세스 내에 있는 모든 스레드에 대한 sz 동기화
  struct proc* p;
  for(p=ptable.proc;p<&ptable.proc[NPROC];p++){
    if(p->parent->pid==np->parent->pid){
      p->sz=np->sz;
    }
  }

  // 현재 프로세스의 파일 디렉토리 복사
  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  // 현재 프로세스의 이름 복사
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));
  

  // 스레드 설정
  np->pid = curproc->pid;
  np->parent = curproc;
  np->mainThread = curproc->mainThread;
  
  np->isThread = 1;
  np->tid = nexttid++;
  *thread = np->tid;
  
  *np->tf = *curproc->tf;

  // 반환값, 시작주소, 스택프레임 레지스터 설정
  np->tf->eax = 0;
  np->tf->eip = (uint)start_routine;
  np->tf->esp = sp;
    
    
// switchuvm(np)의 일부 deadlock을 방지하기 위함(중복 락)
  pushcli();
  lcr3(V2P(np->pgdir));
  popcli();

  np->state = RUNNABLE;
  release(&ptable.lock);

  return 0;
};

// wrapper funcion
int
sys_thread_create(void)
{
  thread_t *thread;
  void *(*start_routine)(void *);
  void *arg;

  if (argptr(0, (void *)&thread, sizeof(*thread)) < 0 || argptr(1, (void *)&start_routine, sizeof(*start_routine)) < 0 || argptr(2, (void *)&arg, sizeof(*arg)) < 0){
    return -1;
  }

  return thread_create(thread, start_routine, arg);
}


// 스레드를 종료하는 함수
void 
thread_exit(void *retval)
{ 
  struct proc *curproc = myproc();
  int fd;


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

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);


  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;

  // 인자로 받은 반환값 지정
  curproc->retval = retval;

  sched();
  panic("zombie exit");
}

// wrapper function
int
sys_thread_exit(void)
{
  void *retval;

  if (argptr(0, (void *)&retval, sizeof(*retval)) < 0){
    return -1;
  }

  thread_exit(retval);
  return 0;
}

// 해당 스레드가 종료될 때까지 대기하는 함수
int 
thread_join(thread_t thread, void **retval)
{
  struct proc *p;
  struct proc *curproc = myproc();
  

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->tid != thread || p->mainThread != curproc->mainThread)
        continue;
      if(p->state == ZOMBIE){
        // Found one.
        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;


        p->mainThread = 0;
        p->tid = 0;
        *retval = p->retval;
        release(&ptable.lock);
        return 0;
      }
    }
    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
  return -1;
}

// wrapper function
int
sys_thread_join(void)
{
  thread_t thread;
  void **retval;

  // Read arguments from the user stack
  if (argint(0, (int *)&thread) < 0 || argptr(1, (void *)&retval, sizeof(*retval)) < 0){
    return -1;
  }

  return thread_join(thread, retval);
}


// 프로세스에 있는 스레드를 모두 종료시키는 함수. exec 함수에서 이용
void
killThreads(void)
{
  struct proc *curproc = myproc();
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    // 같은 프로세스 내에 있고, 실행중인 스레드가 아니면 자원 해제
    if(p->pid == curproc->pid && p != curproc){
      if(p->kstack != 0){
        kfree(p->kstack);
      }
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        p->tid = 0;
        p->isThread = 0;
        p->mainThread = 0; 
    }
  }   
}

// wrapper function
int
sys_killThreads(void)
{
  killThreads();
  return 0;
}