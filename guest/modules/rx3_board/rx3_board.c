// SPDX-License-Identifier: GPL-2.0
/*
 * rx3_board.c - the XDJ-RX3's Pioneer character devices, for dj-emu.
 *
 * The XDJ-RX3 player (rbp) runs as ARM32 code under qemu-arm on this AArch64
 * kernel. It opens the board's own Pioneer devices; this module provides them
 * with the same user ABI as the drivers in Pioneer's published GPL kernel
 * (linux-3.0.101, arch/arm/mach-mx6/{subucom_spi,subucom_spi_rdy,gpiodrv,
 * printkdrv}.c). The ABI is reimplemented here; no code is copied.
 *
 *   /dev/subucom_spi1.0, /dev/subucom_spi2.0          128-byte frames, timer
 *   /dev/subucom_spi_rdy3.0, /dev/subucom_spi_rdy4.0  256-byte frames, timer
 *                                                     plus TX_BUF_STAT
 *   /dev/gpiodrv      one byte per GPIO (offset = IMX_GPIO_NR), IRQ ioctls
 *   /dev/printkdrv0   writes go to the kernel log
 *   /dev/hidg0, /dev/paudiog0  the rear USB-B gadget, with no host attached
 *   pwm-backlight.1   the LCD backlight (sysfs, as the i.MX board registers it)
 *   /sys/class/paudiog/paudiog0/connect  the USB-B audio gadget: unplugged
 *   /dev/rx3_subucom  the host side of the four panel links (see below)
 *
 * subucom: on the real unit a kernel thread runs one full-duplex SPI transfer
 * per timer tick: it sends the current TX frame, and if the received frame
 * differs from the last one it latches it and flags rx_detected (poll wakes,
 * read returns it). Here the "bus" is /dev/rx3_subucom:
 *
 *   read()   blocks for the next TX frame any channel sent and returns
 *            struct rx3_link_rec followed by `len` bytes;
 *   write()  takes the same record and makes its bytes the frame that
 *            channel receives on its next tick.
 *
 * ioctl layouts: subucom_ioc_transfer uses fixed-width fields (identical on
 * ARM32 and AArch64). gpioirq_inf is { int gpio; unsigned long trigger; } on
 * the ARM32 caller, so it is read as two u32. qemu-arm forwards these ioctls
 * and translates subucom_ioc_transfer.tx_buf (qemu/patches/).
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/backlight.h>
#include <linux/device.h>

#define SUBUCOM_IOC_MAGIC 'p'
#define NR_MESSAGE 0
#define NR_TIMER_STATUS 1
#define NR_TIMER_INTERVAL 2
#define NR_BITS_PER_WORD 3
#define NR_RX_BYTES 4
#define NR_TX_BUF_STAT 6
#define NR_RDY_COUNT 7

struct subucom_ioc_transfer {
	__u8 multi_id;
	__u8 _reserved;
	__u8 part_id;
	__u8 part_num;
	__u32 bytes;
	__u64 tx_buf;
};

/* One record on /dev/rx3_subucom. */
struct rx3_link_rec {
	__u8 chan;	/* 0: spi1.0, 1: spi2.0, 2: spi_rdy3.0, 3: spi_rdy4.0 */
	__u8 _pad;
	__u16 len;
	__u32 seq;	/* TX: tick counter of that channel */
} __packed;

#define N_CHAN 4
#define MAX_FRAME 256

struct chan {
	const char *name;
	unsigned bufsize;
	bool rdy;
	struct miscdevice misc;
	struct mutex lock;
	wait_queue_head_t wait;		/* poll / rx_detected */
	struct task_struct *thread;
	bool timer_on;
	u32 interval_ms;
	u16 bits_per_word;
	u32 rx_bytes;
	u8 tx_buf_stat;
	u32 rdy_count;
	u8 txnew[MAX_FRAME];		/* last frame userland queued */
	u32 txnew_len;
	bool tx_updated;
	u8 tx[MAX_FRAME];		/* frame sent on each tick */
	u32 tx_len;
	u8 rx_host[MAX_FRAME];		/* what the host wants received */
	u8 rx_old[MAX_FRAME];		/* last latched RX frame */
	bool rx_detected;
	u32 seq;
	/* the newest TX frame not yet read by the host */
	bool host_pending;
	u8 host_frame[MAX_FRAME];
	u32 host_len;
	u32 host_seq;
};

