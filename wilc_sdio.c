// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Atmel Corporation.  All rights reserved.
 *
 * Module Name:  wilc_sdio.c
 */

#include <linux/string.h>
#include "wilc_wlan_if.h"
#include "wilc_wlan.h"
#include "wilc_wfi_netdevice.h"
#include "wilc_gpio.h"
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/host.h>
#include <linux/of_gpio.h>


struct wilc_gpio wilc_gpio;

void chip_wakeup(struct wilc *wilc, int source);
void chip_allow_sleep(struct wilc *wilc, int source);

enum sdio_host_lock {
	WILC_SDIO_HOST_NO_TAKEN = 0,
	WILC_SDIO_HOST_IRQ_TAKEN = 1,
	WILC_SDIO_HOST_DIS_TAKEN = 2,
};

static enum sdio_host_lock	sdio_intr_lock = WILC_SDIO_HOST_NO_TAKEN;
static wait_queue_head_t sdio_intr_waitqueue;

#define SDIO_MODALIAS "wilc_sdio"

#define SDIO_VENDOR_ID_WILC 0x0296
#define SDIO_DEVICE_ID_WILC 0x5347

static const struct sdio_device_id wilc_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_WILC, SDIO_DEVICE_ID_WILC) },
	{ },
};

#define WILC_SDIO_BLOCK_SIZE 512

struct wilc_sdio {
	bool irq_gpio;
	u32 block_size;
	int nint;
	bool is_init;
};

static struct wilc_sdio g_sdio;
static const struct wilc_hif_func wilc_hif_sdio;

static int sdio_write_reg(struct wilc *wilc, u32 addr, u32 data);
static int sdio_read_reg(struct wilc *wilc, u32 addr, u32 *data);
static int sdio_init(struct wilc *wilc, bool resume);

static void wilc_sdio_interrupt(struct sdio_func *func)
{
	if (sdio_intr_lock == WILC_SDIO_HOST_DIS_TAKEN)
		return;
	sdio_intr_lock = WILC_SDIO_HOST_IRQ_TAKEN;
	sdio_release_host(func);
	wilc_handle_isr(sdio_get_drvdata(func));
	sdio_claim_host(func);
	sdio_intr_lock = WILC_SDIO_HOST_NO_TAKEN;
	wake_up_interruptible(&sdio_intr_waitqueue);
}

static int wilc_sdio_cmd52(struct wilc *wilc, struct sdio_cmd52 *cmd)
{
	struct sdio_func *func = container_of(wilc->dev, struct sdio_func, dev);
	int ret;
	u8 data;

	sdio_claim_host(func);

	func->num = cmd->function;
	if (cmd->read_write) {  /* write */
		if (cmd->raw) {
			sdio_writeb(func, cmd->data, cmd->address, &ret);
			data = sdio_readb(func, cmd->address, &ret);
			cmd->data = data;
		} else {
			sdio_writeb(func, cmd->data, cmd->address, &ret);
		}
	} else {        /* read */
		data = sdio_readb(func, cmd->address, &ret);
		cmd->data = data;
	}

	sdio_release_host(func);

	if (ret)
		dev_err(&func->dev, "%s..failed, err(%d)\n", __func__, ret);
	return ret;
}

static int wilc_sdio_cmd53(struct wilc *wilc, struct sdio_cmd53 *cmd)
{
	struct sdio_func *func = container_of(wilc->dev, struct sdio_func, dev);
	int size, ret;

	sdio_claim_host(func);

	func->num = cmd->function;
	func->cur_blksize = cmd->block_size;
	if (cmd->block_mode)
		size = cmd->count * cmd->block_size;
	else
		size = cmd->count;

	if (cmd->read_write) {  /* write */
		ret = sdio_memcpy_toio(func, cmd->address,
				       (void *)cmd->buffer, size);
	} else {        /* read */
		ret = sdio_memcpy_fromio(func, (void *)cmd->buffer,
					 cmd->address,  size);
	}

	sdio_release_host(func);

	if (ret)
		dev_err(&func->dev, "%s..failed, err(%d)\n", __func__,  ret);

	return ret;
}

