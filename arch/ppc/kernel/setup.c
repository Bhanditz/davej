/*
 * $Id: setup.c,v 1.160 1999/10/08 01:56:38 paulus Exp $
 * Common prep/pmac/chrp boot and setup code.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/blk.h>
#include <linux/ide.h>

#include <asm/init.h>
#include <asm/residual.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/processor.h>
#include <asm/pgtable.h>
#include <asm/bootinfo.h>
#include <asm/setup.h>
#include <asm/amigappc.h>
#include <asm/smp.h>
#ifdef CONFIG_8xx
#include <asm/mpc8xx.h>
#include <asm/8xx_immap.h>
#endif
#include <asm/bootx.h>
#include <asm/machdep.h>
#ifdef CONFIG_OAK
#include "oak_setup.h"
#endif /* CONFIG_OAK */

extern void pmac_init(unsigned long r3,
                      unsigned long r4,
                      unsigned long r5,
                      unsigned long r6,
                      unsigned long r7);

extern void chrp_init(unsigned long r3,
                      unsigned long r4,
                      unsigned long r5,
                      unsigned long r6,
                      unsigned long r7);

extern void prep_init(unsigned long r3,
                      unsigned long r4,
                      unsigned long r5,
                      unsigned long r6,
                      unsigned long r7);

extern void m8xx_init(unsigned long r3,
		     unsigned long r4,
		     unsigned long r5,
		     unsigned long r6,
		     unsigned long r7);

extern void apus_init(unsigned long r3,
                      unsigned long r4,
                      unsigned long r5,
                      unsigned long r6,
                      unsigned long r7);

extern void gemini_init(unsigned long r3,
                      unsigned long r4,
                      unsigned long r5,
                      unsigned long r6,
                      unsigned long r7);

extern boot_infos_t *boot_infos;
char saved_command_line[256];
unsigned char aux_device_present;
struct int_control_struct int_control =
{
	__no_use_cli,
	__no_use_sti,
	__no_use_restore_flags,
	__no_use_save_flags
};
struct ide_machdep_calls ppc_ide_md;
int parse_bootinfo(void);

unsigned long ISA_DMA_THRESHOLD;
unsigned long DMA_MODE_READ, DMA_MODE_WRITE;

#ifndef CONFIG_MACH_SPECIFIC
int _machine = 0;
int have_of = 0;
#endif /* CONFIG_MACH_SPECIFIC */

#ifdef CONFIG_MAGIC_SYSRQ
unsigned long SYSRQ_KEY;
#endif /* CONFIG_MAGIC_SYSRQ */

struct machdep_calls ppc_md;

/*
 * Perhaps we can put the pmac screen_info[] here
 * on pmac as well so we don't need the ifdef's.
 * Until we get multiple-console support in here
 * that is.  -- Cort
 */ 
#if !defined(CONFIG_4xx) && !defined(CONFIG_8xx)
struct screen_info screen_info = {
	0, 25,			/* orig-x, orig-y */
	0,			/* unused */
	0,			/* orig-video-page */
	0,			/* orig-video-mode */
	80,			/* orig-video-cols */
	0,0,0,			/* ega_ax, ega_bx, ega_cx */
	25,			/* orig-video-lines */
	1,			/* orig-video-isVGA */
	16			/* orig-video-points */
};

/*
 * I really need to add multiple-console support... -- Cort
 */
int __init pmac_display_supported(char *name)
{
	return 0;
}
void __init pmac_find_display(void)
{
}

#else /* CONFIG_4xx || CONFIG_8xx */

/* We need this to satisfy some external references until we can
 * strip the kernel down.
 */
struct screen_info screen_info = {
	0, 25,			/* orig-x, orig-y */
	0,			/* unused */
	0,			/* orig-video-page */
	0,			/* orig-video-mode */
	80,			/* orig-video-cols */
	0,0,0,			/* ega_ax, ega_bx, ega_cx */
	25,			/* orig-video-lines */
	0,			/* orig-video-isVGA */
	16			/* orig-video-points */
};
#endif /* !CONFIG_4xx && !CONFIG_8xx */

