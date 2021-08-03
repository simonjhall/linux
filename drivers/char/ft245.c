/*
 * arch/xtensa/platforms/iss/console.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2001-2005 Tensilica Inc.
 *   Authors	Christian Zankel, Joe Taylor
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/major.h>
#include <linux/param.h>
#include <linux/seq_file.h>
#include <linux/serial.h>


#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clocksource.h>
#include <linux/rtc.h>
#include <linux/sched_clock.h>

#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>

#define CUSTOM_RTC

#include <linux/uaccess.h>
#include <asm/irq.h>

#include <linux/tty.h>
#include <linux/tty_flip.h>

#define SERIAL_MAX_NUM_LINES 1
#define SERIAL_TIMER_VALUE (HZ / 10)
//#define SERIAL_TIMER_VALUE 1

//#define BASE_ADDRESS (128 * 1024 * 1024)
#define BASE_ADDRESS (map_control())
//#define BASE_ADDRESS (0x380000)
//origins
//#define BASE_ADDRESS (0xf0080000)

//sim only
#define RTC_OFFSET 0x608
//real
//UART 0
#if defined CONFIG_VM68K || defined CONFIG_SOC_VMRISCV
#define UART_DATA_OFFSET 0x580
#define UART_STATUS_OFFSET 0x584
#endif

//UART 1
#if defined CONFIG_REDUX || defined CONFIG_SOC_REDUX
#define UART_DATA_OFFSET 0x588
#define UART_STATUS_OFFSET 0x58C
#endif

#define COUNTER_OFFSET 0x600

unsigned char __iomem *g_pControlMembase;

unsigned char __iomem *map_control(void)
{
	if (!g_pControlMembase)
	{
		g_pControlMembase = ioremap(0x1000000, 0x800);
		BUG_ON(!g_pControlMembase);
	}

	return g_pControlMembase;
}

#ifdef CUSTOM_RTC
static u64 notrace ft245_read_cycles(struct clocksource *cs)
{
	volatile u32 *pData = (volatile u32 *)(BASE_ADDRESS + COUNTER_OFFSET);
	return *pData;
}

u32 ft245_read_rtc(void)
{
	volatile u32 *pData = (volatile u32 *)(BASE_ADDRESS + RTC_OFFSET);
	return *pData;
}

static struct clocksource ft245_clk = {
	.name	= "ft245_timer",
	.rating	= 250,
	.read	= ft245_read_cycles,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static u64 notrace ft245_read_sched_clock(void)
{
	return ft245_read_cycles(&ft245_clk);
}

#define CLOCKS_PER_SECOND_REAL 33000000
#define CLOCKS_PER_SECOND_VM 32768

#if defined CONFIG_VM68K || defined CONFIG_SOC_VMRISCV
#define CLOCKS_PER_SECOND CLOCKS_PER_SECOND_VM
#endif

#if defined CONFIG_REDUX || defined CONFIG_SOC_REDUX
#define CLOCKS_PER_SECOND CLOCKS_PER_SECOND_REAL
#endif

int __init ft245_timer_init(void)
{
	int ret;

	ret = clocksource_register_hz(&ft245_clk, CLOCKS_PER_SECOND);
	if (ret) {
		pr_err("32k_counter: can't register clocksource\n");
		return ret;
	}

	sched_clock_register(ft245_read_sched_clock, 32, CLOCKS_PER_SECOND);

	return 0;
}

core_initcall(ft245_timer_init);
#endif

static struct tty_driver *serial_driver;
static struct tty_port serial_port;
static struct timer_list serial_timer;

static DEFINE_SPINLOCK(timer_lock);
static DEFINE_SPINLOCK(uart_lock);

static char *serial_version = "0.1";
static char *serial_name = "FT245 serial driver";

/*
 * This routine is called whenever a serial port is opened.  It
 * enables interrupts for a serial port, linking in its async structure into
 * the IRQ chain.   It also performs the serial-specific
 * initialization for the tty structure.
 */

static void rs_poll(struct timer_list *unused);

static int rs_open(struct tty_struct *tty, struct file * filp)
{
	tty->port = &serial_port;
	spin_lock_bh(&timer_lock);
	if (tty->count == 1) {
		timer_setup(&serial_timer, rs_poll, 0);
		mod_timer(&serial_timer, jiffies + SERIAL_TIMER_VALUE);
	}
	spin_unlock_bh(&timer_lock);

	return 0;
}


/*
 * ------------------------------------------------------------
 * iss_serial_close()
 *
 * This routine is called when the serial port gets closed.  First, we
 * wait for the last remaining data to be sent.  Then, we unlink its
 * async structure from the interrupt chain if necessary, and we free
 * that IRQ if nothing is left in the chain.
 * ------------------------------------------------------------
 */
static void rs_close(struct tty_struct *tty, struct file * filp)
{
	spin_lock_bh(&timer_lock);
	if (tty->count == 1)
		del_timer_sync(&serial_timer);
	spin_unlock_bh(&timer_lock);
}

#define MAX_TRANSMIT 256
#define MAX_RECEIVE 128

int ft245_write(const char *buf, int n)
{
	volatile char *pData = (volatile char *)(BASE_ADDRESS + UART_DATA_OFFSET);
	volatile char *pState = (volatile char *)(BASE_ADDRESS + UART_STATUS_OFFSET);
	
	int written = 0;
	
	if (n > MAX_TRANSMIT)
		n = MAX_TRANSMIT;
	
	//if (*pState & (1 << 3))
	//	return 0;			//no space to transmit

	spin_lock(&uart_lock);

	while (n--)
	{
		//if (*pState & (1 << 3))			//no space
		//	break;
		
		while (*pState & (1 << 3))			//no space
			;
			
		*pData = *buf++;
		written++;
	}

	spin_unlock(&uart_lock);
	
	return written;
}

