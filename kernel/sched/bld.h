#ifdef CONFIG_BLD

static DEFINE_RWLOCK(disp_list_lock);
static LIST_HEAD(rq_head);

static inline int select_cpu_for_wakeup(struct task_struct *p, int sd_flags, int wake_flags)
{
	int cpu = smp_processor_id(), prev_cpu = task_cpu(p), i;
	/*bool sync = wake_flags & WF_SYNC; */
	unsigned long load, min_load = ULONG_MAX;
	struct cpumask *mask;

	if (wake_flags & WF_SYNC) {
		if (cpu == prev_cpu)
			return cpu;
		mask = sched_group_cpus(cpu_rq(prev_cpu)->sd->groups);
	} else
		mask = sched_domain_span(cpu_rq(prev_cpu)->sd);

	for_each_cpu(i, mask) {
		load = cpu_rq(i)->load.weight;
		if (load < min_load) {
			min_load = load;
			cpu = i;
		}
	}
	return cpu;
}

static int bld_select_task_rq(struct task_struct *p, int sd_flags, int wake_flags)
{
	struct rq *tmp, *rq;
	unsigned long flag;
	unsigned int cpu = smp_processor_id();

	if (&p->cpus_allowed) {
		struct cpumask *taskmask;
		unsigned long min_load = ULONG_MAX, load, i;
		taskmask = tsk_cpus_allowed(p);
		for_each_cpu(i, taskmask) {
			load = cpu_rq(i)->load.weight;
			if (load < min_load) {
				min_load = load;
				cpu = i;
			}
		}
	} else	if (sd_flags & SD_BALANCE_WAKE) {
		cpu = select_cpu_for_wakeup(p, sd_flags, wake_flags);
		return cpu;
	} else {
		read_lock_irq(&disp_list_lock);
		list_for_each_entry_safe(rq, tmp, &rq_head, disp_load_balance) {
			cpu = cpu_of(rq);
			if (cpu_online(cpu))
				break;
		}
		read_unlock_irq(&disp_list_lock);
	}
	return cpu;
}

static void bld_track_load_activate(struct rq *rq)
{
	unsigned long  flag;

	if (rq->pos != 2) {	/* if rq isn't the last one */
		struct rq *last;
		last = list_entry(rq_head.prev, struct rq, disp_load_balance);
		if (rq->load.weight > last->load.weight) {
			write_lock_irqsave(&disp_list_lock, flag);
			list_del(&rq->disp_load_balance);
			list_add_tail(&rq->disp_load_balance, &rq_head);
			rq->pos = 2; last->pos = 1;
			write_unlock_irqrestore(&disp_list_lock, flag);
		}
	}
}

static void bld_track_load_deactivate(struct rq *rq)
{
	unsigned long flag;

	if (rq->pos != 0) { /* If rq isn't first one */
		struct rq *first;
		first = list_first_entry(&rq_head, struct rq, disp_load_balance);
		if (rq->load.weight <= first->load.weight) {
			write_lock_irqsave(&disp_list_lock, flag);
			list_del(&rq->disp_load_balance);
			list_add_tail(&rq->disp_load_balance, &rq_head);
			rq->pos = 0; first->pos = 1;
			write_unlock_irqrestore(&disp_list_lock, flag);
		}
	}
}
#else
static inline void bld_track_load_activate(struct rq *rq)
{
}

static inline void bld_track_load_deactivate(struct rq *rq)
{
}
#endif /* CONFIG_BLD */
