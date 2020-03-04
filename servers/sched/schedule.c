/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "kernel/proc.h" /* for queue constants */
#include <stdlib.h>  // For Random function
#include <stdio.h>

static timer_t sched_timer;
static unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

static int schedule_process(struct schedproc * rmp, unsigned flags);
static void balance_queues(struct timer *tp);

#define SCHEDULE_CHANGE_PRIO	0x1
#define SCHEDULE_CHANGE_QUANTUM	0x2
#define SCHEDULE_CHANGE_CPU	0x4

#define SCHEDULE_CHANGE_ALL	(	\
		SCHEDULE_CHANGE_PRIO	|	\
		SCHEDULE_CHANGE_QUANTUM	|	\
		SCHEDULE_CHANGE_CPU		\
		)

#define schedule_process_local(p)	\
	schedule_process(p, SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)
#define schedule_process_migrate(p)	\
	schedule_process(p, SCHEDULE_CHANGE_CPU)

#define CPU_DEAD	-1

#define cpu_is_available(c)	(cpu_proc[c] >= 0)

#define DEFAULT_USER_TIME_SLICE 200

/* processes created by RS are sysytem processes */
#define is_system_proc(p)	((p)->parent == RS_PROC_NR)

#define FCFS_SCHED 377
#define RAND_SCHED 577
#define LOTT_SCHED 677

static unsigned scheduler_algo = RAND_SCHED;  // Default random scheduler

static unsigned cpu_proc[CONFIG_MAX_CPUS];

static void pick_cpu(struct schedproc * proc)
{
#ifdef CONFIG_SMP
	unsigned cpu, c;
	unsigned cpu_load = (unsigned) -1;
	
	if (machine.processors_count == 1) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* schedule sysytem processes only on the boot cpu */
	if (is_system_proc(proc)) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* if no other cpu available, try BSP */
	cpu = machine.bsp_id;
	for (c = 0; c < machine.processors_count; c++) {
		/* skip dead cpus */
		if (!cpu_is_available(c))
			continue;
		if (c != machine.bsp_id && cpu_load > cpu_proc[c]) {
			cpu_load = cpu_proc[c];
			cpu = c;
		}
	}
	proc->cpu = cpu;
	cpu_proc[cpu]++;
#else
	proc->cpu = 0;
#endif
}

/*===========================================================================*
*				577 Edit utils				     *
*===========================================================================*/
unsigned idcounter = 0;  // id for processes to indicate who came in first
int is_premeptive(){
	/**
	 * @brief  if the scheduler is premeptive
	 * @note   
	 * @retval 1 if is
	 */
	if (scheduler_algo == LOTT_SCHED) return TRUE;
	if (scheduler_algo == FCFS_SCHED || scheduler_algo == RAND_SCHED) return FALSE;
	return -1;
}


/*===========================================================================*
*				577 Edit FCFS policy (unused)				     *
*===========================================================================*/
int fcfs_algorithm(){
    /**
     * @brief  Move the least recent process to lower priority
     * @note   
     * @retval Status of the operation
     */
    struct schedproc *rmp;
    int proc_nr;
    unsigned next_id = 1000000;

	// Loop through all process in process array and get the lowest id
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if ((rmp->flags & IN_USE)
                && rmp->priority == MIN_USER_Q && rmp->id<next_id) {
            next_id = rmp->id;
        }
    }

	// put the lowest id to MAX Q
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if ((rmp->flags & IN_USE)
                && rmp->id==next_id) {
            rmp->priority = MAX_USER_Q;
            schedule_process_local(rmp);
            break;
        }
    }
    return OK;
}

/*===========================================================================*
*				577 Edit random policy				     *
*===========================================================================*/

int random_algorithm(){
    /**
     * @brief  Move a random process to lower priority
     * @note   
     * @retval Status of operation
     */
	// printf("random_algorithm: starting\n");
    struct schedproc *rmp;
    int proc_nr;
	int process_count = 0;

	// Add all process ids to array
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if ((rmp->flags & IN_USE)
                && rmp->priority == MIN_USER_Q) {
            // all_procs[array_end++] = rmp->id;
			process_count++;
        }
    }

	// If no process
	if (process_count == 0) {
		// printf("random_algorithm: no process in use.\n");
		return OK;
	}

	// if (next == 1) {
	// 	int seed = time(NULL);
	// 	printf("random_algorithm: seeding with %d.\n", seed);
	// 	srandom(seed);
	// }

	// printf("random_algorithm: process count: %d\n", process_count);

	int process_to_raise = random() % process_count;

	// printf("random_algorithm: process index to raise: %d\n", process_to_raise);
	// put the random id to MAX Q
	int process_idx = 0;
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if ((rmp->flags & IN_USE) && rmp->priority == MIN_USER_Q) {
			if (process_idx == process_to_raise) {
				int procid = rmp->id;
				rmp->priority = MAX_USER_Q;
				schedule_process_local(rmp);
				break;
			}
			process_idx++;
        }
    }
    return OK;
}