static struct chan chans[N_CHAN] = {
	{ .name = "subucom_spi1.0", .bufsize = 128 },
	{ .name = "subucom_spi2.0", .bufsize = 128 },
	{ .name = "subucom_spi_rdy3.0", .bufsize = 256, .rdy = true },
	{ .name = "subucom_spi_rdy4.0", .bufsize = 256, .rdy = true },
};

static DECLARE_WAIT_QUEUE_HEAD(host_wait);
static DEFINE_SPINLOCK(host_lock);

static struct chan *chan_of(struct file *f)
{
	struct miscdevice *m = f->private_data;

	return container_of(m, struct chan, misc);
}

/* One emulated bus transfer: send the TX frame, latch a changed RX frame. */
static void chan_tick(struct chan *c)
{
	unsigned long flags;

	mutex_lock(&c->lock);
	if (c->tx_updated) {
		memcpy(c->tx, c->txnew, c->txnew_len);
		c->tx_len = c->txnew_len;
		c->tx_updated = false;
	}
	c->seq++;
	if (c->rdy)
		c->tx_buf_stat = 0;
	if (memcmp(c->rx_old, c->rx_host, c->rx_bytes)) {
		memcpy(c->rx_old, c->rx_host, c->rx_bytes);
		c->rx_detected = true;
	}
	spin_lock_irqsave(&host_lock, flags);
	memcpy(c->host_frame, c->tx, c->tx_len);
	c->host_len = c->tx_len;
	c->host_seq = c->seq;
	c->host_pending = true;
	spin_unlock_irqrestore(&host_lock, flags);
	mutex_unlock(&c->lock);
	if (c->rx_detected)
		wake_up_interruptible(&c->wait);
	wake_up_interruptible(&host_wait);
}

static int chan_thread(void *data)
{
	struct chan *c = data;

	while (!kthread_should_stop()) {
		if (!READ_ONCE(c->timer_on)) {
			msleep_interruptible(20);
			continue;
		}
		chan_tick(c);
		msleep(max_t(u32, READ_ONCE(c->interval_ms), 1));
	}
	return 0;
}

static int chan_open(struct inode *inode, struct file *f)
{
	struct chan *c = chan_of(f);

	pr_notice("rx3_board: %s opened %s\n", current->comm, c->name);
	return 0;
}

static ssize_t chan_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	struct chan *c = chan_of(f);
	ssize_t ret;

	if (n > c->bufsize)
		return -EMSGSIZE;
	mutex_lock(&c->lock);
	ret = copy_to_user(buf, c->rx_old, n) ? -EFAULT : n;
	c->rx_detected = false;
	mutex_unlock(&c->lock);
	return ret;
}

static ssize_t chan_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
	/* Timer-off writes are synchronous transfers on the real bus. */
	struct chan *c = chan_of(f);

	if (n > c->bufsize)
		return -EMSGSIZE;
	mutex_lock(&c->lock);
	if (copy_from_user(c->txnew, buf, n)) {
		mutex_unlock(&c->lock);
		return -EFAULT;
	}
	c->txnew_len = n;
	c->tx_updated = true;
	mutex_unlock(&c->lock);
	chan_tick(c);
	return n;
}

