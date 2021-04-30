#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clocksource.h>
#include <linux/rtc.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <asm/setup.h>
#include <asm/pgtable.h>
#include <asm/machdep.h>

static irq_handler_t timer_interrupt;

static irqreturn_t hw_tick(int irq, void *dummy)
{
	return timer_interrupt(irq, dummy);
}

static struct irqaction ham_redux_timer_irq = {
	.name = "timer",
	.flags = IRQF_TIMER,
	.handler = hw_tick,
};

void hw_timer_init(irq_handler_t handler)
{
	setup_irq(1, &ham_redux_timer_irq);
	timer_interrupt = handler;
}

void __init config_BSP(char *command, int len)
{
        mach_sched_init = hw_timer_init;
}

static struct platform_device rtc_device = {
	.name	= "rtc-ds1307",
	.id	= -1,
};

static struct i2c_board_info simon_i2c_info[] __initdata = {
	{
		I2C_BOARD_INFO("ds3231", 0x68),
	},
};

static struct platform_device *simon_devices[] __initdata = {
	&rtc_device,
};

static int __init init_simon(void)
{
#ifdef CONFIG_REDUX
	/* Add i2c RTC Dallas chip supprt */
	i2c_register_board_info(0, simon_i2c_info,
				ARRAY_SIZE(simon_i2c_info));

	platform_add_devices(simon_devices, ARRAY_SIZE(simon_devices));
#endif
	return 0;
}

arch_initcall(init_simon);