/*===========================================================================*
*				577 Edit lottery policy				     *
*===========================================================================*/

int lottery_algorithm(){
    /**
     * @brief  Assign priority based on tickets
     * @note   
     * @retval Status of operation
     */
	// printf("lottery_algorithm: starting.\n");
    struct schedproc *rmp;
    int proc_nr;
	int total_tickets = 0;  // Holds the total amount of tickets of all processes

	// Get total amount of tickets
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if ((rmp->flags & IN_USE)
                && rmp->priority == MIN_USER_Q) {if (scheduler_algo == LOTT_SCHED)
            total_tickets += rmp->tickets;
        }
    }

	if (total_tickets < 1) return OK;  // When there is no ticket

	// if (next == 1) {
	// 	int seed = time(NULL);
	// 	printf("lottery_algorithm: seeding with %d.\n", seed);
	// 	srandom(seed);
	// }

	int ticket_c = random() % total_tickets;
	// printf("lottery_algorithm: By %d,  %d / %d ticket is selected.\n", rando_nr, ticket_c, total_tickets);
	
	int next_id = -1;
	// Distribute the tickets
	for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if ((rmp->flags & IN_USE)
                && rmp->priority == MIN_USER_Q) {
            ticket_c -= rmp->tickets;
        }
		if (ticket_c <= 0) {
			next_id = rmp->id;
			break;
		}
    }

	if (next_id == -1) {
		// printf("lottery_algorithm: No process chosen.\n");
		return OK;
	}

	// printf("lottery_algorithm: selecting process %d to prio 14.\n", next_id);

	// put the winners id to MAX Q
    for (proc_nr = 0, rmp = schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if ((rmp->flags & IN_USE)
                && rmp->id==next_id) {
            rmp->priority = MAX_USER_Q;
            schedule_process_local(rmp);
            break;
        }
    }
    return OK;
}

/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