static int linux_sdio_probe(struct sdio_func *func,
			    const struct sdio_device_id *id)
{
	struct wilc *wilc;
	int ret, io_type;
	struct device_node *cnp;
	
	cnp = func->card->host->parent->of_node;

	if (IS_ENABLED(CONFIG_WILC_HW_OOB_INTR))
		io_type = HIF_SDIO_GPIO_IRQ;
	else
		io_type = HIF_SDIO;
	dev_dbg(&func->dev, "Initializing netdev\n");
	ret = wilc_netdev_init(&wilc, &func->dev, io_type, &wilc_hif_sdio);
	if (ret) {
		dev_err(&func->dev, "Couldn't initialize netdev\n");
		return ret;
	}
	sdio_set_drvdata(func, wilc);
	wilc->dev = &func->dev;

	mutex_init(&wilc->hif_cs);
	mutex_init(&wilc->cs);

	wilc_bt_init(wilc);

	dev_info(&func->dev, "Driver Initializing success\n");
	return 0;
}

static void linux_sdio_remove(struct sdio_func *func)
{
	wilc_netdev_cleanup(sdio_get_drvdata(func));
	wilc_bt_deinit();
}

static int wilc_sdio_reset(struct wilc *wilc)
{
	struct sdio_cmd52 cmd;
	int ret;
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);

	dev_info(&func->dev, "De Init SDIO\n");

	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x6;
	cmd.data = 0x8;
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret)
		dev_err(&func->dev, "Fail cmd 52, reset cmd\n");
	return ret;
}

static bool sdio_is_init(void)
{
	return g_sdio.is_init;
}

static int wilc_sdio_suspend(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct wilc *wilc = sdio_get_drvdata(func);
	int ret;

	dev_info(&func->dev, "sdio suspend\n");
	mutex_lock(&wilc->hif_cs);

	chip_wakeup(wilc, 0);

	if (mutex_is_locked(&wilc->hif_cs)){
		mutex_unlock(&wilc->hif_cs);
	}

	host_sleep_notify(wilc, 0);
	chip_allow_sleep(wilc, 0);

	mutex_lock(&wilc->hif_cs);

	ret = wilc_sdio_reset(wilc);

	return 0;
}

static int wilc_sdio_resume(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct wilc *wilc = sdio_get_drvdata(func);

	dev_info(&func->dev, "sdio resume\n");
	chip_wakeup(wilc, 0);
	sdio_init(wilc, true);

	if (mutex_is_locked(&wilc->hif_cs))
		mutex_unlock(&wilc->hif_cs);

	host_wakeup_notify(wilc, 0);

	mutex_lock(&wilc->hif_cs);

	chip_allow_sleep(wilc, 0);

	if (mutex_is_locked(&wilc->hif_cs))
		mutex_unlock(&wilc->hif_cs);

	return 0;
}

static const struct of_device_id wilc_of_match[] = {
	{ .compatible = "atmel,wilc_sdio", },
	{}
};
MODULE_DEVICE_TABLE(of, wilc_of_match);

static const struct dev_pm_ops wilc_sdio_pm_ops = {
	.suspend = wilc_sdio_suspend,
	.resume = wilc_sdio_resume,
};

static struct sdio_driver wilc_sdio_driver = {
	.name		= SDIO_MODALIAS,
	.id_table	= wilc_sdio_ids,
	.probe		= linux_sdio_probe,
	.remove		= linux_sdio_remove,
	.drv = {
		.pm = &wilc_sdio_pm_ops,
		.of_match_table = wilc_of_match,
	}
};

