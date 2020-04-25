#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clocksource.h>
#include <linux/rtc.h>
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