static __poll_t chan_poll(struct file *f, poll_table *wait)
{
	struct chan *c = chan_of(f);

	if (!READ_ONCE(c->timer_on))
		return 0;
	poll_wait(f, &c->wait, wait);
	return READ_ONCE(c->rx_detected) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static long chan_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct chan *c = chan_of(f);
	void __user *up = (void __user *)arg;
	bool rd = _IOC_DIR(cmd) & _IOC_READ;
	u32 v = 0;
	long ret = 0;

	if (_IOC_TYPE(cmd) != SUBUCOM_IOC_MAGIC)
		return -ENOTTY;

	if (_IOC_NR(cmd) == NR_MESSAGE) {
		struct subucom_ioc_transfer t;

		if (_IOC_DIR(cmd) != _IOC_WRITE || _IOC_SIZE(cmd) < sizeof(t) ||
		    _IOC_SIZE(cmd) % sizeof(t))
			return -EINVAL;
		/* Like the original, only the first transfer is taken. */
		if (copy_from_user(&t, up, sizeof(t)))
			return -EFAULT;
		if (t.multi_id != 0 || t.part_num > 1 || t.part_id >= max_t(u8, t.part_num, 1) ||
		    t.bytes > c->bufsize)
			return -EINVAL;
		mutex_lock(&c->lock);
		if (copy_from_user(c->txnew, u64_to_user_ptr(t.tx_buf), t.bytes)) {
			ret = -EFAULT;
		} else {
			c->txnew_len = t.bytes;
			c->tx_updated = true;
			if (c->rdy)
				c->tx_buf_stat = 1;
		}
		mutex_unlock(&c->lock);
		return ret;
	}

	if (!rd && copy_from_user(&v, up, _IOC_SIZE(cmd) > 4 ? 4 : _IOC_SIZE(cmd)))
		return -EFAULT;

	mutex_lock(&c->lock);
	switch (_IOC_NR(cmd)) {
	case NR_TIMER_STATUS:
		if (rd) {
			v = c->timer_on;
		} else if (v & ~1u) {
			ret = -EINVAL;
		} else {
			if (v && !c->timer_on) {
				/* Starting the timer clears the buffers, as on the unit. */
				memset(c->rx_old, 0, sizeof(c->rx_old));
				c->rx_detected = false;
			}
			if (!!v != c->timer_on)
				pr_notice("rx3_board: %s timer %s (interval %u ms, rx %u bytes)\n",
					  c->name, v ? "on" : "off", c->interval_ms, c->rx_bytes);
			WRITE_ONCE(c->timer_on, v);
		}
		break;
	case NR_TIMER_INTERVAL:
		if (rd)
			v = c->interval_ms;
		else
			WRITE_ONCE(c->interval_ms, v);
		break;
	case NR_BITS_PER_WORD:
		if (rd)
			v = c->bits_per_word;
		else if (c->timer_on)
			ret = -EBUSY;
		else
			c->bits_per_word = v & 0xffff;
		break;
	case NR_RX_BYTES:
		if (rd)
			v = c->rx_bytes;
		else if (v > c->bufsize)
			ret = -EINVAL;
		else if (c->timer_on)
			ret = -EBUSY;
		else
			c->rx_bytes = v;
		break;
	case NR_TX_BUF_STAT:
		if (!c->rdy)
			ret = -ENOTTY;
		else if (rd)
			v = c->tx_buf_stat;
		else
			c->tx_buf_stat = v & 0xff;
		break;
	case NR_RDY_COUNT:
		if (!c->rdy)
			ret = -ENOTTY;
		else if (rd)
			v = c->rdy_count;
		else
			c->rdy_count = v;
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&c->lock);
	if (ret == 0 && rd) {
		size_t sz = _IOC_SIZE(cmd);

		if (sz == 1) {
			u8 b = v;

			ret = copy_to_user(up, &b, 1) ? -EFAULT : 0;
		} else if (sz == 2) {
			u16 h = v;

			ret = copy_to_user(up, &h, 2) ? -EFAULT : 0;
		} else {
			ret = copy_to_user(up, &v, 4) ? -EFAULT : 0;
		}
	}
	return ret;
}

static const struct file_operations chan_fops = {
	.owner = THIS_MODULE,
	.open = chan_open,
	.read = chan_read,
	.write = chan_write,
	.poll = chan_poll,
	.unlocked_ioctl = chan_ioctl,
	.compat_ioctl = chan_ioctl,
	.llseek = no_llseek,
};

/* ---- /dev/rx3_subucom: the host end of the four links ------------------ */

static ssize_t link_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	struct rx3_link_rec rec = { 0 };
	u8 frame[MAX_FRAME];
	unsigned long flags;
	int i, found = -1;

	for (;;) {
		spin_lock_irqsave(&host_lock, flags);
		for (i = 0; i < N_CHAN; i++) {
			if (chans[i].host_pending) {
				found = i;
				rec.chan = i;
				rec.len = chans[i].host_len;
				rec.seq = chans[i].host_seq;
				memcpy(frame, chans[i].host_frame, rec.len);
				chans[i].host_pending = false;
				break;
			}
		}
		spin_unlock_irqrestore(&host_lock, flags);
		if (found >= 0)
			break;
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(host_wait, chans[0].host_pending ||
					     chans[1].host_pending ||
					     chans[2].host_pending ||
					     chans[3].host_pending))
			return -ERESTARTSYS;
	}
	if (n < sizeof(rec) + rec.len)
		return -EMSGSIZE;
	if (copy_to_user(buf, &rec, sizeof(rec)) ||
	    copy_to_user(buf + sizeof(rec), frame, rec.len))
		return -EFAULT;
	return sizeof(rec) + rec.len;
}

