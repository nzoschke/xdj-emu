// SPDX-License-Identifier: GPL-2.0
/*
 * rx3fb.c - the XDJ-RX3's LCD framebuffer, for dj-emu.
 *
 * The XDJ-RX3 drives its 1280x800 panel through the i.MX6 framebuffer
 * (mxcfb); its player renders through DirectFB's fbdev system into a 16-bit
 * RGB565 primary layer and pans between two buffers. virtio-gpu offers only
 * 32-bit formats, so the RX3 guest gets this fbdev instead, as /dev/fb0.
 *
 * Its memory is QEMU's ivshmem-plain BAR 2, a file the host maps too
 * (the same transport as the CDJ jog LCD). Layout of that file:
 *
 *   0x0000  struct rx3fb_header (below), little-endian, written by the guest
 *   0x0800  struct rx3fb_touch, written by the host: the touch panel
 *   0x1000  pixels: yres_virtual rows of `stride` bytes; the visible frame
 *           starts at row `yoffset`
 *
 * Touch: the RX3's resistive panel is read by a TSC2007 that Pioneer's
 * tsc2007_i2c driver exposes as /dev/tsc2007_2-0048: read() returns the
 * latest struct ts_data { bool touch; u16 x, y; } (6 bytes on ARM32), poll()
 * wakes when it changes, and ioctls 'k' 0/1 set the poll period and the
 * maximum resistance. This module provides that device and feeds it from
 * the host's rx3fb_touch: x and y in LCD pixels, with x reported mirrored
 * (1279 - x) as the panel is wired.
 *
 * The host reads the header, waits for `generation` to be even (it is odd
 * while the mode or the pan offset changes) and converts from `bpp` and the
 * bitfields.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/uaccess.h>

#define RX3FB_MAGIC 0x46335852	/* "RX3F" */
#define RX3FB_VERSION 1
#define RX3FB_PIXELS 0x1000
#define RX3FB_W 1280
#define RX3FB_H 800

struct rx3fb_header {
	__le32 magic;
	__le32 version;
	__le32 generation;
	__le32 width;
	__le32 height;
	__le32 bpp;
	__le32 stride;
	__le32 yoffset;
	__le32 pixels;		/* byte offset of row 0 */
	u8 red_off, red_len, green_off, green_len, blue_off, blue_len, _pad[2];
};

#define RX3FB_TOUCH 0x800

/* Written by the host. seq changes on every update; even values only. */
struct rx3fb_touch {
	__le32 seq;
	__le32 down;
	__le32 x;	/* LCD pixels, 0..1279 */
	__le32 y;	/* LCD pixels, 0..799 */
};

/* Pioneer's struct ts_data as the ARM32 player lays it out. */
struct ts_data32 {
	u8 touch;
	u8 _pad;
	__u16 x, y;
} __packed;

struct rx3fb {
	struct fb_info *info;
	void *mem;
	struct rx3fb_header *hdr;
	struct rx3fb_touch *touch;
	u32 pseudo_palette[16];
	struct task_struct *touch_thread;
	struct mutex ts_lock;
	wait_queue_head_t ts_wait;
	struct ts_data32 ts;
	bool ts_detected;
	u32 poll_period;
	u16 max_rt;
	struct miscdevice ts_misc;
};

static struct rx3fb *the_rx3fb;

static void rx3fb_publish(struct rx3fb *p)
{
	struct fb_var_screeninfo *v = &p->info->var;
	struct rx3fb_header *h = p->hdr;
	u32 gen = le32_to_cpu(READ_ONCE(h->generation));

	WRITE_ONCE(h->generation, cpu_to_le32(gen | 1));
	smp_wmb();
	h->width = cpu_to_le32(v->xres);
	h->height = cpu_to_le32(v->yres);
	h->bpp = cpu_to_le32(v->bits_per_pixel);
	h->stride = cpu_to_le32(p->info->fix.line_length);
	h->yoffset = cpu_to_le32(v->yoffset);
	h->pixels = cpu_to_le32(RX3FB_PIXELS);
	h->red_off = v->red.offset;
	h->red_len = v->red.length;
	h->green_off = v->green.offset;
	h->green_len = v->green.length;
	h->blue_off = v->blue.offset;
	h->blue_len = v->blue.length;
	smp_wmb();
	WRITE_ONCE(h->generation, cpu_to_le32((gen | 1) + 1));
}

