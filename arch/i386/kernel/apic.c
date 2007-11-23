/*
 *	Local APIC handling, local APIC timers
 *
 *	(c) 1999, 2000 Ingo Molnar <mingo@redhat.com>
 *
 */

#include <linux/config.h>
#include <linux/init.h>

#include <linux/mm.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/bootmem.h>
#include <linux/smp_lock.h>
#include <linux/interrupt.h>
#include <linux/mc146818rtc.h>
#include <linux/kernel_stat.h>

#include <asm/smp.h>
#include <asm/mtrr.h>
#include <asm/mpspec.h>
#include <asm/pgalloc.h>

int prof_multiplier[NR_CPUS] = { 1, };
int prof_old_multiplier[NR_CPUS] = { 1, };
int prof_counter[NR_CPUS] = { 1, };

/*
 * IA s/w dev Vol 3, Section 7.4
 */
#define APIC_DEFAULT_PHYS_BASE 0xfee00000

int get_maxlvt(void)
{
	unsigned int v, ver, maxlvt;

	v = apic_read(APIC_LVR);
	ver = GET_APIC_VERSION(v);
	/* 82489DXs do not report # of LVT entries. */
	maxlvt = APIC_INTEGRATED(ver) ? GET_APIC_MAXLVT(v) : 2;
	return maxlvt;
}

void disable_local_APIC (void)
{
	unsigned long value;
        int maxlvt;

	/*
	 * Disable APIC
	 */
 	value = apic_read(APIC_SPIV);
 	value &= ~(1<<8);
 	apic_write(APIC_SPIV,value);

	/*
	 * Clean APIC state for other OSs:
	 */
 	value = apic_read(APIC_SPIV);
 	value &= ~(1<<8);
 	apic_write(APIC_SPIV,value);
	maxlvt = get_maxlvt();
	apic_write_around(APIC_LVTT, 0x00010000);
	apic_write_around(APIC_LVT0, 0x00010000);
	apic_write_around(APIC_LVT1, 0x00010000);
	if (maxlvt >= 3)
		apic_write_around(APIC_LVTERR, 0x00010000);
	if (maxlvt >= 4)
		apic_write_around(APIC_LVTPC, 0x00010000);
}

extern void __error_in_apic_c (void);