static int __init wilc_sdio_driver_init(void) 
{ 
	struct device_node *cnp;
	int ret;
	int gpio_reset = -1;
	int gpio_chip_en = -1;
	int gpio_irq = -1;

	cnp = of_find_node_by_name(NULL, SDIO_GPIO_NODE);
	if (cnp == NULL){
		printk(KERN_WARNING "Device tree \"%s\" not found, using default pin defs\n", SDIO_GPIO_NODE);
		wilc_gpio.gpio_chip_en = GPIO_NUM_CHIP_EN;
		wilc_gpio.gpio_irq = GPIO_NUM_IRQ;
		wilc_gpio.gpio_reset = GPIO_NUM_RESET;
	} else {
		gpio_reset = of_get_named_gpio_flags(cnp, "gpio_reset", 0, NULL);
		if (gpio_reset < 0) {
			ret = gpio_reset;
			gpio_reset = GPIO_NUM_RESET;
			printk(KERN_WARNING "WILC setting default Reset GPIO to %d. Got %d\n",
				 gpio_reset, ret);
		} else {
			printk(KERN_INFO "WILC got %d for gpio_reset\n",
				 gpio_reset);
		}

		gpio_chip_en = of_get_named_gpio_flags(cnp, "gpio_chip_en", 0, NULL);
		if (gpio_chip_en < 0) {
			ret = gpio_chip_en;
			gpio_chip_en = GPIO_NUM_CHIP_EN;
			printk(KERN_WARNING "WILC setting default Chip Enable GPIO to %d. Got %d\n",
				 gpio_chip_en, ret);
		} else {
			printk(KERN_INFO "WILC got %d for gpio_chip_en\n",
				 gpio_chip_en);
		}

		gpio_irq = of_get_named_gpio_flags(cnp, "gpio_irq", 0, NULL);
		if (gpio_irq < 0) {
			ret = gpio_irq;
			gpio_irq = GPIO_NUM_IRQ;
			printk(KERN_WARNING "WILC setting default IRQ GPIO to %d. Got %d\n",
				 gpio_irq, ret);
		} else {
			printk(KERN_INFO "WILC got %d for gpio_irq\n", gpio_irq);
		}

		wilc_gpio.gpio_chip_en = gpio_chip_en;
		wilc_gpio.gpio_irq = gpio_irq;
		wilc_gpio.gpio_reset = gpio_reset;

		gpio_request_one(wilc_gpio.gpio_chip_en, GPIOF_INIT_LOW, "gpio_chip_en");
		//gpio_request_one(wilc_gpio.gpio_irq, GPIOF_INIT_LOW, "gpio_irq");
		gpio_request_one(wilc_gpio.gpio_reset, GPIOF_INIT_LOW, "gpio_reset");
	}

	printk(KERN_INFO "Enabling device\n");
	wilc_wlan_power_on_sequence();
	return sdio_register_driver(&wilc_sdio_driver); 
} 
module_init(wilc_sdio_driver_init); 

static void __exit wilc_sdio_driver_exit(void) 
{ 
	gpio_free(wilc_gpio.gpio_chip_en);
	gpio_free(wilc_gpio.gpio_irq);
	gpio_free(wilc_gpio.gpio_reset);
	sdio_unregister_driver(&wilc_sdio_driver); 
} 
module_exit(wilc_sdio_driver_exit);


MODULE_LICENSE("GPL");

static int wilc_sdio_enable_interrupt(struct wilc *dev)
{
	struct sdio_func *func = container_of(dev->dev, struct sdio_func, dev);
	int ret = 0;
	sdio_intr_lock  = WILC_SDIO_HOST_NO_TAKEN;

	sdio_claim_host(func);
	ret = sdio_claim_irq(func, wilc_sdio_interrupt);
	sdio_release_host(func);

	if (ret < 0) {
		dev_err(&func->dev, "can't claim sdio_irq, err(%d)\n", ret);
		ret = -EIO;
	}
	return ret;
}

static void wilc_sdio_disable_interrupt(struct wilc *dev)
{
	struct sdio_func *func = container_of(dev->dev, struct sdio_func, dev);
	int ret;

	dev_info(&func->dev, "wilc_sdio_disable_interrupt\n");

	if (sdio_intr_lock  == WILC_SDIO_HOST_IRQ_TAKEN)
		wait_event_interruptible(sdio_intr_waitqueue,
				   sdio_intr_lock == WILC_SDIO_HOST_NO_TAKEN);
	sdio_intr_lock  = WILC_SDIO_HOST_DIS_TAKEN;

	sdio_claim_host(func);
	ret = sdio_release_irq(func);
	if (ret < 0)
		dev_err(&func->dev, "can't release sdio_irq, err(%d)\n", ret);
	sdio_release_host(func);
	sdio_intr_lock  = WILC_SDIO_HOST_NO_TAKEN;
}