void machine_restart(char *cmd)
{
	ppc_md.restart(cmd);
}
  
void machine_power_off(void)
{
	ppc_md.power_off();
}
  
void machine_halt(void)
{
	ppc_md.halt();
}
  
unsigned long cpu_temp(void)
{
	unsigned char thres = 0;
	
#if 0
	/* disable thrm2 */
	_set_THRM2( 0 );
	/* threshold 0 C, tid: exceeding threshold, tie: don't generate interrupt */
	_set_THRM1( THRM1_V );

	/* we need 20us to do the compare - assume 300MHz processor clock */
	_set_THRM3(0);
	_set_THRM3(THRM3_E | (300*30)<<18 );

	udelay(100);
	/* wait for the compare to complete */
	/*while ( !(_get_THRM1() & THRM1_TIV) ) ;*/
	if ( !(_get_THRM1() & THRM1_TIV) )
		printk("no tiv\n");
	if ( _get_THRM1() & THRM1_TIN )
		printk("crossed\n");
	/* turn everything off */
	_set_THRM3(0);
	_set_THRM1(0);
#endif
		
	return thres;
}

int get_cpuinfo(char *buffer)
{
	unsigned long len = 0;
	unsigned long bogosum = 0;
	unsigned long i;
	unsigned int pvr;
	unsigned short maj, min;
	
#ifdef __SMP__
#define CPU_PRESENT(x) (cpu_callin_map[(x)])
#define GET_PVR ((long int)(cpu_data[i].pvr))
#define CD(x) (cpu_data[i].x)
#else
#define CPU_PRESENT(x) ((x)==0)
#define smp_num_cpus 1
#define GET_PVR ((long int)_get_PVR())
#define CD(x) (x)
#endif	

	for ( i = 0; i < smp_num_cpus ; i++ )
	{
		if ( !CPU_PRESENT(i) )
			continue;
		if ( i )
			len += sprintf(len+buffer,"\n");
		len += sprintf(len+buffer,"processor\t: %lu\n",i);
		len += sprintf(len+buffer,"cpu\t\t:  ");

		pvr = GET_PVR;
	
		switch (PVR_VER(pvr))
		{
		case 0x0001:
			len += sprintf(len+buffer, "601\n");
			break;
		case 0x0003:
			len += sprintf(len+buffer, "603\n");
			break;
		case 0x0004:
			len += sprintf(len+buffer, "604\n");
			break;
		case 0x0006:
			len += sprintf(len+buffer, "603e\n");
			break;
		case 0x0007:
			len += sprintf(len+buffer, "603");
			if (((pvr >> 12) & 0xF) == 1) {
				pvr ^= 0x00001000;	/* revision fix-up */
				len += sprintf(len+buffer, "r\n");
			} else {
				len += sprintf(len+buffer, "ev\n");
			}
			break;
		case 0x0008:		/* 740/750(P) */
		case 0x1008:
			len += sprintf(len+buffer, "750%s\n",
				       PVR_VER(pvr) == 0x1008 ? "P" : "");
			len += sprintf(len+buffer, "temperature \t: %lu C\n",
				       cpu_temp());
			break;
		case 0x0009:		/* 604e/604r */
		case 0x000A:
			len += sprintf(len+buffer, "604");

			if (PVR_VER(pvr) == 0x000A ||
			    ((pvr >> 12) & 0xF) != 0) {
				pvr &= ~0x00003000;	/* revision fix-up */
				len += sprintf(len+buffer, "r\n");
			} else {
				len += sprintf(len+buffer, "e\n");
			}
			break;
		case 0x000C:
			len += sprintf(len+buffer, "7400\n");
			break;
		case 0x0020:
			len += sprintf(len+buffer, "403G");
			switch ((pvr >> 8) & 0xFF) {
			case 0x02:
				len += sprintf(len+buffer, "C\n");    
				break;				      
			case 0x14:
				len += sprintf(len+buffer, "CX\n");
				break;
			}
			break;
		case 0x0050:
			len += sprintf(len+buffer, "821\n");
			break;
		case 0x0081:
			len += sprintf(len+buffer, "8240\n");
			break;
		case 0x4011:
			len += sprintf(len+buffer, "405GP\n");
			break;
		default:
			len += sprintf(len+buffer, "unknown (%08x)\n", pvr);
			break;
		}
		
		/*
		 * Assume here that all clock rates are the same in a
		 * smp system.  -- Cort
		 */
#ifndef CONFIG_8xx
		if ( have_of )
		{
			struct device_node *cpu_node;
			int *fp;
			
			cpu_node = find_type_devices("cpu");
			if ( !cpu_node ) break;
			{
				int s;
				for ( s = 0; (s < i) && cpu_node->next ;
				      s++, cpu_node = cpu_node->next )
					/* nothing */ ;
#if 0 /* SMP Pmacs don't have all cpu nodes -- Cort */
				if ( s != i )
					printk("get_cpuinfo(): ran out of "
					       "cpu nodes.\n");
#endif
			}
			fp = (int *) get_property(cpu_node, "clock-frequency", NULL);
			if ( !fp ) break;
			len += sprintf(len+buffer, "clock\t\t: %dMHz\n",
				       *fp / 1000000);
		}
#endif

		if (ppc_md.setup_residual != NULL)
		{
			len += ppc_md.setup_residual(buffer + len);
		}
		
		switch (PVR_VER(pvr))
		{
		case 0x0020:
			maj = PVR_MAJ(pvr) + 1;
			min = PVR_MIN(pvr);
			break;
		case 0x1008:
			maj = ((pvr >> 8) & 0xFF) - 1;
			min = pvr & 0xFF;
			break;
		default:
			maj = (pvr >> 8) & 0xFF;
			min = pvr & 0xFF;
			break;
		}

		len += sprintf(len+buffer, "revision\t: %hd.%hd\n", maj, min);

		len += sprintf(buffer+len, "bogomips\t: %lu.%02lu\n",
			       (CD(loops_per_sec)+2500)/500000,
			       (CD(loops_per_sec)+2500)/5000 % 100);
		bogosum += CD(loops_per_sec);
	}

#ifdef __SMP__
	if ( i )
		len += sprintf(buffer+len, "\n");
	len += sprintf(buffer+len,"total bogomips\t: %lu.%02lu\n",
		       (bogosum+2500)/500000,
		       (bogosum+2500)/5000 % 100);
#endif /* __SMP__ */

	/*
	 * Ooh's and aah's info about zero'd pages in idle task
	 */ 
	len += sprintf(buffer+len,"zero pages\t: total: %u (%luKb) "
		       "current: %u (%luKb) hits: %u/%u (%u%%)\n",
		       atomic_read(&zero_cache_total),
		       (atomic_read(&zero_cache_total)*PAGE_SIZE)>>10,
		       atomic_read(&zero_cache_sz),
		       (atomic_read(&zero_cache_sz)*PAGE_SIZE)>>10,
		       atomic_read(&zero_cache_hits),atomic_read(&zero_cache_calls),
		       /* : 1 below is so we don't div by zero */
		       (atomic_read(&zero_cache_hits)*100) /
		       ((atomic_read(&zero_cache_calls))?atomic_read(&zero_cache_calls):1));

	if (ppc_md.get_cpuinfo != NULL)
	{
		len += ppc_md.get_cpuinfo(buffer+len);
	}

	return len;
}

