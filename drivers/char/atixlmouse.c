/*
 * ATI XL Bus Mouse Driver for Linux
 * by Bob Harris (rth@sparta.com)
 *
 * Uses VFS interface for linux 0.98 (01OCT92)
 *
 * Modified by Chris Colohan (colohan@eecg.toronto.edu)
 * Modularised 8-Sep-95 Philip Blundell <pjb27@cam.ac.uk>
 *
 * version 0.3a
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/irq.h>

#define ATIXL_MOUSE_IRQ		5 /* H/W interrupt # set up on ATIXL board */
#define ATIXL_BUSMOUSE		3 /* Minor device # (mknod c 10 3 /dev/bm) */

/* ATI XL Inport Busmouse Definitions */

#define	ATIXL_MSE_DATA_PORT		0x23d
#define	ATIXL_MSE_SIGNATURE_PORT	0x23e
#define	ATIXL_MSE_CONTROL_PORT		0x23c

#define	ATIXL_MSE_READ_BUTTONS		0x00
#define	ATIXL_MSE_READ_X		0x01
#define	ATIXL_MSE_READ_Y		0x02

/* Some nice ATI XL macros */

/* Select IR7, HOLD UPDATES (INT ENABLED), save X,Y */
#define ATIXL_MSE_DISABLE_UPDATE() { outb( 0x07, ATIXL_MSE_CONTROL_PORT ); \
	outb( (0x20 | inb( ATIXL_MSE_DATA_PORT )), ATIXL_MSE_DATA_PORT ); }

/* Select IR7, Enable updates (INT ENABLED) */
#define ATIXL_MSE_ENABLE_UPDATE() { outb( 0x07, ATIXL_MSE_CONTROL_PORT ); \
	 outb( (0xdf & inb( ATIXL_MSE_DATA_PORT )), ATIXL_MSE_DATA_PORT ); }

/* Select IR7 - Mode Register, NO INTERRUPTS */
#define ATIXL_MSE_INT_OFF() { outb( 0x07, ATIXL_MSE_CONTROL_PORT ); \
	outb( (0xe7 & inb( ATIXL_MSE_DATA_PORT )), ATIXL_MSE_DATA_PORT ); }

/* Select IR7 - Mode Register, DATA INTERRUPTS ENABLED */
#define ATIXL_MSE_INT_ON() { outb( 0x07, ATIXL_MSE_CONTROL_PORT ); \
	outb( (0x08 | inb( ATIXL_MSE_DATA_PORT )), ATIXL_MSE_DATA_PORT ); }

/* Same general mouse structure */

static struct mouse_status {
	char buttons;
	char latch_buttons;
	int dx;
	int dy;
	int present;
	int ready;
	int active;
	struct wait_queue *wait;
	struct fasync_struct *fasync;
} mouse;

void mouse_interrupt(int irq, void *dev_id, struct pt_regs * regs)
{
	char dx, dy, buttons;

	ATIXL_MSE_DISABLE_UPDATE(); /* Note that interrupts are still enabled */
	outb(ATIXL_MSE_READ_X, ATIXL_MSE_CONTROL_PORT); /* Select IR1 - X movement */
	dx = inb( ATIXL_MSE_DATA_PORT);
	outb(ATIXL_MSE_READ_Y, ATIXL_MSE_CONTROL_PORT); /* Select IR2 - Y movement */
	dy = inb( ATIXL_MSE_DATA_PORT);
	outb(ATIXL_MSE_READ_BUTTONS, ATIXL_MSE_CONTROL_PORT); /* Select IR0 - Button Status */
	buttons = inb( ATIXL_MSE_DATA_PORT);
	if (dx != 0 || dy != 0 || buttons != mouse.latch_buttons) {
		add_mouse_randomness((buttons << 16) + (dy << 8) + dx);
		mouse.latch_buttons |= buttons;
		mouse.dx += dx;
		mouse.dy += dy;
		mouse.ready = 1;
		wake_up_interruptible(&mouse.wait);
		if (mouse.fasync)
			kill_fasync(mouse.fasync, SIGIO);
	}
	ATIXL_MSE_ENABLE_UPDATE();
}

static int fasync_mouse(struct inode *inode, struct file *filp, int on)
{
	int retval;

	retval = fasync_helper(inode, filp, on, &mouse.fasync);
	if (retval < 0)
		return retval;
	return 0;
}

