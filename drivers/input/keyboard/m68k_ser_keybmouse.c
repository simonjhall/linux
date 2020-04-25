/*
 *  atakbd.c
 *
 *  Copyright (c) 2005 Michael Schmitz
 *
 * Based on amikbd.c, which is
 *
 *  Copyright (c) 2000-2001 Vojtech Pavlik
 *
 *  Based on the work of:
 *	Hamish Macdonald
 */

/*
 * Atari keyboard driver for Linux/m68k
 *
 * The low level init and interrupt stuff is handled in arch/mm68k/atari/atakeyb.c
 * (the keyboard ACIA also handles the mouse and joystick data, and the keyboard
 * interrupt is shared with the MIDI ACIA so MIDI data also get handled there).
 * This driver only deals with handing key events off to the input layer.
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/interrupt.h>


#include <asm/irq.h>

MODULE_AUTHOR("Michael Schmitz <schmitz@biophys.uni-duesseldorf.de>");
MODULE_DESCRIPTION("Atari keyboard driver");
MODULE_LICENSE("GPL");


static unsigned char atakbd_keycode[0x72] = {	/* American layout */
	[0]	 = KEY_GRAVE,
	[1]	 = KEY_ESC,
	[2]	 = KEY_1,
	[3]	 = KEY_2,
	[4]	 = KEY_3,
	[5]	 = KEY_4,
	[6]	 = KEY_5,
	[7]	 = KEY_6,
	[8]	 = KEY_7,
	[9]	 = KEY_8,
	[10]	 = KEY_9,
	[11]	 = KEY_0,
	[12]	 = KEY_MINUS,
	[13]	 = KEY_EQUAL,
	[14]	 = KEY_BACKSPACE,
	[15]	 = KEY_TAB,
	[16]	 = KEY_Q,
	[17]	 = KEY_W,
	[18]	 = KEY_E,
	[19]	 = KEY_R,
	[20]	 = KEY_T,
	[21]	 = KEY_Y,
	[22]	 = KEY_U,
	[23]	 = KEY_I,
	[24]	 = KEY_O,
	[25]	 = KEY_P,
	[26]	 = KEY_LEFTBRACE,
	[27]	 = KEY_RIGHTBRACE,
	[28]	 = KEY_ENTER,
	[29]	 = KEY_LEFTCTRL,
	[30]	 = KEY_A,
	[31]	 = KEY_S,
	[32]	 = KEY_D,
	[33]	 = KEY_F,
	[34]	 = KEY_G,
	[35]	 = KEY_H,
	[36]	 = KEY_J,
	[37]	 = KEY_K,
	[38]	 = KEY_L,
	[39]	 = KEY_SEMICOLON,
	[40]	 = KEY_APOSTROPHE,
	[41]	 = KEY_BACKSLASH,	/* FIXME, '#' */
	[42]	 = KEY_LEFTSHIFT,
	[43]	 = KEY_GRAVE,		/* FIXME: '~' */
	[44]	 = KEY_Z,
	[45]	 = KEY_X,
	[46]	 = KEY_C,
	[47]	 = KEY_V,
	[48]	 = KEY_B,
	[49]	 = KEY_N,
	[50]	 = KEY_M,
	[51]	 = KEY_COMMA,
	[52]	 = KEY_DOT,
	[53]	 = KEY_SLASH,
	[54]	 = KEY_RIGHTSHIFT,
	[55]	 = KEY_KPASTERISK,
	[56]	 = KEY_LEFTALT,
	[57]	 = KEY_SPACE,
	[58]	 = KEY_CAPSLOCK,
	[59]	 = KEY_F1,
	[60]	 = KEY_F2,
	[61]	 = KEY_F3,
	[62]	 = KEY_F4,
	[63]	 = KEY_F5,
	[64]	 = KEY_F6,
	[65]	 = KEY_F7,
	[66]	 = KEY_F8,
	[67]	 = KEY_F9,
	[68]	 = KEY_F10,
	[69]	 = KEY_ESC,
	[70]	 = KEY_DELETE,
	[71]	 = KEY_KP7,
	[72]	 = KEY_KP8,
	[73]	 = KEY_KP9,
	[74]	 = KEY_KPMINUS,
	[75]	 = KEY_KP4,
	[76]	 = KEY_KP5,
	[77]	 = KEY_KP6,
	[78]	 = KEY_KPPLUS,
	[79]	 = KEY_KP1,
	[80]	 = KEY_KP2,
	[81]	 = KEY_KP3,
	[82]	 = KEY_KP0,
	[83]	 = KEY_KPDOT,
	[90]	 = KEY_KPLEFTPAREN,
	[91]	 = KEY_KPRIGHTPAREN,
	[92]	 = KEY_KPASTERISK,	/* FIXME */
	[93]	 = KEY_KPASTERISK,
	[94]	 = KEY_KPPLUS,
	[95]	 = KEY_HELP,
	[96]	 = KEY_102ND,
	[97]	 = KEY_KPASTERISK,	/* FIXME */
	[98]	 = KEY_KPSLASH,
	[99]	 = KEY_KPLEFTPAREN,
	[100]	 = KEY_KPRIGHTPAREN,
	[101]	 = KEY_KPSLASH,
	[102]	 = KEY_KPASTERISK,
	[103]	 = KEY_UP,
	[104]	 = KEY_KPASTERISK,	/* FIXME */
	[105]	 = KEY_LEFT,
	[106]	 = KEY_RIGHT,
	[107]	 = KEY_KPASTERISK,	/* FIXME */
	[108]	 = KEY_DOWN,
	[109]	 = KEY_KPASTERISK,	/* FIXME */
	[110]	 = KEY_KPASTERISK,	/* FIXME */
	[111]	 = KEY_KPASTERISK,	/* FIXME */
	[112]	 = KEY_KPASTERISK,	/* FIXME */
	[113]	 = KEY_KPASTERISK	/* FIXME */
};