static int rs_write(struct tty_struct * tty,
		    const unsigned char *buf, int count)
{
	/* see drivers/char/serialX.c to reference original version */

	//simc_write(1, buf, count);

	int written = ft245_write(buf, count);
	tty_wakeup(tty);
	return written;
}

static void rs_poll(struct timer_list *unused)
{
	volatile unsigned char *pHaveData = (volatile char *)(BASE_ADDRESS + UART_STATUS_OFFSET);
	volatile unsigned char *pData = (volatile char *)(BASE_ADDRESS + UART_DATA_OFFSET);
	
	struct tty_port *port = &serial_port;
	int i = 0;
	int rd = 1;
	unsigned char c;

	spin_lock(&timer_lock);

	/*while (simc_poll(0)) {
		rd = simc_read(0, &c, 1);
		if (rd <= 0)
			break;
		tty_insert_flip_char(port, c, TTY_NORMAL);
		i++;
	}*/

	//vm
	
	//while (*pHaveData == 0)
	while (!(*pHaveData & (1 << 2)))		//if data
	{
		c = *pData;
		tty_insert_flip_char(port, c, TTY_NORMAL);
		i++;
		
		if (i == MAX_RECEIVE)
			break;
	}
	
	//origins
	/*
	volatile unsigned short *pUartS = (unsigned short *)BASE_ADDRESS;
	
	while (1)
	{
		unsigned short s = *pUartS;
		if ((s & 1) == 0)
		{
			unsigned char c = s >> 8;
		
			tty_insert_flip_char(port, c, TTY_NORMAL);
			i++;
		}
		else
			break;
	}*/

	if (i)
		tty_flip_buffer_push(port);
	if (rd)
		mod_timer(&serial_timer, jiffies + SERIAL_TIMER_VALUE);
	spin_unlock(&timer_lock);
}


static int rs_put_char(struct tty_struct *tty, unsigned char ch)
{
	return rs_write(tty, &ch, 1);
}

static void rs_flush_chars(struct tty_struct *tty)
{
}

static int rs_write_room(struct tty_struct *tty)
{
	/* Let's say iss can always accept 2K characters.. */
	return 128;
}

static int rs_chars_in_buffer(struct tty_struct *tty)
{
	/* the iss doesn't buffer characters */
	return 0;
}

static void rs_hangup(struct tty_struct *tty)
{
	/* Stub, once again.. */
}

static void rs_wait_until_sent(struct tty_struct *tty, int timeout)
{
	/* Stub, once again.. */
}

static int rs_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "serinfo:1.0 driver:%s\n", serial_version);
	return 0;
}

static const struct tty_operations serial_ops = {
	.open = rs_open,
	.close = rs_close,
	.write = rs_write,
	.put_char = rs_put_char,
	.flush_chars = rs_flush_chars,
	.write_room = rs_write_room,
	.chars_in_buffer = rs_chars_in_buffer,
	.hangup = rs_hangup,
	.wait_until_sent = rs_wait_until_sent,
	.proc_show = &rs_proc_show,
};

int __init rs_init(void)
{
	tty_port_init(&serial_port);

	serial_driver = alloc_tty_driver(SERIAL_MAX_NUM_LINES);

	printk ("%s %s\n", serial_name, serial_version);

	/* Initialize the tty_driver structure */

	serial_driver->driver_name = "ft245_serial";
	serial_driver->name = "ttyS";
	serial_driver->major = TTY_MAJOR;
	serial_driver->minor_start = 64;
	serial_driver->type = TTY_DRIVER_TYPE_SERIAL;
	serial_driver->subtype = SERIAL_TYPE_NORMAL;
	serial_driver->init_termios = tty_std_termios;
	serial_driver->init_termios.c_cflag =
		B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	serial_driver->flags = TTY_DRIVER_REAL_RAW;

	tty_set_operations(serial_driver, &serial_ops);
	tty_port_link_device(&serial_port, serial_driver, 0);

	if (tty_register_driver(serial_driver))
		panic("Couldn't register serial driver\n");
	return 0;
}


static __exit void rs_exit(void)
{
	int error;

	if ((error = tty_unregister_driver(serial_driver)))
		printk("FT245_SERIAL: failed to unregister serial driver (%d)\n",
		       error);
	put_tty_driver(serial_driver);
	tty_port_destroy(&serial_port);
}


/* We use `late_initcall' instead of just `__initcall' as a workaround for
 * the fact that (1) simcons_tty_init can't be called before tty_init,
 * (2) tty_init is called via `module_init', (3) if statically linked,
 * module_init == device_init, and (4) there's no ordering of init lists.
 * We can do this easily because simcons is always statically linked, but
 * other tty drivers that depend on tty_init and which must use
 * `module_init' to declare their init routines are likely to be broken.
 */

late_initcall(rs_init);


static void iss_console_write(struct console *co, const char *s, unsigned count)
{
	int len = strlen(s);

	if (s != 0 && *s != 0)
		ft245_write(s, count < len ? count : len);
}

static struct tty_driver* iss_console_device(struct console *c, int *index)
{
	*index = c->index;
	return serial_driver;
}


static struct console sercons = {
	.name = "ttyS",
	.write = iss_console_write,
	.device = iss_console_device,
	.flags = CON_PRINTBUFFER,
	.index = -1
};

static int __init iss_console_init(void)
{
	register_console(&sercons);
	return 0;
}

console_initcall(iss_console_init);
