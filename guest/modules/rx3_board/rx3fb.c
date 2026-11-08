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
 *   0x0000  struct rx3fb_header (below), little-endian
 *   0x1000  pixels: yres_virtual rows of `stride` bytes; the visible frame
 *           starts at row `yoffset`
 *
 * The host reads the header, waits for `generation` to be even (it is odd
 * while the mode or the pan offset changes) and converts from `bpp` and the
 * bitfields.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fb.h>
#include <linux/io.h>

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

struct rx3fb {
	struct fb_info *info;
	void *mem;
	struct rx3fb_header *hdr;
	u32 pseudo_palette[16];
};

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