#ifndef CONFIG_MACH_SPECIFIC
void __init
intuit_machine_type(void)
{
	char *model;
	struct device_node *root;
			
	/* ask the OF info if we're a chrp or pmac */
	root = find_path_device("/");
	if (root != 0) {
		/* assume pmac unless proven to be chrp -- Cort */
		_machine = _MACH_Pmac;
		model = get_property(root, "device_type", NULL);
		if (model && !strncmp("chrp", model, 4))
			_machine = _MACH_chrp;
		else {
			model = get_property(root, "model", NULL);
			if (model && !strncmp(model, "IBM", 3))
				_machine = _MACH_chrp;
		}
	}
}
#endif /* CONFIG_MACH_SPECIFIC */

/*
 * Find out what kind of machine we're on and save any data we need
 * from the early boot process (devtree is copied on pmac by prom_init() )
 */
unsigned long __init
identify_machine(unsigned long r3, unsigned long r4, unsigned long r5,
		 unsigned long r6, unsigned long r7)
{
	parse_bootinfo();
	
	if ( ppc_md.progress ) ppc_md.progress("id mach(): start", 0x100);
#if !defined(CONFIG_4xx) && !defined(CONFIG_8xx)
#ifndef CONFIG_MACH_SPECIFIC
	/* if we didn't get any bootinfo telling us what we are... */
	if ( _machine == 0 )
	{
		/* boot loader will tell us if we're APUS */
		if ( r3 == 0x61707573 )
		{
			_machine = _MACH_apus;
			r3 = 0;
		}
		/* prep boot loader tells us if we're prep or not */
		else if ( *(unsigned long *)(KERNELBASE) == (0xdeadc0de) )
		{
			_machine = _MACH_prep;
		} else
			have_of = 1;
	}
#endif /* CONFIG_MACH_SPECIFIC */

	if ( have_of )
	{
		/* prom_init has already been called from __start */
		if (boot_infos)
			relocate_nodes();
#ifndef CONFIG_MACH_SPECIFIC
		/* we need to set _machine before calling finish_device_tree */
		if (_machine == 0)
			intuit_machine_type();
#endif /* CONFIG_MACH_SPECIFIC */
		finish_device_tree();

		/*
		 * If we were booted via quik, r3 points to the physical
		 * address of the command-line parameters.
		 * If we were booted from an xcoff image (i.e. netbooted or
		 * booted from floppy), we get the command line from the
		 * bootargs property of the /chosen node.
		 * If an initial ramdisk is present, r3 and r4
		 * are used for initrd_start and initrd_size,
		 * otherwise they contain 0xdeadbeef.  
		 */
		cmd_line[0] = 0;
		if (r3 >= 0x4000 && r3 < 0x800000 && r4 == 0) {
			strncpy(cmd_line, (char *)r3 + KERNELBASE,
				sizeof(cmd_line));
		} else if (boot_infos != 0) {
			/* booted by BootX - check for ramdisk */
			if (boot_infos->kernelParamsOffset != 0)
				strncpy(cmd_line, (char *) boot_infos
					+ boot_infos->kernelParamsOffset,
					sizeof(cmd_line));
#ifdef CONFIG_BLK_DEV_INITRD
			if (boot_infos->ramDisk) {
				initrd_start = (unsigned long) boot_infos
					+ boot_infos->ramDisk;
				initrd_end = initrd_start + boot_infos->ramDiskSize;
				initrd_below_start_ok = 1;
			}
#endif
		} else {
			struct device_node *chosen;
			char *p;
			
#ifdef CONFIG_BLK_DEV_INITRD
			if (r3 - KERNELBASE < 0x800000
			    && r4 != 0 && r4 != 0xdeadbeef) {
				initrd_start = r3;
				initrd_end = r3 + r4;
				ROOT_DEV = MKDEV(RAMDISK_MAJOR, 0);
			}
#endif
			cmd_line[0] = 0;
			chosen = find_devices("chosen");
			if (chosen != NULL) {
				p = get_property(chosen, "bootargs", NULL);
				if (p != NULL)
					strncpy(cmd_line, p, sizeof(cmd_line));
			}
		}
		cmd_line[sizeof(cmd_line) - 1] = 0;
	}

	switch (_machine)
	{
	case _MACH_Pmac:
                pmac_init(r3, r4, r5, r6, r7);
		break;
	case _MACH_prep:
                prep_init(r3, r4, r5, r6, r7);
		break;
	case _MACH_chrp:
                chrp_init(r3, r4, r5, r6, r7);
		break;
#ifdef CONFIG_APUS
	case _MACH_apus:
                apus_init(r3, r4, r5, r6, r7);
		break;
#endif
#ifdef CONFIG_GEMINI
	case _MACH_gemini:
		gemini_init(r3, r4, r5, r6, r7);
		break;
#endif
	default:
		printk("Unknown machine type in identify_machine!\n");
	}
	/* Check for nobats option (used in mapin_ram). */
	if (strstr(cmd_line, "nobats")) {
		extern int __map_without_bats;
		__map_without_bats = 1;
	}
#else
#if defined(CONFIG_4xx)
	oak_init(r3, r4, r5, r6, r7);
#elif defined(CONFIG_8xx)
        m8xx_init(r3, r4, r5, r6, r7);
#else
#error "No board type has been defined for identify_machine()!"
#endif /* CONFIG_4xx */
#endif /* !CONFIG_4xx && !CONFIG_8xx */

	/* Look for mem= option on command line */
	if (strstr(cmd_line, "mem=")) {
		char *p, *q;
		unsigned long maxmem = 0;
		extern unsigned long __max_memory;

		for (q = cmd_line; (p = strstr(q, "mem=")) != 0; ) {
			q = p + 4;
			if (p > cmd_line && p[-1] != ' ')
				continue;
			maxmem = simple_strtoul(q, &q, 0);
			if (*q == 'k' || *q == 'K') {
				maxmem <<= 10;
				++q;
			} else if (*q == 'm' || *q == 'M') {
				maxmem <<= 20;
				++q;
			}
		}
		__max_memory = maxmem;
	}
	
	/* this is for modules since _machine can be a define -- Cort */
	ppc_md.ppc_machine = _machine;

	if ( ppc_md.progress ) ppc_md.progress("id mach(): done", 0x200);

	return 0;
}