int do_noquantum(message *m_ptr)
{
	// printf("do_noquantum: starting\n");
	register struct schedproc *rmp;
	int rv, proc_nr_n;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
		m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	// original
	// if (rmp->priority < MIN_USER_Q) {
	// 	rmp->priority += 1; /* lower priority */
	// }

	// if ((rv = schedule_process_local(rmp)) != OK) {
	// 	return rv;
	// }

	// 577 edit start
	// if (is_system_proc(rmp)) {
	// 	// printf("do_noquantum: system process, continue.\n");
	// 	schedule_process_local(rmp);  // Continue to run it
	// 	return OK;  // Skip system processes
	// }

	printf("do_noquantum: Process %d finished Q and was in queue %d.\n", rmp->id,rmp->priority);

	// if (rmp->priority < MIN_USER_Q) {  // If the priority is higher than 15
	// 	printf("do_noquantum: high priority proc, continue");
	// 	rmp->priority += 0; /* not lower priority for non preemtive 577 edit*/
	// 	schedule_process_local(rmp);  // Continue to run it; not continue for lottery
	// 	return OK;
	// }
	if (is_premeptive() == FALSE){
		printf("do_noquantum: using non-premeptive scheduler. no other process should run before this process is finished.\n");
		rmp->priority = MAX_USER_Q;
		schedule_process_local(rmp);  // Continue to run it
	} else if (is_premeptive() == TRUE) {
		// reset process queue
		rmp->priority= MIN_USER_Q;

		if (scheduler_algo == FCFS_SCHED) fcfs_algorithm();
		else if (scheduler_algo == RAND_SCHED) random_algorithm();
		else if (scheduler_algo == LOTT_SCHED) lottery_algorithm();
	}
	
	return OK;
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
int do_stop_scheduling(message *m_ptr)
{
	// printf("do_stop_scheduling: starting\n");
	register struct schedproc *rmp;
	int proc_nr_n;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
#ifdef CONFIG_SMP
	cpu_proc[rmp->cpu]--;
#endif
	rmp->flags = 0; /*&= ~IN_USE;*/

	//577 edit start
	// When a process stopped, we want to schedule another one
	if (scheduler_algo == FCFS_SCHED) fcfs_algorithm();
	else if (scheduler_algo == RAND_SCHED) random_algorithm();
	else if (scheduler_algo == LOTT_SCHED) lottery_algorithm();
	//577 edit end
	return OK;
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
int do_start_scheduling(message *m_ptr)
{
	// printf("do_start_scheduling: starting\n");
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n;
	
	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START || 
		m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n))
			!= OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* Populate process slot */
	rmp->endpoint     = m_ptr->SCHEDULING_ENDPOINT;
	rmp->parent       = m_ptr->SCHEDULING_PARENT;
	rmp->max_priority = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}
	// 577 edit start
	rmp->id = ++idcounter;
	rmp->tickets = 2;  // All process have 2 tickets at start
	// printf("do_start_scheduling: starting scheduling for process %d.\n", rmp->id);
	// 577 edit end

	/* Inherit current priority and time slice from parent. Since there
	 * is currently only one scheduler scheduling the whole system, this
	 * value is local and we assert that the parent endpoint is valid */
	if (rmp->endpoint == rmp->parent) {
		/* We have a special case here for init, which is the first
		   process scheduled, and the parent of itself. */
		rmp->priority   = USER_Q;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;

		/*
		 * Since kernel never changes the cpu of a process, all are
		 * started on the BSP and the userspace scheduling hasn't
		 * changed that yet either, we can be sure that BSP is the
		 * processor where the processes run now.
		 */
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
		/* FIXME set the cpu mask */
#endif
	}
	
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
		rmp->priority   = rmp->max_priority;
		rmp->time_slice = (unsigned) m_ptr->SCHEDULING_QUANTUM;
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
				&parent_nr_n)) != OK)
			return rv;

		// 577 edit start
		rmp->priority = USER_Q; //schedproc[parent_nr_n].priority;
		// 577 edit end
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	pick_cpu(rmp);
	while ((rv = schedule_process(rmp, SCHEDULE_CHANGE_ALL)) == EBADCPU) {
		/* don't try this CPU ever again */
		cpu_proc[rmp->cpu] = CPU_DEAD;
		pick_cpu(rmp);
	}

	if (rv != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into SCHEDULING_SCHEDULER
	 */

	m_ptr->SCHEDULING_SCHEDULER = SCHED_PROC_NR;

	return OK;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
int do_nice(message *m_ptr)
{
	// printf("do_nice: starting\n");
	struct schedproc *rmp;
	int rv;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	new_q = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	if (new_q >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Store old values, in case we need to roll back the changes */
	old_q     = rmp->priority;
	old_max_q = rmp->max_priority;

	/* Update the proc entry and reschedule the process */
	rmp->max_priority = rmp->priority = new_q;

	if ((rv = schedule_process_local(rmp)) != OK) {
		/* Something went wrong when rescheduling the process, roll
		 * back the changes to proc struct */
		rmp->priority     = old_q;
		rmp->max_priority = old_max_q;
	}

	return rv;
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
static int schedule_process(struct schedproc * rmp, unsigned flags)
{
	// printf("schedule_process: scheduling %d\n", rmp->id);
	int err;
	int new_prio, new_quantum, new_cpu;

	pick_cpu(rmp);

	if (flags & SCHEDULE_CHANGE_PRIO)
		new_prio = rmp->priority;
	else
		new_prio = -1;

	if (flags & SCHEDULE_CHANGE_QUANTUM)
		new_quantum = rmp->time_slice;
	else
		new_quantum = -1;

	if (flags & SCHEDULE_CHANGE_CPU)
		new_cpu = rmp->cpu;
	else
		new_cpu = -1;

	if ((err = sys_schedule(rmp->endpoint, new_prio,
		new_quantum, new_cpu)) != OK) {
		printf("PM: An error occurred when trying to schedule %d: %d\n",
		rmp->endpoint, err);
	}

	return err;
}


/*===========================================================================*
 *				start_scheduling			     *
 *===========================================================================*/

void init_scheduling(void)
{
	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	init_timer(&sched_timer);
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}

/*===========================================================================*
 *				balance_queues				     *
 *===========================================================================*/

/* This function in called every 100 ticks to rebalance the queues. The current
 * scheduler bumps processes down one priority when ever they run out of
 * quantum. This function will find all proccesses that have been bumped down,
 * and pulls them back up. This default policy will soon be changed.
 */
static void balance_queues(struct timer *tp)
{
	// printf("balance_queues: starting\n");
	struct schedproc *rmp;
	int proc_nr;

	// for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
	// 	if (rmp->flags & IN_USE) {
	// 		if (rmp->priority > rmp->max_priority) {
	// 			rmp->priority -= 1; /* increase priority */
	// 			schedule_process_local(rmp);
	// 		}
	// 	}
	// }

	// 577 edit start
	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			if (rmp->priority > rmp->max_priority) {
				rmp->priority -= 0; /* no tamper priority */
				schedule_process_local(rmp);
			}
		}
	}
	// 577 edit end

	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}