/********************************************
 *
 *      Function 0
 *
 ********************************************/

static int sdio_set_func0_csa_address(struct wilc *wilc, u32 adr)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	struct sdio_cmd52 cmd;
	int ret;

	/**
	 *      Review: BIG ENDIAN
	 **/
	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x10c;
	cmd.data = (u8)adr;
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Failed cmd52, set 0x10c data...\n");
		goto fail;
	}

	cmd.address = 0x10d;
	cmd.data = (u8)(adr >> 8);
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Failed cmd52, set 0x10d data...\n");
		goto fail;
	}

	cmd.address = 0x10e;
	cmd.data = (u8)(adr >> 16);
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Failed cmd52, set 0x10e data...\n");
		goto fail;
	}

	return 1;
fail:
	return 0;
}

static int sdio_set_func0_block_size(struct wilc *wilc, u32 block_size)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	struct sdio_cmd52 cmd;
	int ret;

	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x10;
	cmd.data = (u8)block_size;
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Failed cmd52, set 0x10 data...\n");
		goto fail;
	}

	cmd.address = 0x11;
	cmd.data = (u8)(block_size >> 8);
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Failed cmd52, set 0x11 data...\n");
		goto fail;
	}

	return 1;
fail:
	return 0;
}

/********************************************
 *
 *      Function 1
 *
 ********************************************/

static int sdio_set_func1_block_size(struct wilc *wilc, u32 block_size)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	struct sdio_cmd52 cmd;
	int ret;

	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x110;
	cmd.data = (u8)block_size;
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Failed cmd52, set 0x110 data...\n");
		goto fail;
	}
	cmd.address = 0x111;
	cmd.data = (u8)(block_size >> 8);
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Failed cmd52, set 0x111 data...\n");
		goto fail;
	}

	return 1;
fail:
	return 0;
}

/********************************************
 *
 *      Sdio interfaces
 *
 ********************************************/
static int sdio_write_reg(struct wilc *wilc, u32 addr, u32 data)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	int ret;

	data = cpu_to_le32(data);

	if (addr >= 0xf0 && addr <= 0xff) {
		struct sdio_cmd52 cmd;

		cmd.read_write = 1;
		cmd.function = 0;
		cmd.raw = 0;
		cmd.address = addr;
		cmd.data = data;
		ret = wilc_sdio_cmd52(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Failed cmd 52, write reg %08x ...\n", addr);
			goto fail;
		}
	} else {
		struct sdio_cmd53 cmd;

		/**
		 *      set the AHB address
		 **/
		if (!sdio_set_func0_csa_address(wilc, addr))
			goto fail;

		cmd.read_write = 1;
		cmd.function = 0;
		cmd.address = 0x10f;
		cmd.block_mode = 0;
		cmd.increment = 1;
		cmd.count = 4;
		cmd.buffer = (u8 *)&data;
		cmd.block_size = g_sdio.block_size;
		ret = wilc_sdio_cmd53(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Failed cmd53, write reg (%08x)...\n", addr);
			goto fail;
		}
	}

	return 1;

fail:

	return 0;
}

