/*
* A sample, extra-simple block driver. Updated for kernel 2.6.31.
*
* (C) 2003 Eklektix, Inc.
* (C) 2010 Pat Patterson
* Redistributable under the terms of the GNU GPL.
*/

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h> /* printk() */
#include <linux/fs.h>     /* everything... */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>

#include <linux/version.h>

MODULE_LICENSE("Dual BSD/GPL");
static char *Version __attribute__((unused)) = "1.5";

static int major_num = 0;
module_param(major_num, int, 0);

#define LOGICAL_BLOCK_SIZE 512
//static unsigned long long logical_block_size = 512;
//module_param(logical_block_size, int, 0);

static unsigned long long nsectors = 16 * 1024 * 1024; /* How big the drive is */
//module_param(nsectors, int, 0);

//#define BASE_ADDRESS (128 * 1024 * 1024)
//#define BASE_ADDRESS (0xf0000000)
//#define BASE_ADDRESS (0x300000)
#define BASE_ADDRESS (0xf0000000)

//fake
#define VM_DATA_OFFSET 0x508
#define VM_SEEK_OFFSET 0x500

//real thing
#define CF_DATA_OFFSET 0x500

//USB UART
#define UART_DATA_OFFSET 0x580
#define UART_STATUS_OFFSET 0x584

#ifdef CONFIG_VM68K
#define BLOCK_VM
#endif

#ifdef CONFIG_REDUX
// #define BLOCK_CF
#endif

#define BLOCK_FT245

#define CF_STARTING_SECTOR 135168

struct Cf
{
	unsigned int m_data;
	union {
		const unsigned int m_error;
		unsigned int m_features;
	};
	unsigned int m_sectorCount;
	unsigned int m_lba0_7;
	unsigned int m_lba8_15;
	unsigned int m_lba16_23;
	unsigned int m_lba24_27;
	union {
		const unsigned int m_status;
		unsigned int m_command;
	};
};

static void cf_wait(void)
{
	volatile unsigned char *pStatus = (volatile unsigned char *)(BASE_ADDRESS | 0x51c);

	while (*pStatus & (1 << 7))
		;
	while (!(*pStatus & (1 << 6)))
		;
}

static unsigned long long read_sector(unsigned char *pOut, unsigned int lba, unsigned long long num_sects)
{
	int count, i;
	volatile struct Cf *pCf = (struct Cf *)(BASE_ADDRESS + CF_DATA_OFFSET);
	unsigned short *pShortOut = (unsigned short *)pOut;

	unsigned int n;
	if (num_sects > 255)
		n = 255;
	else
		n = num_sects;

	//printk("read sector %x %x %x %x\n", (lba >> 24) & 0xf, (lba >> 16) & 0xff, (lba >> 8) & 0xff, lba & 0xff);

	//read a sector
	pCf->m_sectorCount = n << 24;

	pCf->m_lba0_7 = (lba & 0xff) << 24;
	pCf->m_lba8_15 = ((lba >> 8) & 0xff) << 24;
	pCf->m_lba16_23 = ((lba >> 16) & 0xff) << 24;
	pCf->m_lba24_27 = (0xe0 | ((lba >> 24) & 0xf)) << 24;

	//printk("wait for idle after lba written\n");
	cf_wait();

	//printk("wait for idle before write\n");
	pCf->m_command = 0x20 << 24;

	for (count = 0; count < n; count++)
	{
		cf_wait();

		for (i = 0; i < 256; i++)
			*pShortOut++ = pCf->m_data >> 16;
	}

	return n;
}

static void cf_print_sector(unsigned char *pBuf, unsigned int lba)
{
	int x, y;
	lba = lba * 512;

	for (x = 0; x < 32; x++)
	{
		unsigned char *c = &pBuf[x * 16];

		printk("%08x  ", lba + x * 16);

		for (y = 0; y < 16; y++)
		{
			printk(KERN_CONT "%02x ", c[y]);

			if (y == 7)
				printk(KERN_CONT " ");
		}

		printk(KERN_CONT " |");

		for (y = 0; y < 16; y++)
		{
			if (c[y] >= 32 && c[y] < 127)
				printk(KERN_CONT "%c", c[y]);
			else
				printk(KERN_CONT ".");
		}

		printk(KERN_CONT "|\n");
	}
}

// #pragma GCC push_options
// #pragma GCC optimize ("-O1")

