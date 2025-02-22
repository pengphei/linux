// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2010 Matt Turner.
 * Copyright 2012 Red Hat
 *
 * Authors: Matthew Garrett
 *          Matt Turner
 *          Dave Airlie
 */

#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_pci.h>

#include "mgag200_drv.h"

static const struct drm_mode_config_funcs mga_mode_funcs = {
	.fb_create = drm_gem_fb_create
};

static int mga_probe_vram(struct mga_device *mdev, void __iomem *mem)
{
	int offset;
	int orig;
	int test1, test2;
	int orig1, orig2;
	unsigned int vram_size;

	/* Probe */
	orig = ioread16(mem);
	iowrite16(0, mem);

	vram_size = mdev->mc.vram_window;

	if ((mdev->type == G200_EW3) && (vram_size >= 0x1000000)) {
		vram_size = vram_size - 0x400000;
	}

	for (offset = 0x100000; offset < vram_size; offset += 0x4000) {
		orig1 = ioread8(mem + offset);
		orig2 = ioread8(mem + offset + 0x100);

		iowrite16(0xaa55, mem + offset);
		iowrite16(0xaa55, mem + offset + 0x100);

		test1 = ioread16(mem + offset);
		test2 = ioread16(mem);

		iowrite16(orig1, mem + offset);
		iowrite16(orig2, mem + offset + 0x100);

		if (test1 != 0xaa55) {
			break;
		}

		if (test2) {
			break;
		}
	}

	iowrite16(orig, mem);
	return offset - 65536;
}

/* Map the framebuffer from the card and configure the core */
static int mga_vram_init(struct mga_device *mdev)
{
	void __iomem *mem;

	/* BAR 0 is VRAM */
	mdev->mc.vram_base = pci_resource_start(mdev->dev->pdev, 0);
	mdev->mc.vram_window = pci_resource_len(mdev->dev->pdev, 0);

	if (!devm_request_mem_region(mdev->dev->dev, mdev->mc.vram_base, mdev->mc.vram_window,
				"mgadrmfb_vram")) {
		DRM_ERROR("can't reserve VRAM\n");
		return -ENXIO;
	}

	mem = pci_iomap(mdev->dev->pdev, 0, 0);
	if (!mem)
		return -ENOMEM;

	mdev->mc.vram_size = mga_probe_vram(mdev, mem);

	pci_iounmap(mdev->dev->pdev, mem);

	return 0;
}

static int mgag200_device_init(struct drm_device *dev,
			       uint32_t flags)
{
	struct mga_device *mdev = dev->dev_private;
	int ret, option;

	mdev->type = flags;

	/* Hardcode the number of CRTCs to 1 */
	mdev->num_crtc = 1;

	pci_read_config_dword(dev->pdev, PCI_MGA_OPTION, &option);
	mdev->has_sdram = !(option & (1 << 14));

	/* BAR 0 is the framebuffer, BAR 1 contains registers */
	mdev->rmmio_base = pci_resource_start(mdev->dev->pdev, 1);
	mdev->rmmio_size = pci_resource_len(mdev->dev->pdev, 1);

	if (!devm_request_mem_region(mdev->dev->dev, mdev->rmmio_base, mdev->rmmio_size,
				"mgadrmfb_mmio")) {
		DRM_ERROR("can't reserve mmio registers\n");
		return -ENOMEM;
	}

	mdev->rmmio = pcim_iomap(dev->pdev, 1, 0);
	if (mdev->rmmio == NULL)
		return -ENOMEM;

	/* stash G200 SE model number for later use */
	if (IS_G200_SE(mdev))
		mdev->unique_rev_id = RREG32(0x1e24);

	ret = mga_vram_init(mdev);
	if (ret)
		return ret;

	mdev->bpp_shifts[0] = 0;
	mdev->bpp_shifts[1] = 1;
	mdev->bpp_shifts[2] = 0;
	mdev->bpp_shifts[3] = 2;
	return 0;
}

/*
 * Functions here will be called by the core once it's bound the driver to
 * a PCI device
 */


int mgag200_driver_load(struct drm_device *dev, unsigned long flags)
{
	struct mga_device *mdev;
	int r;

	mdev = devm_kzalloc(dev->dev, sizeof(struct mga_device), GFP_KERNEL);
	if (mdev == NULL)
		return -ENOMEM;
	dev->dev_private = (void *)mdev;
	mdev->dev = dev;

	r = mgag200_device_init(dev, flags);
	if (r) {
		dev_err(&dev->pdev->dev, "Fatal error during GPU init: %d\n", r);
		return r;
	}
	r = mgag200_mm_init(mdev);
	if (r)
		goto err_mm;

	drm_mode_config_init(dev);
	dev->mode_config.funcs = (void *)&mga_mode_funcs;
	if (IS_G200_SE(mdev) && mdev->vram_fb_available < (2048*1024))
		dev->mode_config.preferred_depth = 16;
	else
		dev->mode_config.preferred_depth = 32;
	dev->mode_config.prefer_shadow = 1;

	r = mgag200_modeset_init(mdev);
	if (r) {
		dev_err(&dev->pdev->dev, "Fatal error during modeset init: %d\n", r);
		goto err_modeset;
	}

	r = mgag200_cursor_init(mdev);
	if (r)
		dev_warn(&dev->pdev->dev,
			"Could not initialize cursors. Not doing hardware cursors.\n");

	r = drm_fbdev_generic_setup(mdev->dev, 0);
	if (r)
		goto err_modeset;

	return 0;

err_modeset:
	drm_mode_config_cleanup(dev);
	mgag200_cursor_fini(mdev);
	mgag200_mm_fini(mdev);
err_mm:
	dev->dev_private = NULL;

	return r;
}

void mgag200_driver_unload(struct drm_device *dev)
{
	struct mga_device *mdev = dev->dev_private;

	if (mdev == NULL)
		return;
	mgag200_modeset_fini(mdev);
	drm_mode_config_cleanup(dev);
	mgag200_cursor_fini(mdev);
	mgag200_mm_fini(mdev);
	dev->dev_private = NULL;
}