void __init setup_local_APIC (void)
{
	unsigned long value, ver, maxlvt;

	if ((SPURIOUS_APIC_VECTOR & 0x0f) != 0x0f)
		__error_in_apic_c();

	/*
	 * Double-check wether this APIC is really registered.
	 */
	if (!test_bit(GET_APIC_ID(apic_read(APIC_ID)), &phys_cpu_present_map))
		BUG();

 	value = apic_read(APIC_SPIV);
	/*
	 * Enable APIC
	 */
 	value |= (1<<8);

	/*
	 * Some unknown Intel IO/APIC (or APIC) errata is biting us with
	 * certain networking cards. If high frequency interrupts are
	 * happening on a particular IOAPIC pin, plus the IOAPIC routing
	 * entry is masked/unmasked at a high rate as well then sooner or
	 * later IOAPIC line gets 'stuck', no more interrupts are received
	 * from the device. If focus CPU is disabled then the hang goes
	 * away, oh well :-(
	 *
	 * [ This bug can be reproduced easily with a level-triggered
	 *   PCI Ne2000 networking cards and PII/PIII processors, dual
	 *   BX chipset. ]
	 */
#if 0
	/* Enable focus processor (bit==0) */
 	value &= ~(1<<9);
#else
	/* Disable focus processor (bit==1) */
	value |= (1<<9);
#endif
	/*
	 * Set spurious IRQ vector
	 */
	value |= SPURIOUS_APIC_VECTOR;
 	apic_write(APIC_SPIV,value);

	/*
	 * Set up LVT0, LVT1:
	 *
	 * set up through-local-APIC on the BP's LINT0. This is not
	 * strictly necessery in pure symmetric-IO mode, but sometimes
	 * we delegate interrupts to the 8259A.
	 */
	if (!smp_processor_id()) {
		value = 0x00000700;
		printk("enabled ExtINT on CPU#%d\n", smp_processor_id());
	} else {
		value = 0x00010700;
		printk("masked ExtINT on CPU#%d\n", smp_processor_id());
	}
 	apic_write_around(APIC_LVT0,value);

	/*
	 * only the BP should see the LINT1 NMI signal, obviously.
	 */
	if (!smp_processor_id())
		value = 0x00000400;		// unmask NMI
	else
		value = 0x00010400;		// mask NMI
 	apic_write_around(APIC_LVT1,value);

	value = apic_read(APIC_LVR);
	ver = GET_APIC_VERSION(value);
	if (APIC_INTEGRATED(ver)) {		/* !82489DX */
		maxlvt = get_maxlvt();
		/*
		 * Due to the Pentium erratum 3AP.
		 */
		if (maxlvt > 3) {
			apic_readaround(APIC_SPIV); // not strictly necessery
			apic_write(APIC_ESR, 0);
		}
		value = apic_read(APIC_ESR);
		printk("ESR value before enabling vector: %08lx\n", value);

		value = apic_read(APIC_LVTERR);
		value = ERROR_APIC_VECTOR;      // enables sending errors
		apic_write(APIC_LVTERR,value);
		/*
		 * spec says clear errors after enabling vector.
		 */
		if (maxlvt != 3) {
			apic_readaround(APIC_SPIV);
			apic_write(APIC_ESR, 0);
		}
		value = apic_read(APIC_ESR);
		printk("ESR value after enabling vector: %08lx\n", value);
	} else
		printk("No ESR for 82489DX.\n");

	/*
	 * Set Task Priority to 'accept all'. We never change this
	 * later on.
	 */
 	value = apic_read(APIC_TASKPRI);
 	value &= ~APIC_TPRI_MASK;
 	apic_write(APIC_TASKPRI,value);

	/*
	 * Set up the logical destination ID and put the
	 * APIC into flat delivery mode.
	 */
 	value = apic_read(APIC_LDR);
	value &= ~APIC_LDR_MASK;
	value |= (1<<(smp_processor_id()+24));
 	apic_write(APIC_LDR,value);

 	value = apic_read(APIC_DFR);
	value |= SET_APIC_DFR(0xf);
 	apic_write(APIC_DFR, value);
}

void __init init_apic_mappings(void)
{
	unsigned long apic_phys;

	if (smp_found_config) {
		apic_phys = mp_lapic_addr;
	} else {
		/*
		 * set up a fake all zeroes page to simulate the
		 * local APIC and another one for the IO-APIC. We
		 * could use the real zero-page, but it's safer
		 * this way if some buggy code writes to this page ...
		 */
		apic_phys = (unsigned long) alloc_bootmem_pages(PAGE_SIZE);
		apic_phys = __pa(apic_phys);
	}
	set_fixmap_nocache(FIX_APIC_BASE, apic_phys);
	Dprintk("mapped APIC to %08lx (%08lx)\n", APIC_BASE, apic_phys);

#ifdef CONFIG_X86_IO_APIC
	{
		unsigned long ioapic_phys, idx = FIX_IO_APIC_BASE_0;
		int i;

		for (i = 0; i < nr_ioapics; i++) {
			if (smp_found_config) {
				ioapic_phys = mp_ioapics[i].mpc_apicaddr;
			} else {
				ioapic_phys = (unsigned long) alloc_bootmem_pages(PAGE_SIZE);
				ioapic_phys = __pa(ioapic_phys);
			}
			set_fixmap_nocache(idx, ioapic_phys);
			Dprintk("mapped IOAPIC to %08lx (%08lx)\n",
					__fix_to_virt(idx), ioapic_phys);
			idx++;
		}
	}
#endif
}

/*
 * This part sets up the APIC 32 bit clock in LVTT1, with HZ interrupts
 * per second. We assume that the caller has already set up the local
 * APIC.
 *
 * The APIC timer is not exactly sync with the external timer chip, it
 * closely follows bus clocks.
 */

/*
 * The timer chip is already set up at HZ interrupts per second here,
 * but we do not accept timer interrupts yet. We only allow the BP
 * to calibrate.
 */