static ssize_t link_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
	struct rx3_link_rec rec;
	struct chan *c;

	if (n < sizeof(rec) || copy_from_user(&rec, buf, sizeof(rec)))
		return -EINVAL;
	if (rec.chan >= N_CHAN || rec.len > chans[rec.chan].bufsize ||
	    n < sizeof(rec) + rec.len)
		return -EINVAL;
	c = &chans[rec.chan];
	mutex_lock(&c->lock);
	memset(c->rx_host, 0, sizeof(c->rx_host));
	if (copy_from_user(c->rx_host, buf + sizeof(rec), rec.len)) {
		mutex_unlock(&c->lock);
		return -EFAULT;
	}
	mutex_unlock(&c->lock);
	return sizeof(rec) + rec.len;
}

static __poll_t link_poll(struct file *f, poll_table *wait)
{
	int i;

	poll_wait(f, &host_wait, wait);
	for (i = 0; i < N_CHAN; i++)
		if (READ_ONCE(chans[i].host_pending))
			return EPOLLIN | EPOLLRDNORM | EPOLLOUT;
	return EPOLLOUT;
}

static const struct file_operations link_fops = {
	.owner = THIS_MODULE,
	.read = link_read,
	.write = link_write,
	.poll = link_poll,
	.llseek = no_llseek,
};

static struct miscdevice link_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "rx3_subucom", .fops = &link_fops, .mode = 0600,
};

/* ---- /dev/gpiodrv ------------------------------------------------------- */

#define GPIODRV_IOC_MAGIC 0xF1
#define N_GPIO 224	/* IMX_GPIO_NR(7, 31) + 1 */

/* USB-A OCS inputs are active low. rbp samples them synchronously from
 * GpioManager's constructor, before its panel message thread exists; a low
 * input posts an overcurrent notification and stalls UI initialization.
 * GPIO4_30 (126) and GPIO7_12 (204) must idle high with no fault.
 */
static u8 gpio_val[N_GPIO] = { [126] = 1, [204] = 1 };
static DEFINE_MUTEX(gpio_lock);

static ssize_t gpio_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	u8 v;

	if (*pos < 0 || *pos >= N_GPIO || n < 1)
		return -EFAULT;
	mutex_lock(&gpio_lock);
	v = gpio_val[*pos];
	mutex_unlock(&gpio_lock);
	return copy_to_user(buf, &v, 1) ? -EFAULT : 1;
}

static ssize_t gpio_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
	u8 v;

	if (*pos < 0 || *pos >= N_GPIO || n < 1)
		return -EFAULT;
	if (copy_from_user(&v, buf, 1))
		return -EFAULT;
	mutex_lock(&gpio_lock);
	gpio_val[*pos] = v ? 1 : 0;
	mutex_unlock(&gpio_lock);
	pr_debug("rx3_board: gpio %lld <- %u\n", *pos, v ? 1 : 0);
	return 1;
}

static long gpio_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	u32 inf[2];	/* ARM32 { int gpio; unsigned long trigger; } */

	if (_IOC_TYPE(cmd) != GPIODRV_IOC_MAGIC)
		return -ENOTTY;
	switch (_IOC_NR(cmd)) {
	case 1: /* SETISR */
	case 2: /* CLRISR */
		if (copy_from_user(inf, (void __user *)arg, sizeof(inf)))
			return -EFAULT;
		if (inf[0] >= N_GPIO)
			return -EINVAL;
		pr_debug("rx3_board: gpio %u irq %s trigger %#x\n", inf[0],
			 _IOC_NR(cmd) == 1 ? "set" : "clear", inf[1]);
		return 0;
	case 3: /* ENABLEIRQ */
	case 4: /* DISABLEIRQ */
		return 0;
	}
	return -ENOTTY;
}