int parse_bootinfo(void)
{
	struct bi_record *rec;
	extern char _end[];

	rec = (struct bi_record *)PAGE_ALIGN((ulong)_end);
	if ( rec->tag != BI_FIRST )
	{
		/*
		 * This 0x10000 offset is a terrible hack but it will go away when
		 * we have the bootloader handle all the relocation and
		 * prom calls -- Cort
		 */
		rec = (struct bi_record *)PAGE_ALIGN((ulong)_end+0x10000);
		if ( rec->tag != BI_FIRST )
			return -1;
	}

	for ( ; rec->tag != BI_LAST ;
	      rec = (struct bi_record *)((ulong)rec + rec->size) )
	{
		ulong *data = rec->data;
		switch (rec->tag)
		{
		case BI_CMD_LINE:
			memcpy(cmd_line, (void *)data, rec->size);
			break;
#ifdef CONFIG_BLK_DEV_INITRD
		case BI_INITRD:
			initrd_start = data[0];
			initrd_end = data[0] + rec->size;
			break;
#endif /* CONFIG_BLK_DEV_INITRD */
#ifndef CONFIG_MACH_SPECIFIC
		case BI_MACHTYPE:
			_machine = data[0];
			have_of = data[1];
			break;
#endif /* CONFIG_MACH_SPECIFIC */
			
		}
	}

	return 0;
}

