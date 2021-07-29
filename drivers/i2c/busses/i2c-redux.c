/*
 *  ARM IOC/IOMD i2c driver.
 *
 *  Copyright (C) 2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  On Acorn machines, the following i2c devices are on the bus:
 *	- PCF8583 real time clock & static RAM
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/io.h>

//#include <mach/hardware.h>
//#include <asm/hardware/ioc.h>

//#define FORCE_ONES	0xdc
#define SCL		0x01
#define SDA		0x02

#define BASE_ADDRESS (0xf0000000)
#define I2C_BASE 0x584

/*
 * We must preserve all non-i2c output bits in IOC_CONTROL.
 * Note also that we need to preserve the value of SCL and
 * SDA outputs as well (which may be different from the
 * values read back from IOC_CONTROL).
 */
//static u_int force_ones;
static unsigned char i2c_state;

static void ioc_setscl(void *data, int state)
{
 	volatile unsigned char *pI2c = (volatile unsigned char *)(BASE_ADDRESS + I2C_BASE);
 	
 	i2c_state &= ~SCL;
 	if (state)
 		i2c_state = i2c_state | SCL;
 	
 	*pI2c = i2c_state;
}

static void ioc_setsda(void *data, int state)
{
	volatile unsigned char *pI2c = (volatile unsigned char *)(BASE_ADDRESS + I2C_BASE);
 	
 	i2c_state &= ~SDA;
 	if (state)
 		i2c_state = i2c_state | SDA;
 	
 	*pI2c = i2c_state;
}

static int ioc_getscl(void *data)
{
	return (i2c_state & SCL) ? 1 : 0;
}

static int ioc_getsda(void *data)
{
	volatile unsigned char *pI2c = (volatile unsigned char *)(BASE_ADDRESS + I2C_BASE);
 	unsigned char existing_value = *pI2c;
 	
 	return (existing_value & SDA) ? 1 : 0;
}

static struct i2c_algo_bit_data ioc_data = {
	.setsda		= ioc_setsda,
	.setscl		= ioc_setscl,
	.getsda		= ioc_getsda,
	.getscl		= ioc_getscl,
	.udelay		= 80,
	.timeout	= HZ,
};

static struct i2c_adapter ioc_ops = {
	.nr			= 0,
	.algo_data		= &ioc_data,
	.name			= "Big Ham Redux I2C bus",
};

static int __init i2c_ioc_init(void)
{
	//force_ones = FORCE_ONES | SCL | SDA;
	
	volatile unsigned char *pI2c = (volatile unsigned char *)(BASE_ADDRESS + I2C_BASE);
	*pI2c = SCL | SDA;
	i2c_state = SCL | SDA;

	return i2c_bit_add_numbered_bus(&ioc_ops);
}

module_init(i2c_ioc_init);

MODULE_AUTHOR("Russell King <linux@armlinux.org.uk>");
MODULE_DESCRIPTION("ARM IOC/IOMD i2c driver");
MODULE_LICENSE("GPL v2");