static struct input_dev *atakbd_dev;
static struct input_dev *atamouse_dev;


#define SERIAL_TIMER_VALUE (HZ / 20)
#define BASE_ADDRESS (0xf0000000)

#define KEYB_MOUSE_DATA_OFFSET 0x604
//UART 0
#define UART_DATA_OFFSET 0x580
#define UART_STATUS_OFFSET 0x584
//UART 1
//#define UART_DATA_OFFSET 0x588
//#define UART_STATUS_OFFSET 0x58C

#ifdef CONFIG_VM68K
#define VM_KEYB
#endif

#ifdef CONFIG_REDUX
#define REAL_KEYB
#endif

static DEFINE_SPINLOCK(timer_lock);
static struct timer_list serial_timer;

static void keybmouse_poll(struct timer_list *unused)
{
	volatile unsigned int *pVmData = (volatile int *)(BASE_ADDRESS + KEYB_MOUSE_DATA_OFFSET);
	volatile unsigned char *pRealData = (volatile char *)(BASE_ADDRESS + UART_DATA_OFFSET);
	volatile unsigned char *pRealHaveData = (volatile char *)(BASE_ADDRESS + UART_STATUS_OFFSET);

	spin_lock(&timer_lock);

	unsigned int d = 0;
	unsigned char c[4];

#ifdef VM_KEYB
	while (d = *pVmData)
	{
		//keyboard
		if (d & (1 << 31))
		{
			unsigned char scancode = d & 0xff;
			unsigned int down = (d >> 8) & 1;

			input_report_key(atakbd_dev, scancode, down);
			input_sync(atakbd_dev);
		}
		else if (d & (1 << 30))	//mouse motion
		{
			char relX = (char)(d & 0xff);
			char relY = (char)((d & 0xff00) >> 8);

			input_report_rel(atamouse_dev, REL_X, relX);
			input_report_rel(atamouse_dev, REL_Y, relY);

			input_report_key(atamouse_dev, BTN_LEFT, d & (1 << 16));
			input_report_key(atamouse_dev, BTN_RIGHT, d & (1 << 17));

			input_sync(atamouse_dev);
		}
	}
#endif

#ifdef REAL_KEYB
	while (!(*pRealHaveData & (1 << 2)))
	{
		c[0] = *pRealData;

		while ((*pRealHaveData & (1 << 2)));
		c[1] = *pRealData;

		while ((*pRealHaveData & (1 << 2)));
		c[2] = *pRealData;

		while ((*pRealHaveData & (1 << 2)));
		c[3] = *pRealData;

		d = (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | (c[3] << 0);

		//keyboard
		if (d & (1 << 31))
		{
			unsigned char scancode = d & 0xff;
			unsigned int down = (d >> 8) & 1;

			input_report_key(atakbd_dev, scancode, down);
			input_sync(atakbd_dev);
		}
		else if (d & (1 << 30))	//mouse motion
		{
			char relX = (char)(d & 0xff);
			char relY = (char)((d & 0xff00) >> 8);

			input_report_rel(atamouse_dev, REL_X, relX);
			input_report_rel(atamouse_dev, REL_Y, relY);

			input_report_key(atamouse_dev, BTN_LEFT, d & (1 << 16));
			input_report_key(atamouse_dev, BTN_RIGHT, d & (1 << 17));

			input_sync(atamouse_dev);
		}
	}
#endif

	mod_timer(&serial_timer, jiffies + SERIAL_TIMER_VALUE);
	spin_unlock(&timer_lock);
}

static int __init atakbd_init(void)
{
	int i, error;

	atakbd_dev = input_allocate_device();
	if (!atakbd_dev)
		return -ENOMEM;

	atakbd_dev->name = "SDL keyboard";
	atakbd_dev->phys = "sdlkbd/input0";
	atakbd_dev->id.bustype = BUS_HOST;
	atakbd_dev->id.vendor = 0x0001;
	atakbd_dev->id.product = 0x0001;
	atakbd_dev->id.version = 0x0100;

	atakbd_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REP);
	atakbd_dev->keycode = atakbd_keycode;
	atakbd_dev->keycodesize = sizeof(unsigned char);
	atakbd_dev->keycodemax = ARRAY_SIZE(atakbd_keycode);

	for (i = 1; i < 0x72; i++) {
		set_bit(atakbd_keycode[i], atakbd_dev->keybit);
	}

	/* error check */
	error = input_register_device(atakbd_dev);
	if (error) {
		input_free_device(atakbd_dev);
		return error;
	}

	///////////////////////////
	atamouse_dev = input_allocate_device();
	if (!atamouse_dev)
		return -ENOMEM;

	atamouse_dev->name = "SDL mouse";
	atamouse_dev->phys = "sdlmouse/input0";
	atamouse_dev->id.bustype = BUS_HOST;
	atamouse_dev->id.vendor = 0x0001;
	atamouse_dev->id.product = 0x0002;
	atamouse_dev->id.version = 0x0100;

	atamouse_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
	atamouse_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);
	atamouse_dev->keybit[BIT_WORD(BTN_LEFT)] = BIT_MASK(BTN_LEFT) |
		BIT_MASK(BTN_MIDDLE) | BIT_MASK(BTN_RIGHT);

	error = input_register_device(atamouse_dev);
	if (error) {
		input_free_device(atamouse_dev);
		return error;
	}
	///////////////////////////

	spin_lock_bh(&timer_lock);

	timer_setup(&serial_timer, keybmouse_poll, 0);
	mod_timer(&serial_timer, jiffies + SERIAL_TIMER_VALUE);

	spin_unlock_bh(&timer_lock);

	return 0;
}

static void __exit atakbd_exit(void)
{
	input_unregister_device(atakbd_dev);
}

module_init(atakbd_init);
module_exit(atakbd_exit);