/* Checks "l2cr=xxxx" command-line option */
void ppc_setup_l2cr(char *str, int *ints)
{
	if ( (_get_PVR() >> 16) == 8)
	{
		unsigned long val = simple_strtoul(str, NULL, 0);
		printk(KERN_INFO "l2cr set to %lx\n", val);
		_set_L2CR(0);
		_set_L2CR(val);
	}
}

void __init ppc_init(void)
{
	/* clear the progress line */
	if ( ppc_md.progress ) ppc_md.progress(" ", 0xffff);
	
	if (ppc_md.init != NULL) {
		ppc_md.init();
	}
}

void __init setup_arch(char **cmdline_p)
{
	extern int panic_timeout;
	extern char _etext[], _edata[];
	extern char *klimit;
	extern void do_init_bootmem(void);

#ifdef CONFIG_XMON
	extern void xmon_map_scc(void);
	xmon_map_scc();
	if (strstr(cmd_line, "xmon"))
		xmon(0);
#endif /* CONFIG_XMON */

	/* reboot on panic */
	panic_timeout = 180;

	init_mm.start_code = PAGE_OFFSET;
	init_mm.end_code = (unsigned long) _etext;
	init_mm.end_data = (unsigned long) _edata;
	init_mm.brk = (unsigned long) klimit;

	/* Save unparsed command line copy for /proc/cmdline */
	strcpy(saved_command_line, cmd_line);
	*cmdline_p = cmd_line;

	/* set up the bootmem stuff with available memory */
	do_init_bootmem();

	ppc_md.setup_arch();
	/* clear the progress line */
	if ( ppc_md.progress ) ppc_md.progress("arch: exit", 0x3eab);
}

