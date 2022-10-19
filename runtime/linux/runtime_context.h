/* -*- linux-c -*- 
 * Context Runtime Functions
 * Copyright (C) 2014 Red Hat Inc.
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 */

#ifndef _LINUX_RUNTIME_CONTEXT_H_
#define _LINUX_RUNTIME_CONTEXT_H_

/* Can't use a lock primitive for this because lock_acquire() has tracepoints */
static atomic_t _stp_contexts_busy_ctr = ATOMIC_INIT(0);
static DEFINE_PER_CPU(struct context *, contexts);

static int _stp_runtime_contexts_alloc(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		/* Module init, so in user context, safe to use
		 * "sleeping" allocation. */
		struct context *c = _stp_vzalloc_node(sizeof (struct context),
						      cpu_to_node(cpu));
		if (c == NULL) {
			_stp_error ("context (size %lu per cpu) allocation failed",
				    (unsigned long) sizeof (struct context));
			return -ENOMEM;
		}
		per_cpu(contexts, cpu) = c;
	}
	return 0;
}

static bool _stp_runtime_context_trylock(void)
{
	bool locked;

	preempt_disable();
	locked = atomic_add_unless(&_stp_contexts_busy_ctr, 1, INT_MAX);
	if (!locked)
		preempt_enable_no_resched();

	return locked;
}

static void _stp_runtime_context_unlock(void)
{
	atomic_dec(&_stp_contexts_busy_ctr);
	preempt_enable_no_resched();
}

/* We should be free of all probes by this time, but for example the timer for
 * _stp_ctl_work_callback may still be running and looking for contexts.  We
 * use _stp_contexts_busy_ctr to be sure its safe to free them.  */
static void _stp_runtime_contexts_free(void)
{
	unsigned int cpu;

	/* Sync to make sure existing readers are done */
	while (atomic_cmpxchg(&_stp_contexts_busy_ctr, 0, INT_MAX))
		cpu_relax();

	/* Now we can actually free the contexts */
	for_each_possible_cpu(cpu)
		_stp_vfree(per_cpu(contexts, cpu));
}

static inline struct context * _stp_runtime_get_context(void)
{
	return per_cpu(contexts, smp_processor_id());
}

static struct context * _stp_runtime_entryfn_get_context(void)
{
	struct context* __restrict__ c = NULL;

	if (!_stp_runtime_context_trylock())
		return NULL;

	c = _stp_runtime_get_context();
	if (c != NULL) {
		if (!atomic_cmpxchg(&c->busy, 0, 1)) {
			// NB: Notice we're not releasing _stp_contexts_busy_ctr
			// here. We exepect the calling code to call
			// _stp_runtime_entryfn_get_context() and
			// _stp_runtime_entryfn_put_context() as a
			// pair.
			return c;
		}
	}
	_stp_runtime_context_unlock();
	return NULL;
}

static inline void _stp_runtime_entryfn_put_context(struct context *c)
{
	if (c) {
		atomic_set(&c->busy, 0);
		_stp_runtime_context_unlock();
	}
}

static void _stp_runtime_context_wait(void)
{
	int holdon;
	unsigned long hold_start;
	int hold_index;

	hold_start = jiffies;
	hold_index = -1;
	do {
		int i;

		holdon = 0;
		if (!_stp_runtime_context_trylock())
			break;

		for_each_possible_cpu(i) {
			struct context *c = per_cpu(contexts, i);
			if (c != NULL
			    && atomic_read (& c->busy)) {
				holdon = 1;

				/* Just in case things are really
				 * stuck, let's print some diagnostics. */
				if (time_after(jiffies, hold_start + HZ)  // > 1 second
				    && (i > hold_index)) { // not already printed
					hold_index = i;
					printk(KERN_ERR "%s context[%d] stuck: %s\n", THIS_MODULE->name, i, c->probe_point);
				}
			}
		}
		_stp_runtime_context_unlock();

		/*
		 * Just in case things are really really stuck, a
		 * handler probably suffered a fault, and the kernel
		 * probably killed a task/thread already.  We can't be
		 * quite sure in what state everything is in, however
		 * auxiliary stuff like kprobes / uprobes / locks have
		 * already been unregistered.  So it's *probably* safe
		 * to pretend/assume/hope everything is OK, and let
		 * the cleanup finish.
		 *
		 * In the worst case, there may occur a fault, as a
		 * genuinely running probe handler tries to access
		 * script globals (about to be freed), or something
		 * accesses module memory (about to be unloaded).
		 * This is sometimes stinky, so the alternative
		 * (default) is to change from a livelock to a
		 * livelock that sleeps awhile.
		 */
#ifdef STAP_OVERRIDE_STUCK_CONTEXT
		if (time_after(jiffies, hold_start + HZ*10)) {  // > 10 seconds
			printk(KERN_ERR "%s overriding stuck context to allow module shutdown.", THIS_MODULE->name);
			holdon = 0; // allow loop to exit
		}
#else
		/* at least stop sucking down the staprun cpu */
		msleep(250);
#endif

		/* NB: we run at least one of these during the
		 * shutdown sequence: */
		yield();	    /* aka schedule() and then some */
	} while (holdon);
}

#endif /* _LINUX_RUNTIME_CONTEXT_H_ */