static int sdio_write(struct wilc *wilc, u32 addr, u8 *buf, u32 size)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	u32 block_size = g_sdio.block_size;
	struct sdio_cmd53 cmd;
	int nblk, nleft, ret;

	cmd.read_write = 1;
	if (addr > 0) {
		/**
		 *      has to be word aligned...
		 **/
		if (size & 0x3) {
			size += 4;
			size &= ~0x3;
		}

		/**
		 *      func 0 access
		 **/
		cmd.function = 0;
		cmd.address = 0x10f;
	} else {
		/**
		 *      has to be word aligned...
		 **/
		if (size & 0x3) {
			size += 4;
			size &= ~0x3;
		}

		/**
		 *      func 1 access
		 **/
		cmd.function = 1;
		cmd.address = 0;
	}

	nblk = size / block_size;
	nleft = size % block_size;

	if (nblk > 0) {
		cmd.block_mode = 1;
		cmd.increment = 1;
		cmd.count = nblk;
		cmd.buffer = buf;
		cmd.block_size = block_size;
		if (addr > 0) {
			if (!sdio_set_func0_csa_address(wilc, addr))
				goto fail;
		}
		ret = wilc_sdio_cmd53(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Failed cmd53 [%x], block send...\n", addr);
			goto fail;
		}
		if (addr > 0)
			addr += nblk * block_size;
		buf += nblk * block_size;
	}

	if (nleft > 0) {
		cmd.block_mode = 0;
		cmd.increment = 1;
		cmd.count = nleft;
		cmd.buffer = buf;

		cmd.block_size = block_size;

		if (addr > 0) {
			if (!sdio_set_func0_csa_address(wilc, addr))
				goto fail;
		}
		ret = wilc_sdio_cmd53(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Failed cmd53 [%x], bytes send...\n", addr);
			goto fail;
		}
	}

	return 1;

fail:

	return 0;
}

static int sdio_read_reg(struct wilc *wilc, u32 addr, u32 *data)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	int ret;

	if (addr >= 0xf0 && addr <= 0xff) {
		struct sdio_cmd52 cmd;

		cmd.read_write = 0;
		cmd.function = 0;
		cmd.raw = 0;
		cmd.address = addr;
		ret = wilc_sdio_cmd52(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Failed cmd 52, read reg (%08x) ...\n", addr);
			goto fail;
		}
		*data = cmd.data;
	} else {
		struct sdio_cmd53 cmd;

		if (!sdio_set_func0_csa_address(wilc, addr))
			goto fail;

		cmd.read_write = 0;
		cmd.function = 0;
		cmd.address = 0x10f;
		cmd.block_mode = 0;
		cmd.increment = 1;
		cmd.count = 4;
		cmd.buffer = (u8 *)data;

		cmd.block_size = g_sdio.block_size;
		ret = wilc_sdio_cmd53(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Failed cmd53, read reg (%08x)...\n", addr);
			goto fail;
		}
	}

	*data = cpu_to_le32(*data);

	return 1;

fail:

	return 0;
}

static int sdio_read(struct wilc *wilc, u32 addr, u8 *buf, u32 size)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	u32 block_size = g_sdio.block_size;
	struct sdio_cmd53 cmd;
	int nblk, nleft, ret;

	cmd.read_write = 0;
	if (addr > 0) {
		/**
		 *      has to be word aligned...
		 **/
		if (size & 0x3) {
			size += 4;
			size &= ~0x3;
		}

		/**
		 *      func 0 access
		 **/
		cmd.function = 0;
		cmd.address = 0x10f;
	} else {
		/**
		 *      has to be word aligned...
		 **/
		if (size & 0x3) {
			size += 4;
			size &= ~0x3;
		}

		/**
		 *      func 1 access
		 **/
		cmd.function = 1;
		cmd.address = 0;
	}

	nblk = size / block_size;
	nleft = size % block_size;

	if (nblk > 0) {
		cmd.block_mode = 1;
		cmd.increment = 1;
		cmd.count = nblk;
		cmd.buffer = buf;
		cmd.block_size = block_size;
		if (addr > 0) {
			if (!sdio_set_func0_csa_address(wilc, addr))
				goto fail;
		}
		ret = wilc_sdio_cmd53(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Failed cmd53 [%x], block read...\n", addr);
			goto fail;
		}
		if (addr > 0)
			addr += nblk * block_size;
		buf += nblk * block_size;
	}       /* if (nblk > 0) */

	if (nleft > 0) {
		cmd.block_mode = 0;
		cmd.increment = 1;
		cmd.count = nleft;
		cmd.buffer = buf;

		cmd.block_size = block_size;

		if (addr > 0) {
			if (!sdio_set_func0_csa_address(wilc, addr))
				goto fail;
		}
		ret = wilc_sdio_cmd53(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Failed cmd53 [%x], bytes read...\n", addr);
			goto fail;
		}
	}

	return 1;