static int rx3fb_check_var(struct fb_var_screeninfo *v, struct fb_info *info)
{
	u32 stride;

	if (v->xres != RX3FB_W || v->yres != RX3FB_H)
		return -EINVAL;
	v->xres_virtual = RX3FB_W;
	v->xoffset = 0;
	switch (v->bits_per_pixel) {
	case 16:
		v->red = (struct fb_bitfield){ 11, 5, 0 };
		v->green = (struct fb_bitfield){ 5, 6, 0 };
		v->blue = (struct fb_bitfield){ 0, 5, 0 };
		v->transp = (struct fb_bitfield){ 0, 0, 0 };
		break;
	case 32:
		v->red = (struct fb_bitfield){ 16, 8, 0 };
		v->green = (struct fb_bitfield){ 8, 8, 0 };
		v->blue = (struct fb_bitfield){ 0, 8, 0 };
		v->transp = (struct fb_bitfield){ 24, 8, 0 };
		break;
	default:
		return -EINVAL;
	}
	stride = RX3FB_W * v->bits_per_pixel / 8;
	if (v->yres_virtual < RX3FB_H)
		v->yres_virtual = RX3FB_H;
	if ((u64)stride * v->yres_virtual > info->fix.smem_len)
		return -ENOMEM;
	if (v->yoffset > v->yres_virtual - RX3FB_H)
		return -EINVAL;
	v->grayscale = 0;
	v->nonstd = 0;
	return 0;
}

static int rx3fb_set_par(struct fb_info *info)
{
	info->fix.line_length = info->var.xres_virtual * info->var.bits_per_pixel / 8;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	rx3fb_publish(info->par);
	return 0;
}

static int rx3fb_pan_display(struct fb_var_screeninfo *v, struct fb_info *info)
{
	if (v->xoffset || v->yoffset > info->var.yres_virtual - info->var.yres)
		return -EINVAL;
	info->var.yoffset = v->yoffset;
	rx3fb_publish(info->par);
	return 0;
}

static int rx3fb_setcolreg(unsigned regno, unsigned red, unsigned green, unsigned blue,
			   unsigned transp, struct fb_info *info)
{
	struct fb_var_screeninfo *v = &info->var;
	u32 c;

	if (regno >= 16)
		return 0;	/* truecolor: only the console palette matters */
	c = ((red >> (16 - v->red.length)) << v->red.offset) |
	    ((green >> (16 - v->green.length)) << v->green.offset) |
	    ((blue >> (16 - v->blue.length)) << v->blue.offset);
	((u32 *)info->pseudo_palette)[regno] = c;
	return 0;
}

static int rx3fb_blank(int blank, struct fb_info *info)
{
	return 0;
}

static int rx3fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	if (cmd != FBIO_WAITFORVSYNC)
		return -ENOTTY;
	/* DirectFB calls this from a high-priority display thread. A missing
	 * vsync makes that thread spin and starve other firmware work.
	 */
	usleep_range(16000, 17000);
	return 0;
}

static const struct fb_ops rx3fb_ops = {
	.owner = THIS_MODULE,
	/* Linux 6.6 has no FB_DEFAULT_SYSMEM_OPS yet. */
	.fb_read = fb_sys_read,
	.fb_write = fb_sys_write,
	.fb_fillrect = sys_fillrect,
	.fb_copyarea = sys_copyarea,
	.fb_imageblit = sys_imageblit,
	.fb_check_var = rx3fb_check_var,
	.fb_set_par = rx3fb_set_par,
	.fb_pan_display = rx3fb_pan_display,
	.fb_setcolreg = rx3fb_setcolreg,
	.fb_blank = rx3fb_blank,
	.fb_ioctl = rx3fb_ioctl,
};

/* ---- /dev/tsc2007_2-0048 ------------------------------------------------ */