static unsigned int __init get_8254_timer_count(void)
{
	extern rwlock_t xtime_lock;
	unsigned long flags;

	unsigned int count;

	write_lock_irqsave(&xtime_lock, flags);

	outb_p(0x00, 0x43);
	count = inb_p(0x40);
	count |= inb_p(0x40) << 8;

	write_unlock_irqrestore(&xtime_lock, flags);

	return count;
}

void __init wait_8254_wraparound(void)
{
	unsigned int curr_count, prev_count=~0;
	int delta;

	curr_count = get_8254_timer_count();

	do {
		prev_count = curr_count;
		curr_count = get_8254_timer_count();
		delta = curr_count-prev_count;

	/*
	 * This limit for delta seems arbitrary, but it isn't, it's
	 * slightly above the level of error a buggy Mercury/Neptune
	 * chipset timer can cause.
	 */

	} while (delta<300);
}

/*
 * This function sets up the local APIC timer, with a timeout of
 * 'clocks' APIC bus clock. During calibration we actually call
 * this function twice on the boot CPU, once with a bogus timeout
 * value, second time for real. The other (noncalibrating) CPUs
 * call this function only once, with the real, calibrated value.
 *
 * We do reads before writes even if unnecessary, to get around the
 * P5 APIC double write bug.
 */

#define APIC_DIVISOR 16

void __setup_APIC_LVTT(unsigned int clocks)
{
	unsigned int lvtt1_value, tmp_value;

	tmp_value = apic_read(APIC_LVTT);
	lvtt1_value = SET_APIC_TIMER_BASE(APIC_TIMER_BASE_DIV) |
			APIC_LVT_TIMER_PERIODIC | LOCAL_TIMER_VECTOR;
	apic_write(APIC_LVTT, lvtt1_value);

	/*
	 * Divide PICLK by 16
	 */
	tmp_value = apic_read(APIC_TDCR);
	apic_write(APIC_TDCR, (tmp_value
				& ~(APIC_TDR_DIV_1 | APIC_TDR_DIV_TMBASE))
				| APIC_TDR_DIV_16);

	tmp_value = apic_read(APIC_TMICT);
	apic_write(APIC_TMICT, clocks/APIC_DIVISOR);
}

void setup_APIC_timer(void * data)
{
	unsigned int clocks = (unsigned int) data, slice, t0, t1;
	unsigned long flags;
	int delta;

	__save_flags(flags);
	__sti();
	/*
	 * ok, Intel has some smart code in their APIC that knows
	 * if a CPU was in 'hlt' lowpower mode, and this increases
	 * its APIC arbitration priority. To avoid the external timer
	 * IRQ APIC event being in synchron with the APIC clock we
	 * introduce an interrupt skew to spread out timer events.
	 *
	 * The number of slices within a 'big' timeslice is smp_num_cpus+1
	 */

	slice = clocks / (smp_num_cpus+1);
	printk("cpu: %d, clocks: %d, slice: %d\n",
		smp_processor_id(), clocks, slice);

	/*
	 * Wait for IRQ0's slice:
	 */
	wait_8254_wraparound();

	__setup_APIC_LVTT(clocks);

	t0 = apic_read(APIC_TMCCT)*APIC_DIVISOR;
	do {
		t1 = apic_read(APIC_TMCCT)*APIC_DIVISOR;
		delta = (int)(t0 - t1 - slice*(smp_processor_id()+1));
	} while (delta < 0);

	__setup_APIC_LVTT(clocks);

	printk("CPU%d<C0:%d,C:%d,D:%d,S:%d,C:%d>\n",
			smp_processor_id(), t0, t1, delta, slice, clocks);

	__restore_flags(flags);
}

/*
 * In this function we calibrate APIC bus clocks to the external
 * timer. Unfortunately we cannot use jiffies and the timer irq
 * to calibrate, since some later bootup code depends on getting
 * the first irq? Ugh.
 *
 * We want to do the calibration only once since we
 * want to have local timer irqs syncron. CPUs connected
 * by the same APIC bus have the very same bus frequency.
 * And we want to have irqs off anyways, no accidental
 * APIC irq that way.
 */