fail:

	return 0;
}

/********************************************
 *
 *      Bus interfaces
 *
 ********************************************/

static int sdio_deinit(struct wilc *wilc)
{
	g_sdio.is_init = false;

	return 1;
}

static int sdio_init(struct wilc *wilc, bool resume)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	struct sdio_cmd52 cmd;
	int loop, ret;
	u32 chipid;

	dev_info(&func->dev, "SDIO speed: %d\n",
		func->card->host->ios.clock);
	init_waitqueue_head(&sdio_intr_waitqueue);
	g_sdio.irq_gpio = (wilc->io_type == HIF_SDIO_GPIO_IRQ);

	/**
	 *      function 0 csa enable
	 **/
	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 1;
	cmd.address = 0x100;
	cmd.data = 0x80;
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Fail cmd 52, enable csa...\n");
		goto fail;
	}

	/**
	 *      function 0 block size
	 **/
	if (!sdio_set_func0_block_size(wilc, WILC_SDIO_BLOCK_SIZE)) {
		dev_err(&func->dev, "Fail cmd 52, set func 0 block size...\n");
		goto fail;
	}
	g_sdio.block_size = WILC_SDIO_BLOCK_SIZE;

	/**
	 *      enable func1 IO
	 **/
	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 1;
	cmd.address = 0x2;
	cmd.data = 0x2;
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev,
			"Fail cmd 52, set IOE register...\n");
		goto fail;
	}

	/**
	 *      make sure func 1 is up
	 **/
	cmd.read_write = 0;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0x3;
	loop = 3;
	do {
		cmd.data = 0;
		ret = wilc_sdio_cmd52(wilc, &cmd);
		if (ret) {
			dev_err(&func->dev,
				"Fail cmd 52, get IOR register...\n");
			goto fail;
		}
		if (cmd.data == 0x2)
			break;
	} while (loop--);

	if (loop <= 0) {
		dev_err(&func->dev, "Fail func 1 is not ready...\n");
		goto fail;
	}

	/**
	 *      func 1 is ready, set func 1 block size
	 **/
	if (!sdio_set_func1_block_size(wilc, WILC_SDIO_BLOCK_SIZE)) {
		dev_err(&func->dev, "Fail set func 1 block size...\n");
		goto fail;
	}

	/**
	 *      func 1 interrupt enable
	 **/
	cmd.read_write = 1;
	cmd.function = 0;
	cmd.raw = 1;
	cmd.address = 0x4;
	cmd.data = 0x3;
	ret = wilc_sdio_cmd52(wilc, &cmd);
	if (ret) {
		dev_err(&func->dev, "Fail cmd 52, set IEN register...\n");
		goto fail;
	}

	/**
	 *      make sure can read back chip id correctly
	 **/
	if (!resume) {
		chipid = wilc_get_chipid(wilc, true);
		if(ISWILC3000(chipid)) {
			wilc->chip = WILC_3000;
		} else if(ISWILC1000(chipid)) {
			wilc->chip = WILC_1000;
		} else {
			dev_err(&func->dev, "Unsupported chipid: %x\n", chipid);
			goto fail;
		}
		dev_info(&func->dev, "chipid %08x\n", chipid);
	}

	g_sdio.is_init = true;

	return 1;

fail:

	return 0;
}

static int sdio_read_size(struct wilc *wilc, u32 *size)
{
	u32 tmp;
	struct sdio_cmd52 cmd;

	/**
	 *      Read DMA count in words
	 **/
	cmd.read_write = 0;
	cmd.function = 0;
	cmd.raw = 0;
	cmd.address = 0xf2;
	cmd.data = 0;
	wilc_sdio_cmd52(wilc, &cmd);
	tmp = cmd.data;

	/* cmd.read_write = 0; */
	/* cmd.function = 0; */
	/* cmd.raw = 0; */
	cmd.address = 0xf3;
	cmd.data = 0;
	wilc_sdio_cmd52(wilc, &cmd);
	tmp |= (cmd.data << 8);

	*size = tmp;
	return 1;
}