static __poll_t gpio_poll(struct file *f, poll_table *wait)
{
	return 0;	/* No GPIO interrupt sources are emulated yet. */
}

static const struct file_operations gpio_fops = {
	.owner = THIS_MODULE,
	.read = gpio_read,
	.write = gpio_write,
	.unlocked_ioctl = gpio_ioctl,
	.compat_ioctl = gpio_ioctl,
	.poll = gpio_poll,
	.llseek = default_llseek,
};

static struct miscdevice gpio_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "gpiodrv", .fops = &gpio_fops, .mode = 0666,
};

/* ---- /dev/printkdrv0 ---------------------------------------------------- */

static ssize_t printk_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
	char line[256];
	size_t len = min(n, sizeof(line) - 1);

	if (copy_from_user(line, buf, len))
		return -EFAULT;
	line[len] = 0;
	pr_info("rbp: %s", line);
	return n;
}

static ssize_t printk_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	return 0;
}

static const struct file_operations printk_fops = {
	.owner = THIS_MODULE, .write = printk_write, .read = printk_read, .llseek = no_llseek,
};

static struct miscdevice printk_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "printkdrv0", .fops = &printk_fops, .mode = 0666,
};

/* ---- /dev/hidg0, /dev/paudiog0: the rear USB-B gadget, unplugged -------- */

/*
 * On the unit these are USB gadget functions (g_pmulti) behind the rear
 * USB-B "PC link". With no computer attached, reads wait for a host that
 * never comes and writes go nowhere; that is what these do. A PC-link
 * bridge can replace them later.
 */
static DECLARE_WAIT_QUEUE_HEAD(gadget_wait);

static ssize_t gadget_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	if (f->f_flags & O_NONBLOCK)
		return -EAGAIN;
	/* Sleeps until a signal: no USB host will ever send a report. */
	if (wait_event_interruptible(gadget_wait, false))
		return -ERESTARTSYS;
	return 0;
}

static ssize_t gadget_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
	return n;
}

static __poll_t gadget_poll(struct file *f, poll_table *wait)
{
	poll_wait(f, &gadget_wait, wait);
	return EPOLLOUT | EPOLLWRNORM;
}

/* f_paudio.h uses native longs: qemu-arm widens its 12-byte usb_xferi.
 * With no USB host, usb_pcm_read returns a silent 4-channel block and 0.
 */
struct rx3_usb_xferi {
	long result;
	void __user *buf;
	long samples;
};
static long gadget_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct rx3_usb_xferi x;
	if (cmd != _IOR('B', 0x51, struct rx3_usb_xferi))
		return -ENOTTY;
	if (copy_from_user(&x, (void __user *)arg, sizeof(x)))
		return -EFAULT;
	if (x.samples < 0 || x.samples > 4096)
		return -EINVAL;
	if (clear_user(x.buf, x.samples * 16))
		return -EFAULT;
	x.result = 0;
	return copy_to_user((void __user *)arg, &x, sizeof(x)) ? -EFAULT : 0;
}

static const struct file_operations gadget_fops = {
	.owner = THIS_MODULE,
	.read = gadget_read,
	.write = gadget_write,
	.poll = gadget_poll,
	.unlocked_ioctl = gadget_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice hidg_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "hidg0", .fops = &gadget_fops, .mode = 0666,
};
static struct miscdevice paudiog_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "paudiog0", .fops = &gadget_fops, .mode = 0666,
};

/* ---- LCD backlight and USB-B gadget state (sysfs) ------------------------ */

/*
 * The i.MX board registers the panel backlight as platform device
 * pwm-backlight.1, so the player reads
 * /sys/devices/platform/pwm-backlight.1/backlight/pwm-backlight.1/. The
 * USB-B audio gadget reports whether a computer is attached in
 * /sys/class/paudiog/paudiog0/connect.
 */
static struct platform_device *bl_pdev;
static struct backlight_device *bl_dev;
static struct class *paudiog_class;
static struct device *paudiog_dev;

static int bl_update(struct backlight_device *bd)
{
	return 0;
}

static const struct backlight_ops bl_ops = {
	.update_status = bl_update,
};

static ssize_t connect_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static DEVICE_ATTR_RO(connect);