int __init calibrate_APIC_clock(void)
{
	unsigned long long t1 = 0, t2 = 0;
	long tt1, tt2;
	long result;
	int i;
	const int LOOPS = HZ/10;

	printk("calibrating APIC timer ... ");

	/*
	 * Put whatever arbitrary (but long enough) timeout
	 * value into the APIC clock, we just want to get the
	 * counter running for calibration.
	 */
	__setup_APIC_LVTT(1000000000);

	/*
	 * The timer chip counts down to zero. Let's wait
	 * for a wraparound to start exact measurement:
	 * (the current tick might have been already half done)
	 */

	wait_8254_wraparound();

	/*
	 * We wrapped around just now. Let's start:
	 */
	if (cpu_has_tsc)
		rdtscll(t1);
	tt1 = apic_read(APIC_TMCCT);

	/*
	 * Let's wait LOOPS wraprounds:
	 */
	for (i = 0; i < LOOPS; i++)
		wait_8254_wraparound();

	tt2 = apic_read(APIC_TMCCT);
	if (cpu_has_tsc)
		rdtscll(t2);

	/*
	 * The APIC bus clock counter is 32 bits only, it
	 * might have overflown, but note that we use signed
	 * longs, thus no extra care needed.
	 *
	 * underflown to be exact, as the timer counts down ;)
	 */

	result = (tt1-tt2)*APIC_DIVISOR/LOOPS;

	if (cpu_has_tsc)
		printk("\n..... CPU clock speed is %ld.%04ld MHz.\n",
			((long)(t2-t1)/LOOPS)/(1000000/HZ),
			((long)(t2-t1)/LOOPS)%(1000000/HZ));

	printk("..... host bus clock speed is %ld.%04ld MHz.\n",
		result/(1000000/HZ),
		result%(1000000/HZ));

	return result;
}

static unsigned int calibration_result;

void __init setup_APIC_clocks (void)
{
	__cli();

	calibration_result = calibrate_APIC_clock();
	/*
	 * Now set up the timer for real.
	 */
	setup_APIC_timer((void *)calibration_result);

	__sti();

	/* and update all other cpus */
	smp_call_function(setup_APIC_timer, (void *)calibration_result, 1, 1);
}

/*
 * the frequency of the profiling timer can be changed
 * by writing a multiplier value into /proc/profile.
 */
int setup_profiling_timer(unsigned int multiplier)
{
	int i;

	/*
	 * Sanity check. [at least 500 APIC cycles should be
	 * between APIC interrupts as a rule of thumb, to avoid
	 * irqs flooding us]
	 */
	if ( (!multiplier) || (calibration_result/multiplier < 500))
		return -EINVAL;

	/* 
	 * Set the new multiplier for each CPU. CPUs don't start using the
	 * new values until the next timer interrupt in which they do process
	 * accounting. At that time they also adjust their APIC timers
	 * accordingly.
	 */
	for (i = 0; i < NR_CPUS; ++i)
		prof_multiplier[i] = multiplier;

	return 0;
}

#undef APIC_DIVISOR

/*
 * Local timer interrupt handler. It does both profiling and
 * process statistics/rescheduling.
 *
 * We do profiling in every local tick, statistics/rescheduling
 * happen only every 'profiling multiplier' ticks. The default
 * multiplier is 1 and it can be changed by writing the new multiplier
 * value into /proc/profile.
 */