static int sdio_read_int(struct wilc *wilc, u32 *int_status)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	u32 tmp;
	struct sdio_cmd52 cmd;
	u32 irq_flags;
	int i;

	if (g_sdio.irq_gpio) {
		sdio_read_size(wilc, &tmp);

		cmd.read_write = 0;
		cmd.function = 1;
		cmd.raw = 0;
		cmd.data = 0;
		if(wilc->chip == WILC_1000) {
			cmd.address = 0xf7;
			wilc_sdio_cmd52(wilc, &cmd);
			irq_flags = cmd.data & 0x1f;
		} else {
			cmd.address = 0xfe;
			wilc_sdio_cmd52(wilc, &cmd);
			irq_flags = cmd.data & 0x0f;
		}
		tmp |= ((irq_flags >> 0) << IRG_FLAGS_OFFSET);

		*int_status = tmp;
	} else {
		sdio_read_size(wilc, &tmp);
		cmd.read_write = 0;
		cmd.function = 1;
		cmd.address = 0x04;
		cmd.data = 0;
		wilc_sdio_cmd52(wilc, &cmd);

		if (cmd.data & BIT(0))
			tmp |= INT_0;
		if (cmd.data & BIT(2))
			tmp |= INT_1;
		if (cmd.data & BIT(3))
			tmp |= INT_2;
		if (cmd.data & BIT(4))
			tmp |= INT_3;
		if (cmd.data & BIT(5))
			tmp |= INT_4;

		for (i = g_sdio.nint; i < MAX_NUM_INT; i++) {
			if ((tmp >> (IRG_FLAGS_OFFSET + i)) & 0x1) {
				dev_err(&func->dev,
					"Unexpected interrupt (1) : tmp=%x, data=%x\n",
					tmp, cmd.data);
				break;
			}
		}

		*int_status = tmp;

	}

	return 1;
}

static int sdio_clear_int_ext(struct wilc *wilc, u32 val)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	int ret;
	u32 reg = 0;

	if(wilc->chip == WILC_1000) {
		if(g_sdio.irq_gpio)
			reg = val & (BIT(MAX_NUM_INT) - 1);

		/* select VMM table 0 */
		if (val & SEL_VMM_TBL0)
			reg |= BIT(5);
		/* select VMM table 1 */
		if (val & SEL_VMM_TBL1)
			reg |= BIT(6);
		/* enable VMM */
		if (val & EN_VMM)
			reg |= BIT(7);
		if (reg) {
			struct sdio_cmd52 cmd;

			cmd.read_write = 1;
			cmd.function = 0;
			cmd.raw = 0;
			cmd.address = 0xf8;
			cmd.data = reg;

			ret = wilc_sdio_cmd52(wilc, &cmd);
			if (ret) {
				dev_err(&func->dev,
					"Failed cmd52, set 0xf8 data (%d) ...\n",
					__LINE__);
				goto fail;
			}
		}
	} else {
		if(g_sdio.irq_gpio) {
			reg = val & (BIT(MAX_NUM_INT) - 1);
			if (reg) {
				struct sdio_cmd52 cmd;

				cmd.read_write = 1;
				cmd.function = 0;
				cmd.raw = 0;
				cmd.address = 0xfe;
				cmd.data = reg;

				ret = wilc_sdio_cmd52(wilc, &cmd);
				if (ret) {
					dev_err(&func->dev,
						"Failed cmd52, set 0xf8 data (%d) ...\n",
						__LINE__);
					goto fail;
				}
			}
		}
		/* select VMM table 0 */
		if (val & SEL_VMM_TBL0)
			reg |= BIT(0);
		/* select VMM table 1 */
		if (val & SEL_VMM_TBL1)
			reg |= BIT(1);
		/* enable VMM */
		if (val & EN_VMM)
			reg |= BIT(2);

		if (reg) {
			struct sdio_cmd52 cmd;

			cmd.read_write = 1;
			cmd.function = 0;
			cmd.raw = 0;
			cmd.address = 0xf1;
			cmd.data = reg;

			ret = wilc_sdio_cmd52(wilc, &cmd);
			if (ret) {
				dev_err(&func->dev,
					"Failed cmd52, set 0xf6 data (%d) ...\n",
					__LINE__);
				goto fail;
			}
		}
	}

	return 1;