static int release_mouse(struct inode * inode, struct file * file)
{
	fasync_mouse(inode, file, 0);
	if (--mouse.active)
		return 0;
	ATIXL_MSE_INT_OFF(); /* Interrupts are really shut down here */
	mouse.ready = 0;
	free_irq(ATIXL_MOUSE_IRQ, NULL);
	MOD_DEC_USE_COUNT;
	return 0;
}

static int open_mouse(struct inode * inode, struct file * file)
{
	if (!mouse.present)
		return -EINVAL;
	if (mouse.active++)
		return 0;
	if (request_irq(ATIXL_MOUSE_IRQ, mouse_interrupt, 0, "ATIXL mouse", NULL)) {
		mouse.active--;
		return -EBUSY;
	}
	mouse.ready = 0;
	mouse.dx = 0;
	mouse.dy = 0;
	mouse.buttons = mouse.latch_buttons = 0;
	ATIXL_MSE_INT_ON(); /* Interrupts are really enabled here */
	MOD_INC_USE_COUNT;
	return 0;
}


static long write_mouse(struct inode * inode, struct file * file,
	const char * buffer, unsigned long count)
{
	return -EINVAL;
}

static long read_mouse(struct inode * inode, struct file * file,
	char * buffer, unsigned long count)
{
	int i;

	if (count < 3)
		return -EINVAL;
	if (!mouse.ready)
		return -EAGAIN;
	ATIXL_MSE_DISABLE_UPDATE();
	/* Allowed interrupts to occur during data gathering - shouldn't hurt */
	put_user((char)(~mouse.latch_buttons&7) | 0x80 , buffer);
	if (mouse.dx < -127)
		mouse.dx = -127;
	if (mouse.dx > 127)
		mouse.dx =  127;
	put_user((char)mouse.dx, buffer + 1);
	if (mouse.dy < -127)
		mouse.dy = -127;
	if (mouse.dy > 127)
		mouse.dy =  127;
	put_user((char)-mouse.dy, buffer + 2);
	for(i = 3; i < count; i++)
		put_user(0x00, buffer + i);
	mouse.dx = 0;
	mouse.dy = 0;
	mouse.latch_buttons = mouse.buttons;
	mouse.ready = 0;
	ATIXL_MSE_ENABLE_UPDATE();
	return i; /* i data bytes returned */
}

static unsigned int mouse_poll(struct file *file, poll_table * wait)
{
	poll_wait(&mouse.wait, wait);
	if (mouse.ready)
		return POLLIN | POLLRDNORM;
	return 0;
}

struct file_operations atixl_busmouse_fops = {
	NULL,		/* mouse_seek */
	read_mouse,
	write_mouse,
	NULL, 		/* mouse_readdir */
	mouse_poll, 	/* mouse_poll */
	NULL, 		/* mouse_ioctl */
	NULL,		/* mouse_mmap */
	open_mouse,
	release_mouse,
	NULL,
	fasync_mouse,
};

static struct miscdevice atixl_mouse = { 
	ATIXL_BUSMOUSE, "atixl", &atixl_busmouse_fops
};


__initfunc(int atixl_busmouse_init(void))
{
	unsigned char a,b,c;

	a = inb( ATIXL_MSE_SIGNATURE_PORT );	/* Get signature */
	b = inb( ATIXL_MSE_SIGNATURE_PORT );
	c = inb( ATIXL_MSE_SIGNATURE_PORT );
	if (( a != b ) && ( a == c ))
		printk(KERN_INFO "\nATI Inport ");
	else{
		mouse.present = 0;
		return -EIO;
	}
	outb(0x80, ATIXL_MSE_CONTROL_PORT);	/* Reset the Inport device */
	outb(0x07, ATIXL_MSE_CONTROL_PORT);	/* Select Internal Register 7 */
	outb(0x0a, ATIXL_MSE_DATA_PORT);	/* Data Interrupts 8+, 1=30hz, 2=50hz, 3=100hz, 4=200hz rate */
	mouse.present = 1;
	mouse.active = 0;
	mouse.ready = 0;
	mouse.buttons = mouse.latch_buttons = 0;
	mouse.dx = mouse.dy = 0;
	mouse.wait = NULL;
	printk("Bus mouse detected and installed.\n");
	misc_register(&atixl_mouse);
	return 0;
}

#ifdef MODULE

int init_module(void)
{
	return atixl_busmouse_init();
}

void cleanup_module(void)
{
	misc_deregister(&atixl_mouse);
}
#endif
