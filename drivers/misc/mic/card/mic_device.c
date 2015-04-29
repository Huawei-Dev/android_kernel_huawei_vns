/*
 * Intel MIC Platform Software Stack (MPSS)
 *
 * Copyright(c) 2013 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Disclaimer: The codes contained in these modules may be specific to
 * the Intel Software Development Platform codenamed: Knights Ferry, and
 * the Intel product codenamed: Knights Corner, and are not backward
 * compatible with other Intel products. Additionally, Intel will NOT
 * support the codes or instruction set in future products.
 *
 * Intel MIC Card driver.
 *
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/reboot.h>
#include <linux/dmaengine.h>
#include <linux/kmod.h>

#include <linux/mic_common.h>
#include "../common/mic_dev.h"
#include "mic_device.h"
#include "mic_virtio.h"

static struct mic_driver *g_drv;
static struct mic_irq *shutdown_cookie;

static void mic_notify_host(u8 state)
{
	struct mic_driver *mdrv = g_drv;
	struct mic_bootparam __iomem *bootparam = mdrv->dp;

	iowrite8(state, &bootparam->shutdown_status);
	dev_dbg(mdrv->dev, "%s %d system_state %d\n",
		__func__, __LINE__, state);
	mic_send_intr(&mdrv->mdev, ioread8(&bootparam->c2h_shutdown_db));
}

static int mic_panic_event(struct notifier_block *this, unsigned long event,
		void *ptr)
{
	struct mic_driver *mdrv = g_drv;
	struct mic_bootparam __iomem *bootparam = mdrv->dp;

	iowrite8(-1, &bootparam->h2c_config_db);
	iowrite8(-1, &bootparam->h2c_shutdown_db);
	mic_notify_host(MIC_CRASHED);
	return NOTIFY_DONE;
}

static struct notifier_block mic_panic = {
	.notifier_call  = mic_panic_event,
};

static irqreturn_t mic_shutdown_isr(int irq, void *data)
{
	struct mic_driver *mdrv = g_drv;
	struct mic_bootparam __iomem *bootparam = mdrv->dp;

	mic_ack_interrupt(&g_drv->mdev);
	if (ioread8(&bootparam->shutdown_card))
		orderly_poweroff(true);
	return IRQ_HANDLED;
}

static int mic_shutdown_init(void)
{
	int rc = 0;
	struct mic_driver *mdrv = g_drv;
	struct mic_bootparam __iomem *bootparam = mdrv->dp;
	int shutdown_db;

	shutdown_db = mic_next_card_db();
	shutdown_cookie = mic_request_card_irq(mic_shutdown_isr, NULL,
					       "Shutdown", mdrv, shutdown_db);
	if (IS_ERR(shutdown_cookie))
		rc = PTR_ERR(shutdown_cookie);
	else
		iowrite8(shutdown_db, &bootparam->h2c_shutdown_db);
	return rc;
}

static void mic_shutdown_uninit(void)
{
	struct mic_driver *mdrv = g_drv;
	struct mic_bootparam __iomem *bootparam = mdrv->dp;

	iowrite8(-1, &bootparam->h2c_shutdown_db);
	mic_free_card_irq(shutdown_cookie, mdrv);
}

static int __init mic_dp_init(void)
{
	struct mic_driver *mdrv = g_drv;
	struct mic_device *mdev = &mdrv->mdev;
	struct mic_bootparam __iomem *bootparam;
	u64 lo, hi, dp_dma_addr;
	u32 magic;

	lo = mic_read_spad(&mdrv->mdev, MIC_DPLO_SPAD);
	hi = mic_read_spad(&mdrv->mdev, MIC_DPHI_SPAD);

	dp_dma_addr = lo | (hi << 32);
	mdrv->dp = mic_card_map(mdev, dp_dma_addr, MIC_DP_SIZE);
	if (!mdrv->dp) {
		dev_err(mdrv->dev, "Cannot remap Aperture BAR\n");
		return -ENOMEM;
	}
	bootparam = mdrv->dp;
	magic = ioread32(&bootparam->magic);
	if (MIC_MAGIC != magic) {
		dev_err(mdrv->dev, "bootparam magic mismatch 0x%x\n", magic);
		return -EIO;
	}
	return 0;
}

/* Uninitialize the device page */
static void mic_dp_uninit(void)
{
	mic_card_unmap(&g_drv->mdev, g_drv->dp);
}