static int board_sysfs_init(void)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW, .max_brightness = 255, .brightness = 255,
	};
	int ret;

	bl_pdev = platform_device_register_simple("pwm-backlight", 1, NULL, 0);
	if (IS_ERR(bl_pdev))
		return PTR_ERR(bl_pdev);
	bl_dev = backlight_device_register("pwm-backlight.1", &bl_pdev->dev, NULL,
					   &bl_ops, &props);
	if (IS_ERR(bl_dev)) {
		ret = PTR_ERR(bl_dev);
		goto err_pdev;
	}
	paudiog_class = class_create("paudiog");
	if (IS_ERR(paudiog_class)) {
		ret = PTR_ERR(paudiog_class);
		goto err_bl;
	}
	paudiog_dev = device_create(paudiog_class, NULL, MKDEV(0, 0), NULL, "paudiog0");
	if (IS_ERR(paudiog_dev)) {
		ret = PTR_ERR(paudiog_dev);
		goto err_class;
	}
	ret = device_create_file(paudiog_dev, &dev_attr_connect);
	if (ret)
		goto err_dev;
	return 0;
err_dev:
	device_destroy(paudiog_class, MKDEV(0, 0));
err_class:
	class_destroy(paudiog_class);
err_bl:
	backlight_device_unregister(bl_dev);
err_pdev:
	platform_device_unregister(bl_pdev);
	return ret;
}

static void board_sysfs_exit(void)
{
	device_remove_file(paudiog_dev, &dev_attr_connect);
	device_destroy(paudiog_class, MKDEV(0, 0));
	class_destroy(paudiog_class);
	backlight_device_unregister(bl_dev);
	platform_device_unregister(bl_pdev);
}

/* ---- module ------------------------------------------------------------- */

static int __init rx3_board_init(void)
{
	int i, ret;

	for (i = 0; i < N_CHAN; i++) {
		struct chan *c = &chans[i];

		mutex_init(&c->lock);
		init_waitqueue_head(&c->wait);
		c->rx_bytes = c->bufsize;
		c->bits_per_word = 8;
		c->interval_ms = 10;
		c->misc = (struct miscdevice){
			.minor = MISC_DYNAMIC_MINOR, .name = c->name, .fops = &chan_fops, .mode = 0666,
		};
		ret = misc_register(&c->misc);
		if (ret)
			goto fail;
		c->thread = kthread_run(chan_thread, c, "rx3_%s", c->name);
		if (IS_ERR(c->thread)) {
			ret = PTR_ERR(c->thread);
			misc_deregister(&c->misc);
			goto fail;
		}
	}
	ret = misc_register(&link_misc);
	if (ret)
		goto fail;
	ret = misc_register(&gpio_misc);
	if (ret)
		goto fail_link;
	ret = misc_register(&printk_misc);
	if (ret)
		goto fail_gpio;
	ret = misc_register(&hidg_misc);
	if (ret)
		goto fail_printk;
	ret = misc_register(&paudiog_misc);
	if (ret)
		goto fail_hidg;
	ret = board_sysfs_init();
	if (ret)
		goto fail_paudiog;
	pr_info("rx3_board: subucom x4, gpiodrv, printkdrv0, hidg0, paudiog0, rx3_subucom ready\n");
	return 0;

fail_paudiog:
	misc_deregister(&paudiog_misc);
fail_hidg:
	misc_deregister(&hidg_misc);
fail_printk:
	misc_deregister(&printk_misc);
fail_gpio:
	misc_deregister(&gpio_misc);
fail_link:
	misc_deregister(&link_misc);
fail:
	while (--i >= 0) {
		kthread_stop(chans[i].thread);
		misc_deregister(&chans[i].misc);
	}
	return ret;
}

static void __exit rx3_board_exit(void)
{
	int i;

	board_sysfs_exit();
	misc_deregister(&paudiog_misc);
	misc_deregister(&hidg_misc);
	misc_deregister(&printk_misc);
	misc_deregister(&gpio_misc);
	misc_deregister(&link_misc);
	for (i = 0; i < N_CHAN; i++) {
		kthread_stop(chans[i].thread);
		misc_deregister(&chans[i].misc);
	}
}

module_init(rx3_board_init);
module_exit(rx3_board_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("XDJ-RX3 Pioneer character devices for dj-emu");