fail:
	return 0;
}

static int sdio_sync_ext(struct wilc *wilc, int nint)
{
	struct sdio_func *func = dev_to_sdio_func(wilc->dev);
	u32 reg;
	int ret, i;

	if (nint > MAX_NUM_INT) {
		dev_err(&func->dev,"Too many interrupts %d\n", nint);
		return 0;
	}

	g_sdio.nint = nint;

/* WILC3000 only. Was removed in WILC1000 on revision 6200.
 * Might be related to suspend/resume
 */
 	if(wilc->chip == WILC_3000) {
		/**
		 *      Disable power sequencer
		 **/
		if (!sdio_read_reg(wilc, WILC_MISC, &reg)) {
			dev_err(&func->dev, "Failed read misc reg\n");
			return 0;
		}
		reg &= ~BIT(8);
		if (!sdio_write_reg(wilc, WILC_MISC, reg)) {
			dev_err(&func->dev, "Failed write misc reg\n");
			return 0;
		}
	}

	if (g_sdio.irq_gpio) {
		/**
		 *      interrupt pin mux select
		 **/
		ret = sdio_read_reg(wilc, WILC_PIN_MUX_0, &reg);
		if (!ret) {
			dev_err(&func->dev, "Failed read reg (%08x)...\n",
				WILC_PIN_MUX_0);
			return 0;
		}
		reg |= BIT(8);
		ret = sdio_write_reg(wilc, WILC_PIN_MUX_0, reg);
		if (!ret) {
			dev_err(&func->dev, "Failed write reg (%08x)...\n",
				WILC_PIN_MUX_0);
			return 0;
		}

		/**
		 *      interrupt enable
		 **/
		ret = sdio_read_reg(wilc, WILC_INTR_ENABLE, &reg);
		if (!ret) {
			dev_err(&func->dev, "Failed read reg (%08x)...\n",
				WILC_INTR_ENABLE);
			return 0;
		}

		for (i = 0; (i < 5) && (nint > 0); i++, nint--)
			reg |= BIT((27 + i));
		ret = sdio_write_reg(wilc, WILC_INTR_ENABLE, reg);
		if (!ret) {
			dev_err(&func->dev, "Failed write reg (%08x)...\n",
				WILC_INTR_ENABLE);
			return 0;
		}
		if (nint) {
			ret = sdio_read_reg(wilc, WILC_INTR2_ENABLE, &reg);
			if (!ret) {
				dev_err(&func->dev,
					"Failed read reg (%08x)...\n",
					WILC_INTR2_ENABLE);
				return 0;
			}

			for (i = 0; (i < 3) && (nint > 0); i++, nint--)
				reg |= BIT(i);

			ret = sdio_read_reg(wilc, WILC_INTR2_ENABLE, &reg);
			if (!ret) {
				dev_err(&func->dev,
					"Failed write reg (%08x)...\n",
					WILC_INTR2_ENABLE);
				return 0;
			}
		}
	}
	return 1;
}

/********************************************
 *
 *      Global sdio HIF function table
 *
 ********************************************/

static const struct wilc_hif_func wilc_hif_sdio = {
	.hif_init = sdio_init,
	.hif_deinit = sdio_deinit,
	.hif_read_reg = sdio_read_reg,
	.hif_write_reg = sdio_write_reg,
	.hif_block_rx = sdio_read,
	.hif_block_tx = sdio_write,
	.hif_read_int = sdio_read_int,
	.hif_clear_int_ext = sdio_clear_int_ext,
	.hif_read_size = sdio_read_size,
	.hif_block_tx_ext = sdio_write,
	.hif_block_rx_ext = sdio_read,
	.hif_sync_ext = sdio_sync_ext,
	.enable_interrupt = wilc_sdio_enable_interrupt,
	.disable_interrupt = wilc_sdio_disable_interrupt,
	.hif_reset = wilc_sdio_reset,
	.hif_is_init = sdio_is_init,
};

