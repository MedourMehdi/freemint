/*
 * This file has been modified as part of the FreeMiNT project. See
 * the file Changes.MH for details and dates.
 */

# ifndef _proc_h
# define _proc_h

# include "mint/mint.h"
# include "mint/proc.h"


extern ushort proc_clock;		/* timeslices */
extern short time_slice;
extern struct proc_queue sysq[NUM_QUEUES];

/* macro for calculating number of missed time slices, based on a
 * process' priority
 */
# define SLICES(pri)	(((pri) >= 0) ? 0 : -(pri))

# define TICKS_PER_TOCK		200
# define TOCKS_PER_SECOND	1

# define SAMPS_PER_MIN		12
# define SAMPS_PER_5MIN		SAMPS_PER_MIN * 5
# define SAMPS_PER_15MIN	SAMPS_PER_MIN * 15

# define LOAD_SCALE		2048

extern ulong uptime;
extern ulong avenrun[3];
extern ushort uptimetick;


void		init_proc	(void);

void		reset_priorities(void);
void		run_next	(struct proc *p, int slices);
void		fresh_slices	(int slices);

void		add_q		(int que, struct proc *proc);
void		rm_q		(int que, struct proc *proc);

void	_cdecl	preempt		(void);
int	_cdecl	sleep		(int que, long cond);
void	_cdecl	wake		(int que, long cond);
void	_cdecl	iwake		(int que, long cond, short pid);
void	_cdecl	wakeselect	(struct proc *p);

void		DUMPPROC	(void);
void		calc_load_average (void);

ulong	_cdecl	remaining_proc_time (void);

struct proc *_cdecl get_curproc(void);

int tas(volatile long *lock);
void switch_to_thread(struct proc *from, struct proc *to);
void schedule(void);
void add_to_wait_queue(struct proc **queue, struct proc *p);
void remove_from_wait_queue(struct proc **queue, struct proc *p);
void add_to_ready_queue(struct proc *p);
void remove_from_ready_queue(struct proc *p);
struct proc* remove_highest_priority(struct proc **queue);
void th_sleep(void);
void wakeup(struct proc *p);
extern void timer_interrupt_handler(void);
void* allocate_thread_stack(void);
void free_thread_stack(void *stack);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
void mutex_init(struct mutex *m);
void semaphore_init(struct semaphore *s, int count);
void init_thread_stack(struct thread *t, void (*entry)(void*), void *arg);

# endif /* _proc_h */