void ft245_write_byte(unsigned char b)
{
	volatile char *pData = (volatile char *)(BASE_ADDRESS + UART_DATA_OFFSET);
	volatile char *pState = (volatile char *)(BASE_ADDRESS + UART_STATUS_OFFSET);

	barrier();

	while (*pState & (1 << 3))			//no space
	{
		// __asm__ __volatile__ ("nop");
		barrier();
	}

	barrier();
	// __asm__ __volatile__ ("nop");
	*pData = b;
	__asm__ __volatile__ ("nop");
	barrier();
}

// # pragma GCC push_options
// # pragma GCC optimize ("-Os")

void ft245_read_bytes(unsigned char *pOut, unsigned int num_bytes)
{
	volatile char *pData = (volatile char *)(BASE_ADDRESS + UART_DATA_OFFSET);
	volatile char *pState = (volatile char *)(BASE_ADDRESS + UART_STATUS_OFFSET);

	int count;

	for (count = 0; count < num_bytes; count++)
	{
		barrier();
		while (*pState & (1 << 2))			//no data
		{
			__asm__ __volatile__ ("nop");
			barrier();
		}

		barrier();
		pOut[count] = *pData;
		barrier();
	}
}

static DEFINE_SPINLOCK(ft245_lock);

unsigned long long read_sectors_ft245(unsigned char *pOut, unsigned int lba, unsigned long long num_sects)
{
	unsigned int n;
	unsigned int count;

	unsigned long flags;
	// printk("s");
	spin_lock_irqsave(&ft245_lock, flags);

	if (num_sects > 255)
		n = 255;
	else
		n = num_sects;

	// printk(KERN_CONT "0");
	ft245_write_byte(n);
	// printk(KERN_CONT "1");
	ft245_write_byte(lba & 0xff);
	// printk(KERN_CONT "2");
	ft245_write_byte((lba >> 8) & 0xff);
	// printk(KERN_CONT "3");
	ft245_write_byte((lba >> 16) & 0xff);
	// printk(KERN_CONT "4");
	ft245_write_byte((lba >> 24) & 0xf);
	// printk(KERN_CONT "r\n");

	ft245_read_bytes(pOut, 512 * n);
	// for (count = 0; count < n; count++)
	// 	cf_print_sector(&pOut[512 * count], lba + count);

	spin_unlock_irqrestore(&ft245_lock, flags);

	return n;
}

/*
* We can tweak our hardware sector size, but the kernel talks to us
* in terms of small sectors, always.
*/
#define KERNEL_SECTOR_SIZE 512

/*
* The internal representation of our device.
*/
//https://prog.world/linux-kernel-5-0-we-write-simple-block-device-under-blk-mq/
static struct sbd_device
{
	unsigned long long size;
	atomic_t open_counter; // How many openers

	struct blk_mq_tag_set tag_set;
	struct request_queue * queue; // For mutual exclusion
	struct gendisk *gd;
} Device;

/*
* Handle an I/O request.
*/
static void sbd_transfer(struct sbd_device *dev, sector_t sector, unsigned long long nsect, char *buffer, int write)
{
	unsigned long long offset = sector * LOGICAL_BLOCK_SIZE;
	unsigned long long nbytes = nsect * LOGICAL_BLOCK_SIZE;
	int count;

#ifdef BLOCK_VM
	volatile unsigned long long *pOffset = (volatile long long *)(BASE_ADDRESS + VM_SEEK_OFFSET);
	volatile char *pData = (volatile char *)(BASE_ADDRESS + VM_DATA_OFFSET);

	if ((offset + nbytes) > dev->size)
	{
		printk (KERN_NOTICE "sbd: Beyond-end write (%lld %lld, %lld)\n",
			offset, nbytes, dev->size);
		return;
	}

	//fake vm
	if (write)
	{
		//printk("WRITE of %lld bytes at offset %lld\n", nbytes, offset);

		*pOffset = offset;

		for (count = 0; count < nbytes; count++)
			*pData = *buffer++;
	}
	else
	{
		//printk("READ of %ld bytes from offset %ld\n", nbytes, offset);

		*pOffset = offset;

		for (count = 0; count < nbytes; count++)
			*buffer++ = *pData;
	}
#endif

#ifdef BLOCK_CF

	//cf
	if (write)
	{
		//printk("WRITE of %lld bytes at offset %lld\n", nbytes, offset);
	}
	else
	{
		/*unsigned int count;
		for (count = 0; count < nsect; count++)
		{
			unsigned char *p = (unsigned char *)buffer + count * LOGICAL_BLOCK_SIZE;
			read_sector(p, sector + count + CF_STARTING_SECTOR);
		}*/

		while (nsect)
		{
			unsigned long long r = read_sector(buffer, sector + CF_STARTING_SECTOR, nsect);

			nsect -= r;
			sector += r;
			buffer += r * LOGICAL_BLOCK_SIZE;
		}
	}
#endif

#ifdef BLOCK_FT245
	if (write)
	{
		// printk("WRITE of %lld bytes at offset %lld\n", nbytes, offset);
	}
	else
	{
		// printk("READ of %lld bytes at offset %lld\n", nbytes, offset);

		while (nsect)
		{
			unsigned long long r = read_sectors_ft245(buffer, sector, nsect);

			nsect -= r;
			sector += r;
			buffer += r * LOGICAL_BLOCK_SIZE;
		}

	}
	// printk("transfer finished\n");
#endif
}