#define TSC2007_IOC_MAGIC 'k'

static int rx3fb_touch_thread(void *data)
{
	struct rx3fb *p = data;
	u32 last = 0;

	while (!kthread_should_stop()) {
		u32 seq = le32_to_cpu(READ_ONCE(p->touch->seq));

		/* A held contact continues to produce ADC samples on the board.
		 * rbp's TouchAdValueHysteresis discards the first contact sample;
		 * forwarding only host position changes loses stationary clicks.
		 */
		if ((seq != last || READ_ONCE(p->ts.touch)) && !(seq & 1)) {
			struct ts_data32 t = { 0 };
			u32 x, y;

			smp_rmb();
			x = min_t(u32, le32_to_cpu(READ_ONCE(p->touch->x)), RX3FB_W - 1);
			y = min_t(u32, le32_to_cpu(READ_ONCE(p->touch->y)), RX3FB_H - 1);
			t.touch = !!le32_to_cpu(READ_ONCE(p->touch->down));
			t.x = RX3FB_W - 1 - x;
			t.y = y;
			last = seq;
			mutex_lock(&p->ts_lock);
			if (!t.touch) {
				/* Like the driver: lifting keeps the last position. */
				t.x = p->ts.x;
				t.y = p->ts.y;
			}
			if (t.touch || memcmp(&t, &p->ts, sizeof(t))) {
				p->ts = t;
				p->ts_detected = true;
			}
			mutex_unlock(&p->ts_lock);
			wake_up_interruptible(&p->ts_wait);
		}
		usleep_range(4000, 6000);
	}
	return 0;
}

static int ts_open(struct inode *inode, struct file *f)
{
	pr_notice("rx3fb: %s opened the touch panel\n", current->comm);
	return 0;
}

static ssize_t ts_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	struct rx3fb *p = the_rx3fb;
	ssize_t ret;

	if (n > sizeof(p->ts))
		n = sizeof(p->ts);
	mutex_lock(&p->ts_lock);
	ret = copy_to_user(buf, &p->ts, n) ? -EFAULT : n;
	p->ts_detected = false;
	mutex_unlock(&p->ts_lock);
	return ret;
}

static ssize_t ts_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
	return -EFAULT;	/* as the driver: not supported */
}