/**
 * mic_request_card_irq - request an irq.
 *
 * @handler: interrupt handler passed to request_threaded_irq.
 * @thread_fn: thread fn. passed to request_threaded_irq.
 * @name: The ASCII name of the callee requesting the irq.
 * @data: private data that is returned back when calling the
 * function handler.
 * @index: The doorbell index of the requester.
 *
 * returns: The cookie that is transparent to the caller. Passed
 * back when calling mic_free_irq. An appropriate error code
 * is returned on failure. Caller needs to use IS_ERR(return_val)
 * to check for failure and PTR_ERR(return_val) to obtained the
 * error code.
 *
 */
struct mic_irq *
mic_request_card_irq(irq_handler_t handler,
		     irq_handler_t thread_fn, const char *name,
		     void *data, int index)
{
	int rc = 0;
	unsigned long cookie;
	struct mic_driver *mdrv = g_drv;

	rc  = request_threaded_irq(mic_db_to_irq(mdrv, index), handler,
				   thread_fn, 0, name, data);
	if (rc) {
		dev_err(mdrv->dev, "request_threaded_irq failed rc = %d\n", rc);
		goto err;
	}
	mdrv->irq_info.irq_usage_count[index]++;
	cookie = index;
	return (struct mic_irq *)cookie;
err:
	return ERR_PTR(rc);
}

/**
 * mic_free_card_irq - free irq.
 *
 * @cookie: cookie obtained during a successful call to mic_request_threaded_irq
 * @data: private data specified by the calling function during the
 * mic_request_threaded_irq
 *
 * returns: none.
 */
void mic_free_card_irq(struct mic_irq *cookie, void *data)
{
	int index;
	struct mic_driver *mdrv = g_drv;

	index = (unsigned long)cookie & 0xFFFFU;
	free_irq(mic_db_to_irq(mdrv, index), data);
	mdrv->irq_info.irq_usage_count[index]--;
}

/**
 * mic_next_card_db - Get the doorbell with minimum usage count.
 *
 * Returns the irq index.
 */
int mic_next_card_db(void)
{
	int i;
	int index = 0;
	struct mic_driver *mdrv = g_drv;

	for (i = 0; i < mdrv->intr_info.num_intr; i++) {
		if (mdrv->irq_info.irq_usage_count[i] <
			mdrv->irq_info.irq_usage_count[index])
			index = i;
	}

	return index;
}

/**
 * mic_init_irq - Initialize irq information.
 *
 * Returns 0 in success. Appropriate error code on failure.
 */
static int mic_init_irq(void)
{
	struct mic_driver *mdrv = g_drv;

	mdrv->irq_info.irq_usage_count = kzalloc((sizeof(u32) *
			mdrv->intr_info.num_intr),
			GFP_KERNEL);
	if (!mdrv->irq_info.irq_usage_count)
		return -ENOMEM;
	return 0;
}

/**
 * mic_uninit_irq - Uninitialize irq information.
 *
 * None.
 */
static void mic_uninit_irq(void)
{
	struct mic_driver *mdrv = g_drv;

	kfree(mdrv->irq_info.irq_usage_count);
}

static inline struct mic_driver *scdev_to_mdrv(struct scif_hw_dev *scdev)
{
	return dev_get_drvdata(scdev->dev.parent);
}

static struct mic_irq *
___mic_request_irq(struct scif_hw_dev *scdev,
		   irqreturn_t (*func)(int irq, void *data),
				       const char *name, void *data,
				       int db)
{
	return mic_request_card_irq(func, NULL, name, data, db);
}

static void
___mic_free_irq(struct scif_hw_dev *scdev,
		struct mic_irq *cookie, void *data)
{
	return mic_free_card_irq(cookie, data);
}

static void ___mic_ack_interrupt(struct scif_hw_dev *scdev, int num)
{
	struct mic_driver *mdrv = scdev_to_mdrv(scdev);

	mic_ack_interrupt(&mdrv->mdev);
}

static int ___mic_next_db(struct scif_hw_dev *scdev)
{
	return mic_next_card_db();
}

static void ___mic_send_intr(struct scif_hw_dev *scdev, int db)
{
	struct mic_driver *mdrv = scdev_to_mdrv(scdev);

	mic_send_intr(&mdrv->mdev, db);
}

static void ___mic_send_p2p_intr(struct scif_hw_dev *scdev, int db,
				 struct mic_mw *mw)
{
	mic_send_p2p_intr(db, mw);
}

static void __iomem *
___mic_ioremap(struct scif_hw_dev *scdev,
	       phys_addr_t pa, size_t len)
{
	struct mic_driver *mdrv = scdev_to_mdrv(scdev);

	return mic_card_map(&mdrv->mdev, pa, len);
}