//static void sbd_transfer(struct sbd_device *dev, sector_t sector, unsigned long long nsect, char *buffer, int write);

static int do_simple_request (struct request * rq, unsigned int * nr_bytes)
{
	int ret = 0;
	struct bio_vec bvec;
	struct req_iterator iter;
	struct sbd_device *dev = rq-> q-> queuedata;

	loff_t pos = blk_rq_pos (rq);

	rq_for_each_segment(bvec, rq, iter)
	{
		void* b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

		BUG_ON(bvec.bv_len & 511);

		sbd_transfer(dev, pos, bvec.bv_len >> SECTOR_SHIFT, b_buf, rq_data_dir (rq) ? 1 : 0);
		pos += bvec.bv_len >> SECTOR_SHIFT;

		*nr_bytes += bvec.bv_len;
	}

	return ret;
}

/*
* The HDIO_GETGEO ioctl is handled in blkdev_ioctl(), which
* calls this. We need to implement getgeo, since we can’t
* use tools such as fdisk to partition the drive otherwise.
*/
int sbd_getgeo(struct block_device * block_device, struct hd_geometry * geo)
{
	//long size;

	/* We have no real geometry, of course, so make something up. */
	//size = Device.size * (LOGICAL_BLOCK_SIZE / KERNEL_SECTOR_SIZE);

	geo->cylinders = 0xffff;//(size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;

	return 0;
}

/*
* The device operations structure.
*/
static struct block_device_operations sbd_ops =
{
	.owner = THIS_MODULE,
	.getgeo = sbd_getgeo,
};

static blk_status_t queue_rq (struct blk_mq_hw_ctx * hctx, const struct blk_mq_queue_data * bd)
{
	blk_status_t status = BLK_STS_OK;
	struct request * rq = bd-> rq;

	blk_mq_start_request (rq);

	// we can't use that thread
	{
		unsigned int nr_bytes = 0;

		if (do_simple_request (rq, &nr_bytes) != 0)
			status = BLK_STS_IOERR;

		//printk (KERN_WARNING "sblkdev: request process% d bytes  n", nr_bytes);

		if (blk_update_request (rq, status, nr_bytes)) // GPL-only symbol
			BUG ();
		__blk_mq_end_request (rq, status);
	}

	return BLK_STS_OK; // always return ok
}

static struct blk_mq_ops _mq_ops = {
	.queue_rq = queue_rq,
};

// types
typedef struct sblkdev_cmd_s
{
    //nothing
} sblkdev_cmd_t;

static int __init sbd_init(void)
{
	/*
	* Set up our internal device.
	*/
	Device.tag_set.cmd_size = sizeof (sblkdev_cmd_t);
	Device.tag_set.driver_data = &Device;

	Device.size = nsectors * LOGICAL_BLOCK_SIZE;
	Device.queue = blk_mq_init_sq_queue (&Device.tag_set, & _mq_ops, 128, BLK_MQ_F_SHOULD_MERGE);
	if (Device.queue == NULL)
		goto out;

	Device.queue->queuedata = &Device;

	/*
	* Get registered.
	*/
	major_num = register_blkdev(major_num, "ham_uart");

	Device.gd = alloc_disk(1);
	if (!Device.gd)
		goto out_unregister;

	Device.gd->flags |= GENHD_FL_NO_PART_SCAN;
	Device.gd->major = major_num;
	Device.gd->first_minor = 0;
	Device.gd->fops = &sbd_ops;
	Device.gd->private_data = &Device;
	Device.gd->queue = Device.queue;

	strcpy(Device.gd->disk_name, "ham_uart0");

	set_capacity(Device.gd, nsectors);

	add_disk(Device.gd);

	return 0;

	out_unregister:
	unregister_blkdev(major_num, "ham_uart");
	out:

	return -ENOMEM;
}

static void __exit sbd_exit(void)
{
	del_gendisk(Device.gd);
	put_disk(Device.gd);
	unregister_blkdev(major_num, "ham_uart");
	blk_cleanup_queue(Device.queue);
}

module_init(sbd_init);
module_exit(sbd_exit);