static __poll_t ts_poll(struct file *f, poll_table *wait)
{
	struct rx3fb *p = the_rx3fb;

	poll_wait(f, &p->ts_wait, wait);
	return READ_ONCE(p->ts_detected) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static long ts_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct rx3fb *p = the_rx3fb;
	void __user *up = (void __user *)arg;
	u32 v32;
	u16 v16;

	if (_IOC_TYPE(cmd) != TSC2007_IOC_MAGIC)
		return -ENOTTY;
	switch (cmd) {
	case _IOR(TSC2007_IOC_MAGIC, 0, __u32):
		return put_user(p->poll_period, (u32 __user *)up);
	case _IOW(TSC2007_IOC_MAGIC, 0, __u32):
		if (get_user(v32, (u32 __user *)up))
			return -EFAULT;
		p->poll_period = v32;
		return 0;
	case _IOR(TSC2007_IOC_MAGIC, 1, __u16):
		return put_user(p->max_rt, (u16 __user *)up);
	case _IOW(TSC2007_IOC_MAGIC, 1, __u16):
		if (get_user(v16, (u16 __user *)up))
			return -EFAULT;
		p->max_rt = v16;
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations ts_fops = {
	.owner = THIS_MODULE,
	.open = ts_open,
	.read = ts_read,
	.write = ts_write,
	.poll = ts_poll,
	.unlocked_ioctl = ts_ioctl,
	.llseek = no_llseek,
};

static int rx3fb_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	resource_size_t start, len;
	struct fb_info *info;
	struct rx3fb *p;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	start = pci_resource_start(pdev, 2);
	len = pci_resource_len(pdev, 2);
	if (len < RX3FB_PIXELS + (resource_size_t)RX3FB_W * RX3FB_H * 4)
		return -ENOSPC;
	ret = pci_request_region(pdev, 2, "rx3fb");
	if (ret)
		return ret;

	info = framebuffer_alloc(sizeof(*p), &pdev->dev);
	if (!info) {
		pci_release_region(pdev, 2);
		return -ENOMEM;
	}
	p = info->par;
	p->info = info;
	p->mem = devm_memremap(&pdev->dev, start, len, MEMREMAP_WB);
	if (IS_ERR(p->mem)) {
		ret = PTR_ERR(p->mem);
		goto err;
	}
	p->hdr = p->mem;
	memset(p->mem, 0, len);
	p->hdr->magic = cpu_to_le32(RX3FB_MAGIC);
	p->hdr->version = cpu_to_le32(RX3FB_VERSION);

	strscpy(info->fix.id, "mxc_rx3fb", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.ypanstep = 1;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.smem_start = start + RX3FB_PIXELS;
	info->fix.smem_len = len - RX3FB_PIXELS;
	info->screen_buffer = p->mem + RX3FB_PIXELS;
	info->screen_size = info->fix.smem_len;
	info->fbops = &rx3fb_ops;
	info->flags = FBINFO_VIRTFB;
	info->pseudo_palette = p->pseudo_palette;

	/* The i.MX6 framebuffer comes up in RGB565, like the RX3's. */
	info->var.xres = RX3FB_W;
	info->var.yres = RX3FB_H;
	info->var.xres_virtual = RX3FB_W;
	info->var.yres_virtual = RX3FB_H * 2;
	info->var.bits_per_pixel = 16;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.width = 217;	/* mm, a 10.1" 16:10 panel */
	info->var.height = 136;
	info->var.pixclock = 14000;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	ret = rx3fb_check_var(&info->var, info);
	if (ret)
		goto err;
	rx3fb_set_par(info);

	ret = register_framebuffer(info);
	if (ret)
		goto err;
	pci_set_drvdata(pdev, info);

	p->touch = p->mem + RX3FB_TOUCH;
	mutex_init(&p->ts_lock);
	init_waitqueue_head(&p->ts_wait);
	p->poll_period = 10;
	p->max_rt = 0xffff;
	the_rx3fb = p;
	p->ts_misc = (struct miscdevice){
		.minor = MISC_DYNAMIC_MINOR, .name = "tsc2007_2-0048", .fops = &ts_fops, .mode = 0666,
	};
	ret = misc_register(&p->ts_misc);
	if (ret) {
		unregister_framebuffer(info);
		goto err;
	}
	p->touch_thread = kthread_run(rx3fb_touch_thread, p, "rx3fb_touch");
	if (IS_ERR(p->touch_thread)) {
		ret = PTR_ERR(p->touch_thread);
		misc_deregister(&p->ts_misc);
		unregister_framebuffer(info);
		goto err;
	}
	fb_info(info, "XDJ-RX3 LCD %ux%u, %llu KiB shared with the host\n",
		RX3FB_W, RX3FB_H, (unsigned long long)len / 1024);
	return 0;
err:
	framebuffer_release(info);
	pci_release_region(pdev, 2);
	return ret;
}

static void rx3fb_remove(struct pci_dev *pdev)
{
	struct fb_info *info = pci_get_drvdata(pdev);
	struct rx3fb *p = info->par;

	kthread_stop(p->touch_thread);
	misc_deregister(&p->ts_misc);
	the_rx3fb = NULL;
	unregister_framebuffer(info);
	framebuffer_release(info);
	pci_release_region(pdev, 2);
}

/* QEMU ivshmem (ivshmem-plain): Red Hat, Inc. 1af4:1110. */
static const struct pci_device_id rx3fb_ids[] = {
	{ PCI_DEVICE(0x1af4, 0x1110) },
	{ }
};
MODULE_DEVICE_TABLE(pci, rx3fb_ids);

static struct pci_driver rx3fb_driver = {
	.name = "rx3fb",
	.id_table = rx3fb_ids,
	.probe = rx3fb_probe,
	.remove = rx3fb_remove,
};
module_pci_driver(rx3fb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("XDJ-RX3 LCD framebuffer on ivshmem for dj-emu");