inline void smp_local_timer_interrupt(struct pt_regs * regs)
{
	int user = (user_mode(regs) != 0);
	int cpu = smp_processor_id();

	/*
	 * The profiling function is SMP safe. (nothing can mess
	 * around with "current", and the profiling counters are
	 * updated with atomic operations). This is especially
	 * useful with a profiling multiplier != 1
	 */
	if (!user)
		x86_do_profile(regs->eip);

	if (--prof_counter[cpu] <= 0) {
		int system = 1 - user;
		struct task_struct * p = current;

		/*
		 * The multiplier may have changed since the last time we got
		 * to this point as a result of the user writing to
		 * /proc/profile. In this case we need to adjust the APIC
		 * timer accordingly.
		 *
		 * Interrupts are already masked off at this point.
		 */
		prof_counter[cpu] = prof_multiplier[cpu];
		if (prof_counter[cpu] != prof_old_multiplier[cpu]) {
			__setup_APIC_LVTT(calibration_result/prof_counter[cpu]);
			prof_old_multiplier[cpu] = prof_counter[cpu];
		}

		/*
		 * After doing the above, we need to make like
		 * a normal interrupt - otherwise timer interrupts
		 * ignore the global interrupt lock, which is the
		 * WrongThing (tm) to do.
		 */

 		irq_enter(cpu, 0);
		update_one_process(p, 1, user, system, cpu);
		if (p->pid) {
			p->counter -= 1;
			if (p->counter <= 0) {
				p->counter = 0;
				p->need_resched = 1;
			}
			if (p->priority < DEF_PRIORITY) {
				kstat.cpu_nice += user;
				kstat.per_cpu_nice[cpu] += user;
			} else {
				kstat.cpu_user += user;
				kstat.per_cpu_user[cpu] += user;
			}
			kstat.cpu_system += system;
			kstat.per_cpu_system[cpu] += system;

		}
		irq_exit(cpu, 0);
	}

	/*
	 * We take the 'long' return path, and there every subsystem
	 * grabs the apropriate locks (kernel lock/ irq lock).
	 *
	 * we might want to decouple profiling from the 'long path',
	 * and do the profiling totally in assembly.
	 *
	 * Currently this isn't too much of an issue (performance wise),
	 * we can take more than 100K local irqs per second on a 100 MHz P5.
	 */
}

/*
 * Local APIC timer interrupt. This is the most natural way for doing
 * local interrupts, but local timer interrupts can be emulated by
 * broadcast interrupts too. [in case the hw doesnt support APIC timers]
 *
 * [ if a single-CPU system runs an SMP kernel then we call the local
 *   interrupt as well. Thus we cannot inline the local irq ... ]
 */
unsigned int apic_timer_irqs [NR_CPUS] = { 0, };

void smp_apic_timer_interrupt(struct pt_regs * regs)
{
	/*
	 * the NMI deadlock-detector uses this.
	 */
	apic_timer_irqs[smp_processor_id()]++;

	/*
	 * NOTE! We'd better ACK the irq immediately,
	 * because timer handling can be slow.
	 */
	ack_APIC_irq();
	smp_local_timer_interrupt(regs);
}

/*
 * This interrupt should _never_ happen with our APIC/SMP architecture
 */
asmlinkage void smp_spurious_interrupt(void)
{
	ack_APIC_irq();
	/* see sw-dev-man vol 3, chapter 7.4.13.5 */
	printk("spurious APIC interrupt on CPU#%d, should never happen.\n",
			smp_processor_id());
}

/*
 * This interrupt should never happen with our APIC/SMP architecture
 */

static spinlock_t err_lock = SPIN_LOCK_UNLOCKED;

asmlinkage void smp_error_interrupt(void)
{
	unsigned long v;

	spin_lock(&err_lock);

	v = apic_read(APIC_ESR);
	printk("APIC error interrupt on CPU#%d, should never happen.\n",
			smp_processor_id());
	printk("... APIC ESR0: %08lx\n", v);

	apic_write(APIC_ESR, 0);
	v |= apic_read(APIC_ESR);
	printk("... APIC ESR1: %08lx\n", v);
	/*
	 * Be a bit more verbose. (multiple bits can be set)
	 */
	if (v & 0x01)
		printk("... bit 0: APIC Send CS Error (hw problem).\n");
	if (v & 0x02)
		printk("... bit 1: APIC Receive CS Error (hw problem).\n");
	if (v & 0x04)
		printk("... bit 2: APIC Send Accept Error.\n");
	if (v & 0x08)
		printk("... bit 3: APIC Receive Accept Error.\n");
	if (v & 0x10)
		printk("... bit 4: Reserved!.\n");
	if (v & 0x20)
		printk("... bit 5: Send Illegal Vector (kernel bug).\n");
	if (v & 0x40)
		printk("... bit 6: Received Illegal Vector.\n");
	if (v & 0x80)
		printk("... bit 7: Illegal Register Address.\n");

	ack_APIC_irq();

	irq_err_count++;

	spin_unlock(&err_lock);
}