static void ___mic_iounmap(struct scif_hw_dev *scdev, void __iomem *va)
{
	struct mic_driver *mdrv = scdev_to_mdrv(scdev);

	mic_card_unmap(&mdrv->mdev, va);
}

static struct scif_hw_ops scif_hw_ops = {
	.request_irq = ___mic_request_irq,
	.free_irq = ___mic_free_irq,
	.ack_interrupt = ___mic_ack_interrupt,
	.next_db = ___mic_next_db,
	.send_intr = ___mic_send_intr,
	.send_p2p_intr = ___mic_send_p2p_intr,
	.ioremap = ___mic_ioremap,
	.iounmap = ___mic_iounmap,
};

static int mic_request_dma_chans(struct mic_driver *mdrv)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;

	request_module("mic_x100_dma");
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	do {
		chan = dma_request_channel(mask, NULL, NULL);
		if (chan) {
			mdrv->dma_ch[mdrv->num_dma_ch++] = chan;
			if (mdrv->num_dma_ch >= MIC_MAX_DMA_CHAN)
				break;
		}
	} while (chan);
	dev_info(mdrv->dev, "DMA channels # %d\n", mdrv->num_dma_ch);
	return mdrv->num_dma_ch;
}

static void mic_free_dma_chans(struct mic_driver *mdrv)
{
	int i = 0;

	for (i = 0; i < mdrv->num_dma_ch; i++) {
		dma_release_channel(mdrv->dma_ch[i]);
		mdrv->dma_ch[i] = NULL;
	}
	mdrv->num_dma_ch = 0;
}

/*
 * mic_driver_init - MIC driver initialization tasks.
 *
 * Returns 0 in success. Appropriate error code on failure.
 */
int __init mic_driver_init(struct mic_driver *mdrv)
{
	int rc;
	struct mic_bootparam __iomem *bootparam;
	u8 node_id;

	g_drv = mdrv;
	/*
	 * Unloading the card module is not supported. The MIC card module
	 * handles fundamental operations like host/card initiated shutdowns
	 * and informing the host about card crashes and cannot be unloaded.
	 */
	if (!try_module_get(mdrv->dev->driver->owner)) {
		rc = -ENODEV;
		goto done;
	}
	rc = mic_dp_init();
	if (rc)
		goto put;
	rc = mic_init_irq();
	if (rc)
		goto dp_uninit;
	rc = mic_shutdown_init();
	if (rc)
		goto irq_uninit;
	if (!mic_request_dma_chans(mdrv)) {
		rc = -ENODEV;
		goto shutdown_uninit;
	}
	rc = mic_devices_init(mdrv);
	if (rc)
		goto dma_free;
	bootparam = mdrv->dp;
	node_id = ioread8(&bootparam->node_id);
	mdrv->scdev = scif_register_device(mdrv->dev, MIC_SCIF_DEV,
					   NULL, &scif_hw_ops,
					   0, node_id, &mdrv->mdev.mmio, NULL,
					   NULL, mdrv->dp, mdrv->dma_ch,
					   mdrv->num_dma_ch);
	if (IS_ERR(mdrv->scdev)) {
		rc = PTR_ERR(mdrv->scdev);
		goto device_uninit;
	}
	mic_create_card_debug_dir(mdrv);
	atomic_notifier_chain_register(&panic_notifier_list, &mic_panic);
done:
	return rc;
device_uninit:
	mic_devices_uninit(mdrv);
dma_free:
	mic_free_dma_chans(mdrv);
shutdown_uninit:
	mic_shutdown_uninit();
irq_uninit:
	mic_uninit_irq();
dp_uninit:
	mic_dp_uninit();
put:
	module_put(mdrv->dev->driver->owner);
	return rc;
}

/*
 * mic_driver_uninit - MIC driver uninitialization tasks.
 *
 * Returns None
 */
void mic_driver_uninit(struct mic_driver *mdrv)
{
	mic_delete_card_debug_dir(mdrv);
	scif_unregister_device(mdrv->scdev);
	mic_devices_uninit(mdrv);
	mic_free_dma_chans(mdrv);
	/*
	 * Inform the host about the shutdown status i.e. poweroff/restart etc.
	 * The module cannot be unloaded so the only code path to call
	 * mic_devices_uninit(..) is the shutdown callback.
	 */
	mic_notify_host(system_state);
	mic_shutdown_uninit();
	mic_uninit_irq();
	mic_dp_uninit();
	module_put(mdrv->dev->driver->owner);
}
