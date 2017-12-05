#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}




void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
	struct proc *curproc = myproc();
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  case T_PGFLT: // For CS153 lab2 part1
		cprintf("page fault\n");
		if(curproc && (tf->cs&3) == DPL_USER){ // user mode
			if(curproc->tf->esp <= curproc->tstack){ // Check to see if stack size matches esp and if it doesn't it makes it the same
				uint rep = ((curproc->tstack - curproc->tf->esp)/PGSIZE)+1;
				cprintf("stack required to be allocated: %d\n", rep);
				if(curproc->sz+2*PGSIZE > curproc->tf->esp){ // Checks for the garbage so that if the esp reaches we delete until it is no longer there.
					cprintf("guard page reached! esp 0x%x stack 0x%x sz 0x%x addr 0x%x\n", curproc->tf->esp, curproc->tstack, curproc->sz, rcr2());
					curproc->killed = 1;
					break;
				}
				if(addstackpage(curproc->pgdir, curproc->tstack, rep) == 0){
		    	cprintf("allocation error! esp 0x%x stack 0x%x sz 0x%x addr 0x%x\n", curproc->tf->esp, curproc->tstack, curproc->sz, rcr2());
					curproc->killed = 1;
					break;
				}
				cprintf("current esp 0x%x\n",curproc->tf->esp);
				cprintf("current tstack 0x%x\n",curproc->tstack);
				break;
			}
		  cprintf("Access forbidden! esp 0x%x stack 0x%x sz 0x%x addr 0x%x\n", curproc->tf->esp, curproc->tstack, curproc->sz, rcr2());
			curproc->killed = 1;
			break;
		}else if(curproc){ // kernel mode
			cprintf("Stack Owerflow in proccess pid %d %s: trap %d err %d on cpu %d "
						 "eip 0x%x addr 0x%x--kill proc\n",
						  curproc->pid, curproc->name, tf->trapno, tf->err, cpuid(), tf->eip, 
						  rcr2());                                          
			curproc->killed = 1;
			break;
    }

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