void ppc_generic_ide_fix_driveid(struct hd_driveid *id)
{
        int i;
	unsigned short *stringcast;


	id->config         = __le16_to_cpu(id->config);
	id->cyls           = __le16_to_cpu(id->cyls);
	id->reserved2      = __le16_to_cpu(id->reserved2);
	id->heads          = __le16_to_cpu(id->heads);
	id->track_bytes    = __le16_to_cpu(id->track_bytes);
	id->sector_bytes   = __le16_to_cpu(id->sector_bytes);
	id->sectors        = __le16_to_cpu(id->sectors);
	id->vendor0        = __le16_to_cpu(id->vendor0);
	id->vendor1        = __le16_to_cpu(id->vendor1);
	id->vendor2        = __le16_to_cpu(id->vendor2);
	stringcast = (unsigned short *)&id->serial_no[0];
	for (i=0; i<(20/2); i++)
	        stringcast[i] = __le16_to_cpu(stringcast[i]);
	id->buf_type       = __le16_to_cpu(id->buf_type);
	id->buf_size       = __le16_to_cpu(id->buf_size);
	id->ecc_bytes      = __le16_to_cpu(id->ecc_bytes);
	stringcast = (unsigned short *)&id->fw_rev[0];
	for (i=0; i<(8/2); i++)
	        stringcast[i] = __le16_to_cpu(stringcast[i]);
	stringcast = (unsigned short *)&id->model[0];
	for (i=0; i<(40/2); i++)
	        stringcast[i] = __le16_to_cpu(stringcast[i]);
	id->dword_io       = __le16_to_cpu(id->dword_io);
	id->reserved50     = __le16_to_cpu(id->reserved50);
	id->field_valid    = __le16_to_cpu(id->field_valid);
	id->cur_cyls       = __le16_to_cpu(id->cur_cyls);
	id->cur_heads      = __le16_to_cpu(id->cur_heads);
	id->cur_sectors    = __le16_to_cpu(id->cur_sectors);
	id->cur_capacity0  = __le16_to_cpu(id->cur_capacity0);
	id->cur_capacity1  = __le16_to_cpu(id->cur_capacity1);
	id->lba_capacity   = __le32_to_cpu(id->lba_capacity);
	id->dma_1word      = __le16_to_cpu(id->dma_1word);
	id->dma_mword      = __le16_to_cpu(id->dma_mword);
	id->eide_pio_modes = __le16_to_cpu(id->eide_pio_modes);
	id->eide_dma_min   = __le16_to_cpu(id->eide_dma_min);
	id->eide_dma_time  = __le16_to_cpu(id->eide_dma_time);
	id->eide_pio       = __le16_to_cpu(id->eide_pio);
	id->eide_pio_iordy = __le16_to_cpu(id->eide_pio_iordy);
	id->word69         = __le16_to_cpu(id->word69);
	id->word70         = __le16_to_cpu(id->word70);
	id->word71         = __le16_to_cpu(id->word71);
	id->word72         = __le16_to_cpu(id->word72);
	id->word73         = __le16_to_cpu(id->word73);
	id->word74         = __le16_to_cpu(id->word74);
	id->word75         = __le16_to_cpu(id->word75);
	id->word76         = __le16_to_cpu(id->word76);
	id->word77         = __le16_to_cpu(id->word77);
	id->word78         = __le16_to_cpu(id->word78);
	id->word79         = __le16_to_cpu(id->word79);
	id->word80         = __le16_to_cpu(id->word80);
	id->word81         = __le16_to_cpu(id->word81);
	id->command_sets   = __le16_to_cpu(id->command_sets);
	id->word83         = __le16_to_cpu(id->word83);
	id->word84         = __le16_to_cpu(id->word84);
	id->word85         = __le16_to_cpu(id->word85);
	id->word86         = __le16_to_cpu(id->word86);
	id->word87         = __le16_to_cpu(id->word87);
	id->dma_ultra      = __le16_to_cpu(id->dma_ultra);
	id->word89         = __le16_to_cpu(id->word89);
	id->word90         = __le16_to_cpu(id->word90);
	id->word91         = __le16_to_cpu(id->word91);
	id->word92         = __le16_to_cpu(id->word92);
	id->word93         = __le16_to_cpu(id->word93);
	id->word94         = __le16_to_cpu(id->word94);
	id->word95         = __le16_to_cpu(id->word95);
	id->word96         = __le16_to_cpu(id->word96);
	id->word97         = __le16_to_cpu(id->word97);
	id->word98         = __le16_to_cpu(id->word98);
	id->word99         = __le16_to_cpu(id->word99);
	id->word100        = __le16_to_cpu(id->word100);
	id->word101        = __le16_to_cpu(id->word101);
	id->word102        = __le16_to_cpu(id->word102);
	id->word103        = __le16_to_cpu(id->word103);
	id->word104        = __le16_to_cpu(id->word104);
	id->word105        = __le16_to_cpu(id->word105);
	id->word106        = __le16_to_cpu(id->word106);
	id->word107        = __le16_to_cpu(id->word107);
	id->word108        = __le16_to_cpu(id->word108);
	id->word109        = __le16_to_cpu(id->word109);
	id->word110        = __le16_to_cpu(id->word110);
	id->word111        = __le16_to_cpu(id->word111);
	id->word112        = __le16_to_cpu(id->word112);
	id->word113        = __le16_to_cpu(id->word113);
	id->word114        = __le16_to_cpu(id->word114);
	id->word115        = __le16_to_cpu(id->word115);
	id->word116        = __le16_to_cpu(id->word116);
	id->word117        = __le16_to_cpu(id->word117);
	id->word118        = __le16_to_cpu(id->word118);
	id->word119        = __le16_to_cpu(id->word119);
	id->word120        = __le16_to_cpu(id->word120);
	id->word121        = __le16_to_cpu(id->word121);
	id->word122        = __le16_to_cpu(id->word122);
	id->word123        = __le16_to_cpu(id->word123);
	id->word124        = __le16_to_cpu(id->word124);
	id->word125        = __le16_to_cpu(id->word125);
	id->word126        = __le16_to_cpu(id->word126);
	id->word127        = __le16_to_cpu(id->word127);
	id->security       = __le16_to_cpu(id->security);
	for (i=0; i<127; i++)
	        id->reserved[i] = __le16_to_cpu(id->reserved[i]);
}
