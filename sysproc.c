#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

extern struct spinlock wait_lock;
extern struct proc proc[NPROC];
extern int getlevel(void);
extern int getmlfqinfo(int pid, struct mlfqinfo *info);

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  // t=0 means lazy (default), t=1 means eager
  if(t == SBRK_EAGER || n < 0){    // SBRK_EAGER=0, so only n<0 triggers eager
    if(growproc(n) < 0)
      return -1;
  } else {
    if(addr + n < addr)        return -1;
    if(addr + n > TRAPFRAME)   return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
uint64
sys_hello(void)
{
  printf("Hello from the kernel!\n");
  return 0;
}
uint64
sys_getpid2(void)
{
  struct proc *p = myproc();
  return p->pid;
}
uint64
sys_getppid(void)
{
  struct proc *p = myproc();

  if(p->parent == 0)
    return -1;

  return p->parent->pid;
}
uint64
sys_getnumchild(void)
{
  struct proc *p = myproc();
  struct proc *pp;
  int count = 0;

  acquire(&wait_lock);

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p && pp->state != ZOMBIE && pp->state != UNUSED){
      count++;
    }
  }

  release(&wait_lock);

  return count;
}
uint64
sys_getsyscount(void)
{
  struct proc *p = myproc();
  return p->syscount;
}
uint64
sys_getchildsyscount(void)
{
  int pid;
  struct proc *p = myproc();
  struct proc *pp;

  argint(0, &pid);


  if(pid == p->pid)
    return -1;

  acquire(&wait_lock);

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->pid == pid){
      if(pp->parent != p){
        release(&wait_lock);
        return -1;
      }
      int c = pp->syscount;
      release(&wait_lock);
      return c;
    }
  }

  release(&wait_lock);
  return -1;
}

uint64
sys_getlevel(void)
{
  return getlevel();
}
uint64
sys_getmlfqinfo(void)
{
  int pid;
  uint64 addr;
  
  argint(0, &pid);
  argaddr(1, &addr);

  struct mlfqinfo info;

  if(getmlfqinfo(pid, &info) < 0)
    return -1;

  if(copyout(myproc()->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;

  return 0;
}
uint64
sys_getvmstats(void)
{
  int pid;
  uint64 addr;

  argint(0, &pid);
  argaddr(1, &addr);

  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->pid == pid){
      struct vmstats info;

      info.page_faults = p->page_faults;
      info.pages_evicted = p->pages_evicted;
      info.pages_swapped_in = p->pages_swapped_in;
      info.pages_swapped_out = p->pages_swapped_out;
      info.resident_pages = p->resident_pages;

      copyout(myproc()->pagetable, addr, (char*)&info, sizeof(info));
      return 0;
    }
  }

  return -1;
}
uint64
sys_setdisksched(void)
{
  int policy;

  argint(0, &policy);

  if(policy != 0 && policy != 1)
    return -1;
  extern int disk_policy;
  disk_policy = policy;   

  return 0;
}