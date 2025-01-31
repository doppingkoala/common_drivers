// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

//#define DEBUG
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/dma-mapping.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio_func.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/interrupt.h>
#include <linux/bitfield.h>
#include <linux/pinctrl/consumer.h>
#include <linux/amlogic/aml_sd.h>
#include <linux/delay.h>
#include <core.h>
#include <mmc_ops.h>
#include <linux/time.h>
#include <linux/random.h>
#include <linux/gpio/consumer.h>
#include <linux/sched/clock.h>
#include <linux/debugfs.h>
#include "mmc_key.h"
#include "mmc_dtb.h"
#include <trace/hooks/mmc.h>
#include <linux/moduleparam.h>
#include <linux/amlogic/gki_module.h>

#include "meson-cqhci.h"

struct mmc_gpio {
	struct gpio_desc *ro_gpio;
	struct gpio_desc *cd_gpio;
	irqreturn_t (*cd_gpio_isr)(int irq, void *dev_id);
	char *ro_label;
	char *cd_label;
	u32 cd_debounce_delay_ms;
};

static struct wifi_clk_table wifi_clk[WIFI_CLOCK_TABLE_MAX] = {
	{"8822BS", 0, 0xb822, 167000000},
	{"8822CS", 0, 0xc822, 167000000},
	{"qca6174", 0, 0x50a, 167000000}
};

//extern struct mmc_host *sdio_host;

#if CONFIG_AMLOGIC_KERNEL_VERSION == 13515
void mmc_sd_update_cmdline_timing(void *data, struct mmc_card *card, int *err)
{
	/* nothing */
	*err = 0;
}

void mmc_sd_update_dataline_timing(void *data, struct mmc_card *card, int *err)
{
	/* nothing */
	*err = 0;
}

//#define SD_CMD_TIMING mmc_sd_update_cmdline_timing
//#define SD_DATA_TIMING mmc_sd_update_dataline_timing
#endif

static inline u32 aml_mv_dly1_nommc(u32 x)
{
	return (x) | ((x) << 6) | ((x) << 12) | ((x) << 18);
}

static inline u32 aml_mv_dly1(u32 x)
{
	return (x) | ((x) << 6) | ((x) << 12) | ((x) << 18) | ((x) << 24);
}

static inline u32 aml_mv_dly2(u32 x)
{
	return (x) | ((x) << 6) | ((x) << 12) | ((x) << 24);
}

static inline u32 aml_mv_dly2_nocmd(u32 x)
{
	return (x) | ((x) << 6) | ((x) << 12);
}

//int aml_disable_mmc_cqe(struct mmc_card *card)
//{
//	int ret = 0;
//
//	if (card->reenable_cmdq && card->ext_csd.cmdq_en) {
//		pr_debug("[%s] [%d]\n", __func__, __LINE__);
//		ret = mmc_cmdq_disable(card);
//		if (ret)
//			pr_err("[%s] disable cqe mode failed\n", __func__);
//	}
//	return ret;
//}
//
//int aml_enable_mmc_cqe(struct mmc_card *card)
//{
//	int ret = 0;
//
//	if (card->reenable_cmdq && !card->ext_csd.cmdq_en) {
//		pr_debug("[%s] [%d]\n", __func__, __LINE__);
//		ret = mmc_cmdq_enable(card);
//		if (ret)
//			pr_err("[%s] reenable cqe mode failed\n", __func__);
//	}
//	return ret;
//}

static int tdma_of_parse(struct mmc_host *mmc, u32 index)
{
	struct device *dev = mmc->parent;
	struct amlsd_platform *pdata = mmc_priv(mmc);
	u32 cd_debounce_delay_ms;
	int ret = 0;

	if (!dev || !dev_fwnode(dev))
		return 0;

	mmc->caps |= MMC_CAP_4_BIT_DATA;

	if (index == MMC_MULT_DEV_SEQ_SDIO) {
		mmc->caps |= MMC_CAP_NONREMOVABLE;
		device_property_read_u32(dev, "max-frequency-sdio", &mmc->f_max);
	} else {
		device_property_read_u32(dev, "max-frequency-sd", &mmc->f_max);

		if (device_property_read_bool(dev, "cd-inverted"))
			mmc->caps2 |= MMC_CAP2_CD_ACTIVE_HIGH;

		if (device_property_read_u32(dev, "cd-debounce-delay-ms",
					     &cd_debounce_delay_ms))
			cd_debounce_delay_ms = 200;

		if (device_property_read_bool(dev, "broken-cd"))
			mmc->caps |= MMC_CAP_NEEDS_POLL;

		ret = mmc_gpiod_request_cd(mmc, "cd", 0, false,
					   cd_debounce_delay_ms * 1000);
		if (!ret) {
			dev_info(mmc->parent, "Got CD GPIO\n");
		} else if (ret != -ENOENT) {
			dev_info(mmc->parent, "[%s] ret:%d\n", __func__, ret);
			return ret;
		}
	}

	if (device_property_read_bool(dev, "disable-wp"))
		mmc->caps2 |= MMC_CAP2_NO_WRITE_PROTECT;
	if (device_property_read_bool(dev, "cap-sd-highspeed"))
		mmc->caps |= MMC_CAP_SD_HIGHSPEED;
	if (device_property_read_bool(dev, "cap-mmc-highspeed"))
		mmc->caps |= MMC_CAP_MMC_HIGHSPEED;

	if (index == MMC_MULT_DEV_SEQ_SDIO) {
		if (device_property_read_bool(dev, "sd-uhs-sdr12"))
			mmc->caps |= MMC_CAP_UHS_SDR12;
		if (device_property_read_bool(dev, "sd-uhs-sdr25"))
			mmc->caps |= MMC_CAP_UHS_SDR25;
		if (device_property_read_bool(dev, "sd-uhs-sdr50"))
			mmc->caps |= MMC_CAP_UHS_SDR50;
		if (device_property_read_bool(dev, "sd-uhs-sdr104"))
			mmc->caps |= MMC_CAP_UHS_SDR104;
		if (device_property_read_bool(dev, "cap-sdio-irq"))
			mmc->caps |= MMC_CAP_SDIO_IRQ;
		if (device_property_read_bool(dev, "keep-power-in-suspend"))
			mmc->pm_caps |= MMC_PM_KEEP_POWER;
		if (device_property_read_bool(dev, "non-removable"))
			mmc->caps |= MMC_CAP_NONREMOVABLE;
	}
	if (device_property_read_bool(dev, "no-sdio"))
		mmc->caps2 |= MMC_CAP2_NO_SDIO;
	if (device_property_read_bool(dev, "no-sd"))
		mmc->caps2 |= MMC_CAP2_NO_SD;
	if (device_property_read_bool(dev, "no-mmc"))
		mmc->caps2 |= MMC_CAP2_NO_MMC;

	if (index == MMC_MULT_DEV_SEQ_SD) {
		mmc->caps2 &= ~(MMC_CAP2_NO_SD);
		pdata->card_type = CARD_TYPE_NON_SDIO;
	}
	if (index == MMC_MULT_DEV_SEQ_SDIO) {
		mmc->caps2 &= ~(MMC_CAP2_NO_SDIO);
		pdata->card_type = CARD_TYPE_SDIO;
	}
	return ret;
}

static int amlogic_of_parse(struct mmc_host *mmc)
{
	struct device *dev = mmc->parent;
	struct amlsd_platform *pdata = mmc_priv(mmc);

	if (device_property_read_u32(dev, "init_core_phase",
			&pdata->sd_mmc.init.core_phase) < 0)
		pdata->sd_mmc.init.core_phase = 2;
	if (device_property_read_u32(dev, "init_tx_phase",
			&pdata->sd_mmc.init.tx_phase) < 0)
		pdata->sd_mmc.init.tx_delay = 0;
	if (device_property_read_u32(dev, "hs2_core_phase",
			&pdata->sd_mmc.hs2.core_phase) < 0)
		pdata->sd_mmc.hs2.core_phase = 2;
	if (device_property_read_u32(dev, "hs2_tx_phase",
			&pdata->sd_mmc.hs2.tx_phase) < 0)
		pdata->sd_mmc.hs2.tx_delay = 0;
	if (device_property_read_u32(dev, "hs4_core_phase",
			&pdata->sd_mmc.hs4.core_phase) < 0)
		pdata->sd_mmc.hs4.core_phase = 0;
	if (device_property_read_u32(dev, "hs4_tx_phase",
			&pdata->sd_mmc.hs4.tx_phase) < 0)
		pdata->sd_mmc.hs4.tx_phase = 0;
	if (device_property_read_u32(dev, "src_clk_rate",
			&pdata->src_clk_rate) < 0)
		pdata->src_clk_rate = 1000000000;
	if (device_property_read_u32(dev, "sdr_tx_delay",
			&pdata->sd_mmc.sdr.tx_delay) < 0)
		pdata->sd_mmc.sdr.tx_delay = 0;
	if (device_property_read_u32(dev, "sdr_core_phase",
			&pdata->sd_mmc.sdr.core_phase) < 0)
		pdata->sd_mmc.sdr.core_phase = 2;
	if (device_property_read_u32(dev, "sdr_tx_phase",
			&pdata->sd_mmc.sdr.tx_phase) < 0)
		pdata->sd_mmc.sdr.tx_phase = 0;

	if (device_property_read_u32(dev, "card_type",
			&pdata->card_type) < 0)
		dev_err(mmc->parent,
			"No config cart type value\n");

	if (device_property_read_bool(dev, "mmc_debug_flag"))
		pdata->debug_flag = 0;
	else
		pdata->debug_flag = 1;

	return 0;
}

/*
 * Checks that a normal transfer didn't have any errors
 */
//static int mmc_check_result(struct mmc_request *mrq)
//{
//	int ret;
//
//	WARN_ON(!mrq || !mrq->cmd || !mrq->data);
//
//	ret = 0;
//
//	if (!ret && mrq->cmd->error)
//		ret = mrq->cmd->error;
//	if (!ret && mrq->data->error)
//		ret = mrq->data->error;
//	if (!ret && mrq->stop && mrq->stop->error)
//		ret = mrq->stop->error;
//	if (!ret && mrq->data->bytes_xfered !=
//			mrq->data->blocks * mrq->data->blksz)
//		ret = RESULT_FAIL;
//
//	if (ret == -EINVAL)
//		ret = RESULT_UNSUP_HOST;
//
//	return ret;
//}

//static void mmc_prepare_mrq(struct mmc_card *card,
//			    struct mmc_request *mrq, struct scatterlist *sg,
//			    unsigned int sg_len, unsigned int dev_addr,
//			    unsigned int blocks,
//			    unsigned int blksz, int write)
//{
//	WARN_ON(!mrq || !mrq->cmd || !mrq->data || !mrq->stop);
//
//	if (blocks > 1) {
//		mrq->cmd->opcode = write ?
//			MMC_WRITE_MULTIPLE_BLOCK : MMC_READ_MULTIPLE_BLOCK;
//	} else {
//		mrq->cmd->opcode = write ?
//			MMC_WRITE_BLOCK : MMC_READ_SINGLE_BLOCK;
//	}
//
//	mrq->cmd->arg = dev_addr;
//	if (!mmc_card_is_blockaddr(card))
//		mrq->cmd->arg <<= 9;
//
//	mrq->cmd->flags = MMC_RSP_R1 | MMC_CMD_ADTC;
//
//	if (blocks == 1) {
//		mrq->stop = NULL;
//	} else {
//		mrq->stop->opcode = MMC_STOP_TRANSMISSION;
//		mrq->stop->arg = 0;
//		mrq->stop->flags = MMC_RSP_R1B | MMC_CMD_AC;
//	}
//
//	mrq->data->blksz = blksz;
//	mrq->data->blocks = blocks;
//	mrq->data->flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;
//	mrq->data->sg = sg;
//	mrq->data->sg_len = sg_len;
//
//	mmc_set_data_timeout(mrq->data, card);
//}

//static unsigned int mmc_capacity(struct mmc_card *card)
//{
//	if (!mmc_card_sd(card) && mmc_card_is_blockaddr(card))
//		return card->ext_csd.sectors;
//	else
//		return card->csd.capacity << (card->csd.read_blkbits - 9);
//}

//static int mmc_transfer(struct mmc_card *card, unsigned int dev_addr,
//			unsigned int blocks, void *buf, int write)
//{
//	u8 original_part_config;
//	u8 user_part_number = 0;
//	u8 cur_part_number;
//	bool switch_partition = false;
//	unsigned int size;
//	struct scatterlist sg;
//	struct mmc_request mrq = {0};
//	struct mmc_command cmd = {0};
//	struct mmc_command stop = {0};
//	struct mmc_data data = {0};
//	int ret;
//
//	cur_part_number = card->ext_csd.part_config
//		& EXT_CSD_PART_CONFIG_ACC_MASK;
//	if (cur_part_number != user_part_number) {
//		switch_partition = true;
//		original_part_config = card->ext_csd.part_config;
//		cur_part_number = original_part_config
//			& (~EXT_CSD_PART_CONFIG_ACC_MASK);
//		ret = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
//				 EXT_CSD_PART_CONFIG, cur_part_number,
//				 card->ext_csd.part_time);
//		if (ret)
//			return ret;
//
//		card->ext_csd.part_config = cur_part_number;
//	}
//	if ((dev_addr + blocks) >= mmc_capacity(card)) {
//		pr_info("[%s] %s range exceeds device capacity!\n",
//			__func__, write ? "write" : "read");
//		ret = -1;
//		return ret;
//	}
//
//	size = blocks << card->csd.read_blkbits;
//	sg_init_one(&sg, buf, size);
//
//	mrq.cmd = &cmd;
//	mrq.data = &data;
//	mrq.stop = &stop;
//
//	mmc_prepare_mrq(card, &mrq, &sg, 1, dev_addr,
//			blocks, 1 << card->csd.read_blkbits, write);
//
//	mmc_wait_for_req(card->host, &mrq);
//
//	ret = mmc_check_result(&mrq);
//	if (switch_partition) {
//		ret = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
//				 EXT_CSD_PART_CONFIG, original_part_config,
//				 card->ext_csd.part_time);
//		if (ret)
//			return ret;
//		card->ext_csd.part_config = original_part_config;
//	}
//
//	return ret;
//}

//static int mmc_read_internal(struct mmc_card *card, unsigned int dev_addr,
//			unsigned int blocks, void *buf)
//{
//	return mmc_transfer(card, dev_addr, blocks, buf, 0);
//}
//
//static int mmc_write_internal(struct mmc_card *card, unsigned int dev_addr,
//			unsigned int blocks, void *buf)
//{
//	return mmc_transfer(card, dev_addr, blocks, buf, 1);
//}

static unsigned int meson_mmc_get_timeout_msecs(struct mmc_data *data)
{
	unsigned int timeout = data->timeout_ns / NSEC_PER_MSEC;

	if (!timeout)
		return SD_EMMC_CMD_TIMEOUT_DATA;

	timeout = roundup_pow_of_two(timeout);

	return min(timeout, 32768U); /* max. 2^15 ms */
}

static void meson_mmc_get_transfer_mode(struct mmc_host *mmc,
					struct mmc_request *mrq)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	struct mmc_data *data = mrq->data;
	struct scatterlist *sg;
	int i;
	bool use_desc_chain_mode = true;

	/*
	 * When Controller DMA cannot directly access DDR memory, disable
	 * support for Chain Mode to directly use the internal SRAM using
	 * the bounce buffer mode.
	 */
	if (host->dram_access_quirk)
		return;

	/*
	 * Broken SDIO with AP6255-based WiFi on Khadas VIM Pro has been
	 * reported. For some strange reason this occurs in descriptor
	 * chain mode only. So let's fall back to bounce buffer mode
	 * for command SD_IO_RW_EXTENDED.
	 */
	/*if (mrq->cmd->opcode == SD_IO_RW_EXTENDED)
	 *	return;
	 */

	for_each_sg(data->sg, sg, data->sg_len, i)
		/* check for 8 byte alignment */
		if (sg->offset & 7) {
			use_desc_chain_mode = false;
			break;
		}

	if (use_desc_chain_mode)
		data->host_cookie |= SD_EMMC_DESC_CHAIN_MODE;
}

static inline bool meson_mmc_desc_chain_mode(const struct mmc_data *data)
{
	return data->host_cookie & SD_EMMC_DESC_CHAIN_MODE;
}

static inline bool meson_mmc_bounce_buf_read(const struct mmc_data *data)
{
	return data && data->flags & MMC_DATA_READ &&
	       !meson_mmc_desc_chain_mode(data);
}

static void meson_mmc_pre_req(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct mmc_data *data = mrq->data;

	if (!data)
		return;

	meson_mmc_get_transfer_mode(mmc, mrq);
	data->host_cookie |= SD_EMMC_PRE_REQ_DONE;

	if (!meson_mmc_desc_chain_mode(data))
		return;

	data->sg_count = dma_map_sg(mmc_dev(mmc), data->sg, data->sg_len,
				    mmc_get_dma_dir(data));
	if (!data->sg_count)
		dev_err(mmc_dev(mmc), "dma_map_sg failed");
}

static void meson_mmc_post_req(struct mmc_host *mmc, struct mmc_request *mrq,
			       int err)
{
	struct mmc_data *data = mrq->data;

	if (data && meson_mmc_desc_chain_mode(data) && data->sg_count)
		dma_unmap_sg(mmc_dev(mmc), data->sg, data->sg_len,
			     mmc_get_dma_dir(data));
}

/*
 * Gating the clock on this controller is tricky.  It seems the mmc clock
 * is also used by the controller.  It may crash during some operation if the
 * clock is stopped.  The safest thing to do, whenever possible, is to keep
 * clock running at stop it at the pad using the pinmux.
 */
static void meson_mmc_clk_gate(struct mmc_host *mmc)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 cfg;

	if (pdata->pins_clk_gate) {
		pinctrl_select_state(host->pinctrl, pdata->pins_clk_gate);
	} else {
		/*
		 * If the pinmux is not provided - default to the classic and
		 * unsafe method
		 */
		cfg = readl(host->regs + SD_EMMC_CFG);
		cfg |= CFG_STOP_CLOCK;
		writel(cfg, host->regs + SD_EMMC_CFG);
	}
}

static void meson_mmc_clk_ungate(struct mmc_host *mmc)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 cfg;

	if (pdata->pins_clk_gate)
		pinctrl_select_state(host->pinctrl, pdata->pins_default);

	/* Make sure the clock is not stopped in the controller */
	cfg = readl(host->regs + SD_EMMC_CFG);
	cfg &= ~CFG_STOP_CLOCK;
	writel(cfg, host->regs + SD_EMMC_CFG);
}

static void meson_mmc_set_phase_delay(struct mmc_host *mmc, u32 mask,
				      unsigned int phase)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 val;

	val = readl(host->regs);
	val &= ~mask;
	val |= phase << __ffs(mask);
	writel(val, host->regs);
}

static int no_pxp_clk_set(struct mmc_host *mmc, struct mmc_ios *ios,
						unsigned long rate)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	int ret = 0;
	struct clk *src_clk = NULL;
	u32 cfg = readl(host->regs + SD_EMMC_CFG);

	dev_dbg(host->dev, "[%s]set rate:%lu, %s\n", __func__, rate, mmc_hostname(mmc));
	if (pdata->src_clk)
		clk_disable_unprepare(pdata->src_clk);

	switch (ios->timing) {
	case MMC_TIMING_MMC_HS400:
		if (host->clk[2])
			src_clk = host->clk[2];
		else
			src_clk = host->clk[1];
		dev_dbg(host->dev, "HS400 set src rate to:%u\n",
			pdata->src_clk_rate);
		ret = clk_set_rate(src_clk, pdata->src_clk_rate);
		if (ret) {
			dev_err(host->dev, "set src err\n");
				return ret;
		}
		cfg |= CFG_AUTO_CLK;
		break;
	case MMC_TIMING_MMC_HS:
	case MMC_TIMING_SD_HS:
	case MMC_TIMING_MMC_DDR52:
	case MMC_TIMING_UHS_DDR50:
	case MMC_TIMING_MMC_HS200:
	case MMC_TIMING_UHS_SDR12:
	case MMC_TIMING_UHS_SDR25:
	case MMC_TIMING_UHS_SDR50:
	case MMC_TIMING_UHS_SDR104:
		dev_dbg(host->dev, "[%s]Other mode set src rate to:%u, %s\n",
				__func__, pdata->src_clk_rate, mmc_hostname(mmc));
		ret = clk_set_rate(host->clk[1], pdata->src_clk_rate);
		if (ret) {
			dev_err(host->dev, "set src err\n");
				return ret;
		}
		src_clk = host->clk[1];
		cfg |= CFG_AUTO_CLK;
	/* sdio set clk always on default */
		if (aml_card_type_sdio(pdata) && !pdata->auto_clk)
			cfg &= ~CFG_AUTO_CLK;
		break;
	case MMC_TIMING_LEGACY:
		dev_dbg(host->dev, "[%s]Legacy set rate to:%lu, %s\n",
				__func__, rate, mmc_hostname(mmc));
		src_clk = host->clk[0];
	/* enable always on clock for 400KHZ */
		cfg &= ~CFG_AUTO_CLK;

	/* switch source clock as Total before clk =0, then disable source clk */
		if (rate == 0) {
			if (host->mux_div && (!strcmp(__clk_get_name(src_clk), "xtal")))
				ret = clk_set_parent(host->mux[2], src_clk);
			else
				ret = clk_set_parent(host->mux[0], src_clk);
			pdata->src_clk = NULL;
			cfg |= CFG_AUTO_CLK;
			writel(cfg, host->regs + SD_EMMC_CFG);
			return ret;
		}
		break;
	default:
		dev_notice(host->dev, "Check mmc/sd/sdio timing mode\n");
		WARN_ON(1);
		break;
	}

	clk_prepare_enable(src_clk);
	writel(cfg, host->regs + SD_EMMC_CFG);
	pdata->src_clk = src_clk;
	if (host->mux_div) { // C1/C2
		if (!strcmp(__clk_get_name(src_clk), "xtal")) {
			ret = clk_set_parent(host->mux[2], src_clk);
		} else {
			ret = clk_set_parent(host->mux[0], src_clk);
			if (!ret) {
				clk_set_rate(host->mux_div, clk_get_rate((src_clk)));
				ret = clk_set_parent(host->mux[2], host->mux_div);
			}
		}
	} else { // other soc
		ret = clk_set_parent(host->mux[0], src_clk);
	}

	if (ret) {
		dev_err(host->dev, "set parent error\n");
		return ret;
	}

	ret = clk_set_rate(host->mmc_clk, rate);
	if (ret) {
		dev_err(host->dev, "Unable to set cfg_div_clk to %lu. ret=%d\n",
			rate, ret);
		return ret;
	}
	host->req_rate = rate;
	pdata->req_rate = rate;
	mmc->actual_clock = clk_get_rate(host->mmc_clk);

	dev_dbg(host->dev, "clk rate: %u Hz, %s\n", mmc->actual_clock, mmc_hostname(mmc));

	return ret;
}

/*
 * The SD/eMMC IP block has an internal mux and divider used for
 * generating the MMC clock.  Use the clock framework to create and
 * manage these clocks.
 */
static int meson_mmc_clk_init(struct meson_host *host)
{
	struct clk_init_data init;
	struct clk_divider *div;
	char clk_name[32], name[16];
	int i, ret = 0;
	const char *clk_parent[1];
	u32 clk_reg;

	/* init SD_EMMC_CLOCK to sane defaults w/min clock rate */
	clk_reg = CLK_ALWAYS_ON(host);
	clk_reg |= CLK_DIV_MASK;
	clk_reg |= FIELD_PREP(CLK_CORE_PHASE_MASK, CLK_PHASE_180);
	clk_reg |= FIELD_PREP(CLK_TX_PHASE_MASK, CLK_PHASE_0);
	clk_reg |= FIELD_PREP(CLK_RX_PHASE_MASK, CLK_PHASE_0);
	writel(clk_reg, host->regs + SD_EMMC_CLOCK);

	for (i = 0; i < 2; i++) {
		snprintf(name, sizeof(name), "mux%d", i);
		host->mux[i] = devm_clk_get(host->dev, name);
		if (IS_ERR(host->mux[i])) {
			if (host->mux[i] != ERR_PTR(-EPROBE_DEFER))
				dev_err(host->dev, "Missing clock %s\n", name);
			return PTR_ERR(host->mux[i]);
		}
	}
	host->mux_div = devm_clk_get(host->dev, "mux_div");
	if (IS_ERR(host->mux_div)) {
		host->mux_div = NULL;
		dev_dbg(host->dev,
			"Missing clock %s(only c1/c2 have mux_div)\n",
			"mux_div");
	} else {
		snprintf(name, sizeof(name), "mux%d", 2);
		host->mux[2] = devm_clk_get(host->dev, name);
		if (IS_ERR(host->mux[2]))
			dev_err(host->dev, "Missing clock %s\n", "mux2");
	}

	for (i = 0; i < 3; i++) {
		snprintf(name, sizeof(name), "clkin%d", i);
		host->clk[i] = devm_clk_get(host->dev, name);
		if (IS_ERR(host->clk[i])) {
			dev_dbg(host->dev,
				"Missing clock%s, i = %d\n", name, i);
			host->clk[i] = NULL;
		}
	}

	if (host->mux_div) {
		ret = clk_set_parent(host->mux[2], host->mux_div);
		if (ret) {
			dev_err(host->dev, "Set div parent error\n");
			return ret;
		}
	}

	/* create the divider */
	div = devm_kzalloc(host->dev, sizeof(*div), GFP_KERNEL);
	if (!div)
		return -ENOMEM;

	snprintf(clk_name, sizeof(clk_name), "%s#div", dev_name(host->dev));
	init.name = clk_name;
	init.ops = &clk_divider_ops;
	clk_parent[0] = __clk_get_name(host->mux[1]);
	init.parent_names = clk_parent;
	init.num_parents = 1;
	init.flags = CLK_SET_RATE_PARENT;

	div->reg = host->regs + SD_EMMC_CLOCK;
	div->shift = __ffs(CLK_DIV_MASK);
	div->width = __builtin_popcountl(CLK_DIV_MASK);
	div->hw.init = &init;
	div->flags = CLK_DIVIDER_ONE_BASED;

	host->mmc_clk = devm_clk_register(host->dev, &div->hw);
	if (WARN_ON(IS_ERR(host->mmc_clk)))
		return PTR_ERR(host->mmc_clk);
	/* create the mmc core clock */
	if (host->mux_div)
		ret = clk_set_parent(host->mux[2], host->clk[0]);
	else
		ret = clk_set_parent(host->mux[0], host->clk[0]);
	if (ret) {
		dev_err(host->dev, "Set 24m parent error\n");
		return ret;
	}
	/* init SD_EMMC_CLOCK to sane defaults w/min clock rate */
	host->f_min = clk_round_rate(host->mmc_clk, 400000);
	ret = clk_set_rate(host->mmc_clk, host->f_min);
	if (ret)
		return ret;

	return clk_prepare_enable(host->mmc_clk);
}

static int meson_mmc_set_adjust(struct mmc_host *mmc, u32 value)
{
	u32 val;
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;

	val = readl(host->regs + SD_EMMC_V3_ADJUST);
	val &= ~CLK_ADJUST_DELAY;
	val &= ~CFG_ADJUST_ENABLE;
	val |= CFG_ADJUST_ENABLE;
	val |= value << __ffs(CLK_ADJUST_DELAY);

	writel(val, host->regs + SD_EMMC_V3_ADJUST);
	return 0;
}

static int meson_mmc_tuning_transfer(struct mmc_host *mmc, u32 opcode)
{
	int tuning_err = 0;
	int n, nmatch;
	/* try ntries */
	for (n = 0, nmatch = 0; n < TUNING_NUM_PER_POINT; n++) {
		tuning_err = mmc_send_tuning(mmc, opcode, NULL);
		if (!tuning_err) {
			nmatch++;
		} else {
		/* After the cmd21 command fails,
		 * it takes a certain time for the emmc status to
		 * switch from data back to transfer. Currently,
		 * only this model has this problem.
		 * by add usleep_range(20, 30);
		 */
			usleep_range(20, 30);
			break;
		}
	}
	return nmatch;
}

static int find_best_win(struct mmc_host *mmc,
		char *buf, int num, int *b_s, int *b_sz, bool wrap_f)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	int wrap_win_start = -1, wrap_win_size = 0;
	int curr_win_start = -1, curr_win_size = 0;
	int best_win_start = -1, best_win_size = 0;
	int i = 0, len = 0;
	u8 *adj_print = NULL;

	len = 0;
	adj_print = host->adj_win;
	memset(adj_print, 0, sizeof(u8) * ADJ_WIN_PRINT_MAXLEN);
	len += sprintf(adj_print, "%s: adj_win: < ", mmc_hostname(mmc));

	for (i = 0; i < num; i++) {
		/*get a ok adjust point!*/
		if (buf[i]) {
			if (i == 0)
				wrap_win_start = i;

			if (wrap_win_start >= 0)
				wrap_win_size++;

			if (curr_win_start < 0)
				curr_win_start = i;

			curr_win_size++;
			len += sprintf(adj_print + len,
					"%d ", i);
		} else {
			if (curr_win_start >= 0) {
				if (best_win_start < 0) {
					best_win_start = curr_win_start;
					best_win_size = curr_win_size;
				} else {
					if (best_win_size < curr_win_size) {
						best_win_start = curr_win_start;
						best_win_size = curr_win_size;
					}
				}
				wrap_win_start = -1;
				curr_win_start = -1;
				curr_win_size = 0;
			}
		}
	}

	sprintf(adj_print + len, ">\n");
	if (num <= AML_FIXED_ADJ_MAX)
		pr_debug("%s", host->adj_win);

	/* last point is ok! */
	if (curr_win_start >= 0) {
		if (best_win_start < 0) {
			best_win_start = curr_win_start;
			best_win_size = curr_win_size;
		} else if ((wrap_win_size > 0) && wrap_f) {
			/* Wrap around case */
			if (curr_win_size + wrap_win_size > best_win_size) {
				best_win_start = curr_win_start;
				best_win_size = curr_win_size + wrap_win_size;
			}
		} else if (best_win_size < curr_win_size) {
			best_win_start = curr_win_start;
			best_win_size = curr_win_size;
		}

		curr_win_start = -1;
		curr_win_size = 0;
	}
	*b_s = best_win_start;
	*b_sz = best_win_size;

	return 0;
}

static void pr_adj_info(char *name,
		unsigned long x, u32 fir_adj, u32 div)
{
	int i;

	pr_debug("[%s] fixed_adj_win_map:%lu\n", name, x);
	for (i = 0; i < div; i++)
		pr_debug("[%d]=%d\n", (fir_adj + i) % div,
				((x >> i) & 0x1) ? 1 : 0);
}

static unsigned long _test_fixed_adj(struct mmc_host *mmc,
		u32 opcode, u32 adj, u32 div)
{
	int i = 0;
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u8 *adj_print = host->adj_win;
	u32 len = 0;
	u32 nmatch = 0;
	unsigned long fixed_adj_map = 0;

	memset(adj_print, 0, sizeof(u8) * ADJ_WIN_PRINT_MAXLEN);
	len += sprintf(adj_print + len, "%s: adj_win: < ", mmc_hostname(mmc));
	bitmap_zero(&fixed_adj_map, div);
	for (i = 0; i < div; i++) {
		meson_mmc_set_adjust(mmc, adj + i);
		nmatch = meson_mmc_tuning_transfer(mmc, opcode);
		/*get a ok adjust point!*/
		if (nmatch == TUNING_NUM_PER_POINT) {
			set_bit(adj + i, &fixed_adj_map);
			len += sprintf(adj_print + len,
				"%d ", adj + i);
		}
		pr_debug("%s: rx_tuning_result[%d] = %d\n",
				mmc_hostname(mmc), adj + i, nmatch);
	}
	len += sprintf(adj_print + len, ">\n");
	pr_debug("%s", host->adj_win);

	return fixed_adj_map;
}

static u32 _find_fixed_adj_mid(unsigned long map,
		u32 adj, u32 div, u32 co)
{
	u32 left, right, mid, size = 0;

	left = find_last_bit(&map, div);
	right = find_first_bit(&map, div);
	/*
	 * The lib functions don't need to be modified.
	 */
	/* coverity[callee_ptr_arith:SUPPRESS] */
	mid = find_first_zero_bit(&map, div);
	size = left - right + 1;
	pr_debug("left:%u, right:%u, mid:%u, size:%u\n",
			left, right, mid, size);
	if (size >= 3 && (mid < right || mid > left)) {
		mid = (adj + (size - 1) / 2 + (size - 1) % 2) % div;
		if ((mid == (co - 1)) && div == 5)
			return NO_FIXED_ADJ_MID;
		pr_info("tuning-c:%u, tuning-s:%u\n",
			mid, size);
		return mid;
	}
	return NO_FIXED_ADJ_MID;
}

static unsigned long _swap_fixed_adj_win(unsigned long map,
		u32 shift, u32 div)
{
	unsigned long left, right;
	/*
	 * The lib functions don't need to be modified.
	 */
	/* coverity[callee_ptr_arith:SUPPRESS] */
	bitmap_shift_right(&right, &map,
			shift, div);
	bitmap_shift_left(&left, &map,
			div - shift, div);
	bitmap_or(&map, &right, &left, div);
	return map;
}

static void set_fixed_adj_line_delay(u32 step,
		struct mmc_host *mmc, bool no_cmd)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;

	if (aml_card_type_mmc(pdata)) {
		writel(aml_mv_dly1(step), host->regs + SD_EMMC_DELAY1);
		if (no_cmd)
			writel(aml_mv_dly2_nocmd(step),
					host->regs + SD_EMMC_DELAY2);
		else
			writel(aml_mv_dly2(step),
					host->regs + SD_EMMC_DELAY2);
	} else {
		writel(aml_mv_dly1_nommc(step), host->regs + SD_EMMC_DELAY1);
		if (!no_cmd)
			writel(AML_MV_DLY2_NOMMC_CMD(step),
					host->regs + SD_EMMC_DELAY2);
	}
	pr_debug("step:%u, delay1:0x%x, delay2:0x%x\n",
			step,
			readl(host->regs + SD_EMMC_DELAY1),
			readl(host->regs + SD_EMMC_DELAY2));
}

/*	1. find first removed a fixed_adj_point
 *	2. re-range fixed adj point
 *	3. retry
 */
static u32 _find_fixed_adj_valid_win(struct mmc_host *mmc,
		u32 opcode,	unsigned long *fixed_adj_map, u32 div)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 step = 0, ret = NO_FIXED_ADJ_MID, fir_adj = 0xff;
	unsigned long cur_map[1] = {0};
	unsigned long prev_map[1] = {0};
	unsigned long tmp[1] = {0};
	unsigned long dst[1] = {0};
//	struct mmc_phase *mmc_phase_init = &host->sd_mmc.init;
	u32 cop, vclk;

	vclk = readl(host->regs + SD_EMMC_CLOCK);
	cop = (vclk & CLK_CORE_PHASE_MASK) >> __ffs(CLK_CORE_PHASE_MASK);
//	cop = para->hs2.core_phase;

	div = (div == AML_FIXED_ADJ_MIN) ?
			AML_FIXED_ADJ_MIN : AML_FIXED_ADJ_MAX;
	*prev_map = *fixed_adj_map;
	pr_adj_info("prev_map", *prev_map, 0, div);
	for (; step <= 63;) {
		pr_debug("[%s]retry test fixed adj...\n", __func__);
		step += AML_FIXADJ_STEP;
		set_fixed_adj_line_delay(step, mmc, false);
		*cur_map = _test_fixed_adj(mmc, opcode, 0, div);
		/*pr_adj_info("cur_map", *cur_map, 0, div);*/
		bitmap_and(tmp, prev_map, cur_map, div);
		bitmap_xor(dst, prev_map, tmp, div);
		if (*dst != 0) {
			fir_adj = find_first_bit(dst, div);
			pr_adj_info(">>>>>>>>bitmap_xor_dst", *dst, 0, div);
			pr_debug("[%s] fir_adj:%u\n", __func__, fir_adj);

			*prev_map = _swap_fixed_adj_win(*prev_map,
					fir_adj, div);
			pr_adj_info(">>>>>>>>prev_map_range",
					*prev_map, fir_adj, div);
			ret = _find_fixed_adj_mid(*prev_map, fir_adj, div, cop);
			if (ret != NO_FIXED_ADJ_MID) {
				/* pre adj=core phase-1="hole"&&200MHZ,all line delay+step */
				if (((ret - 1) == (cop - 1)) && div == 5)
					set_fixed_adj_line_delay(AML_FIXADJ_STEP, mmc, false);
				else
					set_fixed_adj_line_delay(0, mmc, false);
				return ret;
			}

			fir_adj = (fir_adj + find_next_bit(prev_map,
				div, 1)) % div;
		}
		if (fir_adj == 0xff)
			continue;

		*prev_map = *cur_map;
		*cur_map = _swap_fixed_adj_win(*cur_map, fir_adj, div);
		pr_adj_info(">>>>>>>>cur_map_range", *cur_map, fir_adj, div);
		ret = _find_fixed_adj_mid(*cur_map, fir_adj, div, cop);
		if (ret != NO_FIXED_ADJ_MID) {
			/* pre adj=core phase-1="hole"&&200MHZ,all line delay+step */
			if (((ret - 1) == (cop - 1)) && div == 5) {
				step += AML_FIXADJ_STEP;
				set_fixed_adj_line_delay(step, mmc, false);
			}
			return ret;
		}
	}

	pr_debug("[%s][%d] no fixed adj\n", __func__, __LINE__);
	return ret;
}

static int meson_mmc_fixadj_tuning(struct mmc_host *mmc, u32 opcode)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 nmatch = 0;
	int adj_delay = 0;
	u8 tuning_num = 0;
	u32 clk_div, vclk;
	u32 old_dly, d1_dly, dly;
	u32 adj_delay_find =  0xff;
	unsigned long fixed_adj_map[1];
	bool all_flag = false;
	int best_s = -1, best_sz = 0;
	char rx_adj[64] = {0};
	u8 *adj_print = NULL;
	u32 len = 0;

	old_dly = readl(host->regs + SD_EMMC_V3_ADJUST);
	d1_dly = (old_dly >> 0x6) & 0x3F;
	pr_debug("Data 1 aligned delay is %d\n", d1_dly);
	writel(0, host->regs + SD_EMMC_V3_ADJUST);

tuning:
	/* renew */
	best_s = -1;
	best_sz = 0;
	memset(rx_adj, 0, 64);

	len = 0;
	adj_print = host->adj_win;
	memset(adj_print, 0, sizeof(u8) * ADJ_WIN_PRINT_MAXLEN);
	len += sprintf(adj_print + len, "%s: adj_win: < ", mmc_hostname(mmc));
	vclk = readl(host->regs + SD_EMMC_CLOCK);
	clk_div = vclk & CLK_DIV_MASK;
	pr_debug("%s: clk %d div %d tuning start\n",
			mmc_hostname(mmc), mmc->actual_clock, clk_div);

	if (clk_div <= AML_FIXED_ADJ_MAX)
		bitmap_zero(fixed_adj_map, clk_div);
	for (adj_delay = 0; adj_delay < clk_div; adj_delay++) {
		meson_mmc_set_adjust(mmc, adj_delay);
		nmatch = meson_mmc_tuning_transfer(mmc, opcode);
		if (nmatch == TUNING_NUM_PER_POINT) {
			rx_adj[adj_delay]++;
			if (clk_div <= AML_FIXED_ADJ_MAX)
				set_bit(adj_delay, fixed_adj_map);
			len += sprintf(adj_print + len,
				"%d ", adj_delay);
		}
	}

	len += sprintf(adj_print + len, ">\n");
	pr_debug("%s", host->adj_win);

	find_best_win(mmc, rx_adj, clk_div, &best_s, &best_sz, true);

	if (best_sz <= 0) {
		if ((tuning_num++ > MAX_TUNING_RETRY) || clk_div >= 10) {
			pr_info("%s: final result of tuning failed\n",
				 mmc_hostname(mmc));
			return -1;
		}
		clk_div++;
		vclk &= ~CLK_DIV_MASK;
		vclk |= clk_div & CLK_DIV_MASK;
		writel(vclk, host->regs + SD_EMMC_CLOCK);
		pr_info("%s: tuning failed, reduce freq and retuning\n",
			mmc_hostname(mmc));
		goto tuning;
	} else if ((best_sz < clk_div) &&
			(clk_div <= AML_FIXED_ADJ_MAX) &&
			(clk_div >= AML_FIXED_ADJ_MIN) &&
			!all_flag) {
		adj_delay_find = _find_fixed_adj_valid_win(mmc,
				opcode, fixed_adj_map, clk_div);
	} else if (best_sz == clk_div) {
		all_flag = true;
		dly = readl(host->regs + SD_EMMC_DELAY1);
		d1_dly = (dly >> 0x6) & 0x3F;
		pr_debug("%s() d1_dly %d, window start %d, size %d\n",
			__func__, d1_dly, best_s, best_sz);
		if (++d1_dly > 0x3F) {
			pr_err("%s: tuning failed\n",
				mmc_hostname(mmc));
			return -1;
		}
		dly &= ~(0x3F << 6);
		dly |= d1_dly << 6;
		writel(dly, host->regs + SD_EMMC_DELAY1);
		goto tuning;
	} else {
		pr_debug("%s: best_s = %d, best_sz = %d\n",
				mmc_hostname(mmc),
				best_s, best_sz);
	}

	if (adj_delay_find == 0xff) {
		adj_delay_find = best_s + (best_sz - 1) / 2
		+ (best_sz - 1) % 2;
		writel(old_dly, host->regs + SD_EMMC_DELAY1);
		pr_info("tuning-c:%u, tuning-s:%u\n",
			adj_delay_find % clk_div, best_sz);
	}
	adj_delay_find = adj_delay_find % clk_div;

	meson_mmc_set_adjust(mmc, adj_delay_find);

	pr_info("%s: clk= 0x%x, adj = 0x%x, dly1 = %x, dly2 = %x\n",
			mmc_hostname(mmc),
			readl(host->regs + SD_EMMC_CLOCK),
			readl(host->regs + SD_EMMC_V3_ADJUST),
			readl(host->regs + SD_EMMC_DELAY1),
			readl(host->regs + SD_EMMC_DELAY2));

	return 0;
}

//void sdio_get_card(struct mmc_host *host, struct mmc_card *card)
//{
//	host->card = card;
//}

static int sdio_get_device(void)
{
	unsigned int i, device = 0;

	if (sdio_host && sdio_host->card)
		device = sdio_host->card->cis.device;

	for (i = 0; i < ARRAY_SIZE(wifi_clk); i++) {
		if (wifi_clk[i].m_device_id == device) {
			wifi_clk[i].m_use_flag = 1;
			break;
		}
	}
	pr_debug("sdio device is 0x%x\n", device);
	return device;
}

static int meson_mmc_clk_set(struct mmc_host *mmc,
			struct mmc_ios *ios, bool ddr)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	int ret = 0;
	u32 cfg;
	unsigned long rate = ios->clock;

	/* Same request - bail-out */
	if (aml_card_type_mmc(pdata) &&
		host->ddr == ddr && host->req_rate == rate) {
		dev_notice(host->dev, "[%s]bail-out,clk rate: %lu Hz, %s\n",
			__func__, rate, mmc_hostname(mmc));
		return 0;
	}

	/* stop clock */
	meson_mmc_clk_gate(mmc);
	host->req_rate = 0;
	mmc->actual_clock = 0;

	/* Stop the clock during rate change to avoid glitches */
	cfg = readl(host->regs + SD_EMMC_CFG);
	cfg |= CFG_STOP_CLOCK;
	cfg &= ~CFG_AUTO_CLK;
	writel(cfg, host->regs + SD_EMMC_CFG);

	if (ddr) {
		/* DDR modes require higher module clock */
		rate <<= 1;
		cfg |= CFG_DDR;
	} else {
		cfg &= ~CFG_DDR;
	}
	writel(cfg, host->regs + SD_EMMC_CFG);
	host->ddr = ddr;
	pdata->ddr = ddr;

	ret = no_pxp_clk_set(mmc, ios, rate);

	/* We should report the real output frequency of the controller */
	if (ddr) {
		host->req_rate >>= 1;
		mmc->actual_clock >>= 1;
	}

	dev_dbg(host->dev, "clk rate: %u Hz, %s\n", mmc->actual_clock, mmc_hostname(mmc));
	if (rate != mmc->actual_clock)
		dev_dbg(host->dev, "requested rate was %lu\n", rate);

	/* (re)start clock */
	meson_mmc_clk_ungate(mmc);

	return ret;
}

static int meson_mmc_prepare_ios_clock(struct mmc_host *mmc,
				       struct mmc_ios *ios)
{
	//struct amlsd_platform *pdata = mmc_priv(mmc);
	//struct meson_host *host = pdata->host;
	bool ddr = false;
	int i;

	switch (ios->timing) {
	case MMC_TIMING_MMC_DDR52:
	case MMC_TIMING_UHS_DDR50:
	case MMC_TIMING_MMC_HS400:
		ddr = true;
		break;
	case MMC_TIMING_UHS_SDR104:
		for (i = 0; i < ARRAY_SIZE(wifi_clk); i++) {
			if (wifi_clk[i].m_use_flag) {
				ios->clock = wifi_clk[i].m_uhs_max_dtr;
				break;
			}
		}
		break;
	default:
		ddr = false;
		break;
	}

	return meson_mmc_clk_set(mmc, ios, ddr);
}

static void meson_mmc_check_resampling(struct mmc_host *mmc,
				       struct mmc_ios *ios)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	struct mmc_phase *mmc_phase_set;
	unsigned int val;

	if (aml_card_type_mmc(pdata) &&
		host->timing == ios->timing) {
		dev_notice(host->dev, "[%s]bail-out, timing,  %s\n",
			__func__, mmc_hostname(mmc));
		return;
	}

	writel(0, host->regs + SD_EMMC_DELAY1);
	writel(0, host->regs + SD_EMMC_DELAY2);
	writel(0, host->regs + SD_EMMC_INTF3);
	writel(0, host->regs + SD_EMMC_V3_ADJUST);
	val = readl(host->regs + SD_EMMC_IRQ_EN);
	val &= ~CFG_CMD_SETUP;
	writel(val, host->regs + SD_EMMC_IRQ_EN);
	switch (ios->timing) {
	case MMC_TIMING_MMC_HS400:
		val = readl(host->regs + SD_EMMC_V3_ADJUST);
		val |= DS_ENABLE;
		writel(val, host->regs + SD_EMMC_V3_ADJUST);
		val = readl(host->regs + SD_EMMC_IRQ_EN);
		val |= CFG_CMD_SETUP;
		writel(val, host->regs + SD_EMMC_IRQ_EN);
		val = readl(host->regs + SD_EMMC_INTF3);
		val |= SD_INTF3;
		writel(val, host->regs + SD_EMMC_INTF3);
		mmc_phase_set = &pdata->sd_mmc.hs4;
		break;
	case MMC_TIMING_MMC_HS200:
		mmc_phase_set = &pdata->sd_mmc.hs2;
		break;
	case MMC_TIMING_MMC_HS:
		val = readl(host->regs + host->data->adjust);
		val |= CFG_ADJUST_ENABLE;
		val &= ~CLK_ADJUST_DELAY;
		val |= CALI_HS_50M_ADJUST << __ffs(CLK_ADJUST_DELAY);
		writel(val, host->regs + host->data->adjust);
		mmc_phase_set = &pdata->sd_mmc.init;
		break;
	case MMC_TIMING_MMC_DDR52:
		mmc_phase_set = &pdata->sd_mmc.init;
		break;
	case MMC_TIMING_SD_HS:
		val = readl(host->regs + SD_EMMC_V3_ADJUST);
		val |= CFG_ADJUST_ENABLE;
		writel(val, host->regs + SD_EMMC_V3_ADJUST);
		mmc_phase_set = &pdata->sd_mmc.init;
		break;
	case MMC_TIMING_UHS_SDR12:
	case MMC_TIMING_UHS_SDR25:
	case MMC_TIMING_UHS_SDR50:
	case MMC_TIMING_UHS_SDR104:
		if (aml_card_type_sdio(pdata))
			sdio_get_device();
		mmc_phase_set = &pdata->sd_mmc.sdr;
		break;
	default:
		mmc_phase_set = &pdata->sd_mmc.init;
	}
	meson_mmc_set_phase_delay(mmc, CLK_CORE_PHASE_MASK,
				  mmc_phase_set->core_phase);
	meson_mmc_set_phase_delay(mmc, CLK_TX_PHASE_MASK,
				  mmc_phase_set->tx_phase);
	meson_mmc_set_phase_delay(mmc, CLK_TX_DELAY_MASK(host),
				  mmc_phase_set->tx_delay);

	host->timing = ios->timing;
	pdata->timing = ios->timing;
	dev_dbg(host->dev, "[%s]set mmc timing:%u, %s\n",
			__func__, ios->timing, mmc_hostname(mmc));
}

static void meson_mmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 bus_width, val;
	int err;

	if (host->tdma)
		wait_for_completion(&host->drv_completion);
	/*
	 * GPIO regulator, only controls switching between 1v8 and
	 * 3v3, doesn't support MMC_POWER_OFF, MMC_POWER_ON.
	 */
	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		if (!IS_ERR(mmc->supply.vmmc))
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, 0);

		if (!IS_ERR(mmc->supply.vqmmc) && pdata->vqmmc_enabled) {
			regulator_set_voltage_triplet(mmc->supply.vqmmc, 1700000, 1800000, 1950000);
			regulator_disable(mmc->supply.vqmmc);
			pdata->vqmmc_enabled = false;
		}

		break;

	case MMC_POWER_UP:
		if (!IS_ERR(mmc->supply.vmmc))
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, ios->vdd);

		break;

	case MMC_POWER_ON:
		if (!IS_ERR(mmc->supply.vqmmc) && !pdata->vqmmc_enabled) {
			int ret = regulator_enable(mmc->supply.vqmmc);

			if (ret < 0)
				dev_err(host->dev,
					"failed to enable vqmmc regulator\n");
			else
				pdata->vqmmc_enabled = true;
		}

		break;
	}
	pdata->power_mode = ios->power_mode;
	/* Bus width */
	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_1:
		bus_width = CFG_BUS_WIDTH_1;
		break;
	case MMC_BUS_WIDTH_4:
		bus_width = CFG_BUS_WIDTH_4;
		break;
	case MMC_BUS_WIDTH_8:
		bus_width = CFG_BUS_WIDTH_8;
		break;
	default:
		dev_err(host->dev, "Invalid ios->bus_width: %u.  Setting to 4.\n",
			ios->bus_width);
		bus_width = CFG_BUS_WIDTH_4;
	}
	pdata->bus_width = ios->bus_width;

	val = readl(host->regs + SD_EMMC_CFG);
	val &= ~CFG_BUS_WIDTH_MASK;
	val |= FIELD_PREP(CFG_BUS_WIDTH_MASK, bus_width);
	writel(val, host->regs + SD_EMMC_CFG);

	meson_mmc_check_resampling(mmc, ios);
	err = meson_mmc_prepare_ios_clock(mmc, ios);
	if (err)
		dev_err(host->dev, "Failed to set clock: %d\n,", err);

	pdata->clkc = readl(host->regs + SD_EMMC_CLOCK);
	pdata->ctrl = readl(host->regs + SD_EMMC_CFG);
	dev_dbg(host->dev, "SD_EMMC_CFG:  0x%08x, %s\n", val, mmc_hostname(mmc));
	dev_dbg(host->dev, "SD_EMMC_clock:  0x%08x, %s\n", readl(host->regs), mmc_hostname(mmc));

	if (host->tdma)
		complete(&host->drv_completion);
}

static void aml_sd_emmc_check_sdio_irq(struct mmc_host *mmc)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 vstat = readl(host->regs + SD_EMMC_STATUS);

	if (host->sdio_irqen) {
		if (((vstat & IRQ_SDIO) || (!(vstat & (1 << 17)))) &&
		    host->mmc->sdio_irq_thread &&
		    (!atomic_read(&host->mmc->sdio_irq_thread_abort))) {
			/* pr_info("signalling irq 0x%x\n", vstat); */
			mmc_signal_sdio_irq(host->mmc);
		}
	}
}

void aml_config_pinmux(struct mmc_host *mmc)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;

	pinctrl_select_state(host->pinctrl, pdata->pins_default);
}

int aml_config_mmc_clk(struct mmc_host *mmc)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	unsigned int clk;
	int ret = 0;

	clk = clk_get_rate(host->mmc_clk);
	pr_debug("[%s][%d] clk:%u\n", __func__, __LINE__, clk);
	if (clk == mmc->actual_clock) {
		pr_debug("[%s][%d] return config clk\n", __func__, __LINE__);
		return ret;
	}
	ret = clk_set_parent(host->mux[0], pdata->src_clk);
	if (ret) {
		dev_err(host->dev, "[%s]set parent error\n", __func__);
		return ret;
	}

	ret = clk_set_rate(host->mmc_clk, pdata->req_rate);
	if (ret) {
		dev_err(host->dev, "Unable to set cfg_div_clk to %lu. ret=%d\n",
			pdata->req_rate, ret);
		return ret;
	}
	return ret;
}

int aml_save_parameter(struct mmc_host *mmc)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 adj, dly1, dly2, intf3, clk, conf;

	if (aml_card_type_mmc(pdata))
		return 0;
	aml_config_pinmux(mmc);
	clk = readl(host->regs + SD_EMMC_CLOCK);
	conf = readl(host->regs + SD_EMMC_CFG);
	adj = readl(host->regs + SD_EMMC_V3_ADJUST);
	dly1 = readl(host->regs + SD_EMMC_DELAY1);
	dly2 = readl(host->regs + SD_EMMC_DELAY2);
	intf3 = readl(host->regs + SD_EMMC_INTF3);

	if (clk == pdata->clkc &&
		conf == pdata->ctrl &&
		adj == pdata->adj &&
		dly1 == pdata->dly1 &&
		dly2 == pdata->dly2 &&
		intf3 == pdata->intf3) {
		pr_debug("[%s][%d] %s\n",
			__func__, __LINE__, mmc_hostname(mmc));
		return 0;
	}

	pr_debug("[%s][%d] c:0x%x, r-c:0x%x, ctrl:0x%x, r-ctrl:0x%x, %s\n",
		__func__, __LINE__, pdata->clkc, clk, pdata->ctrl, conf, mmc_hostname(mmc));
	writel(pdata->clkc, host->regs + SD_EMMC_CLOCK);
	writel(pdata->ctrl, host->regs + SD_EMMC_CFG);
	writel(pdata->adj, host->regs + SD_EMMC_V3_ADJUST);
	writel(pdata->dly1, host->regs + SD_EMMC_DELAY1);
	writel(pdata->dly2, host->regs + SD_EMMC_DELAY2);
	writel(pdata->intf3, host->regs + SD_EMMC_INTF3);
	aml_config_mmc_clk(mmc);
	return 0;
}

static void meson_mmc_request_done(struct mmc_host *mmc,
				   struct mmc_request *mrq)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;

	pr_debug("d %s op:%u, arg:0x%x\n",
		mmc_hostname(mmc),
		mrq->cmd->opcode, mrq->cmd->arg);
	host->cmd = NULL;
	if (host->needs_pre_post_req)
		meson_mmc_post_req(mmc, mrq, 0);
	aml_sd_emmc_check_sdio_irq(mmc);
	mmc_request_done(host->mmc, mrq);
	if (host->tdma && pdata->is_tuning == 0)
		complete(&host->drv_completion);
}

static void meson_mmc_set_blksz(struct mmc_host *mmc, unsigned int blksz)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 cfg, blksz_old;

	cfg = readl(host->regs + SD_EMMC_CFG);
	blksz_old = FIELD_GET(CFG_BLK_LEN_MASK, cfg);

	if (!is_power_of_2(blksz))
		dev_err(host->dev, "blksz %u is not a power of 2\n", blksz);

	blksz = ilog2(blksz);

	/* check if block-size matches, if not update */
	if (blksz == blksz_old)
		return;

	dev_dbg(host->dev, "%s: update blk_len %d -> %d\n", __func__,
		blksz_old, blksz);

	cfg &= ~CFG_BLK_LEN_MASK;
	cfg |= FIELD_PREP(CFG_BLK_LEN_MASK, blksz);
	writel(cfg, host->regs + SD_EMMC_CFG);
	pdata->ctrl = cfg;
}

static void meson_mmc_set_response_bits(struct mmc_command *cmd, u32 *cmd_cfg)
{
	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			*cmd_cfg |= CMD_CFG_RESP_128;
		*cmd_cfg |= CMD_CFG_RESP_NUM;

		if (!(cmd->flags & MMC_RSP_CRC))
			*cmd_cfg |= CMD_CFG_RESP_NOCRC;

		if (cmd->flags & MMC_RSP_BUSY)
			*cmd_cfg |= CMD_CFG_R1B;
	} else {
		*cmd_cfg |= CMD_CFG_NO_RESP;
	}
}

static void meson_mmc_desc_chain_transfer(struct mmc_host *mmc, u32 cmd_cfg,
					  struct mmc_command *cmd)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	struct sd_emmc_desc *desc = host->descs;
	struct mmc_data *data = host->cmd->data;
	struct scatterlist *sg;
	u32 start;
	int i, j = 0;

	if (data->flags & MMC_DATA_WRITE)
		cmd_cfg |= CMD_CFG_DATA_WR;

	if (data->blocks > 1) {
		cmd_cfg |= CMD_CFG_BLOCK_MODE;
		meson_mmc_set_blksz(mmc, data->blksz);
	}

	if (mmc_op_multi(cmd->opcode) && cmd->mrq->sbc) {
		desc[j].cmd_cfg = 0;
		desc[j].cmd_cfg |= FIELD_PREP(CMD_CFG_CMD_INDEX_MASK,
					      MMC_SET_BLOCK_COUNT);
		desc[j].cmd_cfg |= FIELD_PREP(CMD_CFG_TIMEOUT_MASK, 0xc);
		desc[j].cmd_cfg |= CMD_CFG_OWNER;
		desc[j].cmd_cfg |= CMD_CFG_RESP_NUM;
		desc[j].cmd_arg = cmd->mrq->sbc->arg;
		desc[j].cmd_resp = 0;
		desc[j].cmd_data = 0;
		j++;
	}

	for_each_sg(data->sg, sg, data->sg_count, i) {
		unsigned int len = sg_dma_len(sg);

		if (data->blocks > 1)
			len /= data->blksz;

		desc[i + j].cmd_cfg = cmd_cfg;
		desc[i + j].cmd_cfg |= FIELD_PREP(CMD_CFG_LENGTH_MASK, len);
		if (i > 0)
			desc[i + j].cmd_cfg |= CMD_CFG_NO_CMD;
		desc[i + j].cmd_arg = host->cmd->arg;
		desc[i + j].cmd_resp = 0;
		desc[i + j].cmd_data = sg_dma_address(sg);
	}

	if (mmc_op_multi(cmd->opcode) && !cmd->mrq->sbc) {
		desc[data->sg_count].cmd_cfg = 0;
		desc[data->sg_count].cmd_cfg |=
			FIELD_PREP(CMD_CFG_CMD_INDEX_MASK,
				   MMC_STOP_TRANSMISSION);
		desc[data->sg_count].cmd_cfg |=
			FIELD_PREP(CMD_CFG_TIMEOUT_MASK, 0xc);
		desc[data->sg_count].cmd_cfg |= CMD_CFG_OWNER;
		desc[data->sg_count].cmd_cfg |= CMD_CFG_RESP_NUM;
		desc[data->sg_count].cmd_cfg |= CMD_CFG_R1B;
		desc[data->sg_count].cmd_resp = 0;
		desc[data->sg_count].cmd_data = 0;
		j++;
	}

	desc[data->sg_count + j - 1].cmd_cfg |= CMD_CFG_END_OF_CHAIN;
	dma_wmb(); /* ensure descriptor is written before kicked */
	start = host->descs_dma_addr | START_DESC_BUSY;
	writel(start, host->regs + SD_EMMC_START);
}

static void meson_mmc_quirk_transfer(struct mmc_host *mmc, u32 cmd_cfg,
					  struct mmc_command *cmd)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	struct sd_emmc_desc *desc = host->descs;
	struct mmc_data *data = host->cmd->data;
	u32 start, data_len;

	if (data->blocks > 1) {
		cmd_cfg |= CMD_CFG_BLOCK_MODE;
		meson_mmc_set_blksz(mmc, data->blksz);
		data_len = data->blocks;
	} else {
		data_len = data->blksz;
	}

	if (mmc_op_multi(cmd->opcode) && cmd->mrq->sbc) {
		desc->cmd_cfg = 0;
		desc->cmd_cfg |= FIELD_PREP(CMD_CFG_CMD_INDEX_MASK,
					      MMC_SET_BLOCK_COUNT);
		desc->cmd_cfg |= FIELD_PREP(CMD_CFG_TIMEOUT_MASK, 0xc);
		desc->cmd_cfg |= CMD_CFG_OWNER;
		desc->cmd_cfg |= CMD_CFG_RESP_NUM;
		desc->cmd_arg = cmd->mrq->sbc->arg;
		desc->cmd_resp = 0;
		desc->cmd_data = 0;
		desc++;
	}

	desc->cmd_cfg = cmd_cfg;
	desc->cmd_cfg |= FIELD_PREP(CMD_CFG_LENGTH_MASK, data_len);
	desc->cmd_arg = host->cmd->arg;
	desc->cmd_resp = 0;
	desc->cmd_data = host->bounce_dma_addr;

	if (mmc_op_multi(cmd->opcode) && !cmd->mrq->sbc) {
		desc++;
		desc->cmd_cfg = 0;
		desc->cmd_cfg |= FIELD_PREP(CMD_CFG_CMD_INDEX_MASK,
				   MMC_STOP_TRANSMISSION);
		desc->cmd_cfg |= FIELD_PREP(CMD_CFG_TIMEOUT_MASK, 0xc);
		desc->cmd_cfg |= CMD_CFG_OWNER;
		desc->cmd_cfg |= CMD_CFG_RESP_NUM;
		desc->cmd_cfg |= CMD_CFG_R1B;
		desc->cmd_resp = 0;
		desc->cmd_data = 0;
	}

	desc->cmd_cfg |= CMD_CFG_END_OF_CHAIN;

	dma_wmb(); /* ensure descriptor is written before kicked */
	start = host->descs_dma_addr | START_DESC_BUSY | CMD_DATA_SRAM;
	writel(start, host->regs + SD_EMMC_START);
}

static void meson_mmc_start_cmd(struct mmc_host *mmc, struct mmc_command *cmd)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	struct mmc_data *data = cmd->data;
	u32 val, cmd_cfg = 0, cmd_data = 0;
	unsigned int xfer_bytes = 0;

	/* Setup descriptors */
	dma_rmb();

	host->cmd = cmd;

	cmd_cfg |= FIELD_PREP(CMD_CFG_CMD_INDEX_MASK, cmd->opcode);
	cmd_cfg |= CMD_CFG_OWNER;  /* owned by CPU */

	meson_mmc_set_response_bits(cmd, &cmd_cfg);

	if (cmd->opcode == SD_SWITCH_VOLTAGE) {
		val = readl(host->regs + SD_EMMC_CFG);
		val &= ~CFG_AUTO_CLK;
		writel(val, host->regs + SD_EMMC_CFG);
	}

	/* data? */
	if (data) {
		data->bytes_xfered = 0;
		cmd_cfg |= CMD_CFG_DATA_IO;
		cmd_cfg |= FIELD_PREP(CMD_CFG_TIMEOUT_MASK,
				      ilog2(meson_mmc_get_timeout_msecs(data)));

		if (meson_mmc_desc_chain_mode(data)) {
			meson_mmc_desc_chain_transfer(mmc, cmd_cfg, cmd);
			return;
		}

		if (data->blocks > 1) {
			cmd_cfg |= CMD_CFG_BLOCK_MODE;
			cmd_cfg |= FIELD_PREP(CMD_CFG_LENGTH_MASK,
					      data->blocks);
			meson_mmc_set_blksz(mmc, data->blksz);
		} else {
			cmd_cfg |= FIELD_PREP(CMD_CFG_LENGTH_MASK, data->blksz);
		}

		xfer_bytes = data->blksz * data->blocks;
		if (data->flags & MMC_DATA_WRITE) {
			cmd_cfg |= CMD_CFG_DATA_WR;
			WARN_ON(xfer_bytes > host->bounce_buf_size);
			sg_copy_to_buffer(data->sg, data->sg_len,
					  host->bounce_buf, xfer_bytes);
			dma_wmb();
		}

		cmd_data = host->bounce_dma_addr & CMD_DATA_MASK;

		if (host->dram_access_quirk) {
			meson_mmc_quirk_transfer(mmc, cmd_cfg, cmd);
			return;
		}
	} else {
		cmd_cfg |= FIELD_PREP(CMD_CFG_TIMEOUT_MASK,
				      ilog2(SD_EMMC_CMD_TIMEOUT));
	}

	/* Last descriptor */
	cmd_cfg |= CMD_CFG_END_OF_CHAIN;
	writel(cmd_cfg, host->regs + SD_EMMC_CMD_CFG);
	writel(cmd_data, host->regs + SD_EMMC_CMD_DAT);
	writel(0, host->regs + SD_EMMC_CMD_RSP);
	wmb(); /* ensure descriptor is written before kicked */
	writel(cmd->arg, host->regs + SD_EMMC_CMD_ARG);
}

static void meson_mmc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	unsigned long flags;

	if (host->tdma &&
		mrq->cmd->opcode != MMC_SEND_TUNING_BLOCK) {
		wait_for_completion(&host->drv_completion);
		pr_debug("[%s][%d] %s opcode:%u, arg:0x%x\n",
			__func__, __LINE__, mmc_hostname(mmc),
			mrq->cmd->opcode, mrq->cmd->arg);
		aml_save_parameter(mmc);
	}

	spin_lock_irqsave(&host->lock, flags);
	host->mmc = mmc;
	host->needs_pre_post_req = mrq->data &&
			!(mrq->data->host_cookie & SD_EMMC_PRE_REQ_DONE);

	if (host->needs_pre_post_req) {
		meson_mmc_get_transfer_mode(mmc, mrq);
		if (!meson_mmc_desc_chain_mode(mrq->data))
			host->needs_pre_post_req = false;
	}

	if (host->needs_pre_post_req)
		meson_mmc_pre_req(mmc, mrq);
	/* Stop execution */
	writel(0, host->regs + SD_EMMC_START);

	meson_mmc_start_cmd(mmc, mrq->cmd);
	spin_unlock_irqrestore(&host->lock, flags);
}

static void aml_sd_emmc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 vclkc = 0, virqc = 0;
	unsigned long flags;

	host->sdio_irqen = enable;
	spin_lock_irqsave(&host->lock, flags);
	virqc = readl(host->regs + SD_EMMC_IRQ_EN);
	virqc &= ~IRQ_SDIO;
	if (enable)
		virqc |= IRQ_SDIO;
	writel(virqc, host->regs + SD_EMMC_IRQ_EN);

	if (!pdata->irq_sdio_sleep) {
		vclkc = readl(host->regs + SD_EMMC_CLOCK);
		vclkc |= CFG_IRQ_SDIO_SLEEP;
		vclkc &= ~CFG_IRQ_SDIO_SLEEP_DS;
		writel(vclkc, host->regs + SD_EMMC_CLOCK);
		pdata->clkc = vclkc;
		pdata->irq_sdio_sleep = 1;
	}
	spin_unlock_irqrestore(&host->lock, flags);

	/* check if irq already occurred */
	aml_sd_emmc_check_sdio_irq(mmc);
}

static void meson_mmc_read_resp(struct mmc_host *mmc, struct mmc_command *cmd)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;

	if (cmd->flags & MMC_RSP_136) {
		cmd->resp[0] = readl(host->regs + SD_EMMC_CMD_RSP3);
		cmd->resp[1] = readl(host->regs + SD_EMMC_CMD_RSP2);
		cmd->resp[2] = readl(host->regs + SD_EMMC_CMD_RSP1);
		cmd->resp[3] = readl(host->regs + SD_EMMC_CMD_RSP);
	} else if (cmd->flags & MMC_RSP_PRESENT) {
		cmd->resp[0] = readl(host->regs + SD_EMMC_CMD_RSP);
	}
}

static irqreturn_t meson_mmc_irq(int irq, void *dev_id)
{
	struct meson_host *host = dev_id;
	struct mmc_host *mmc = host->mmc;
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct mmc_command *cmd;
	struct mmc_data *data;
	u32 irq_en, status, raw_status;
	irqreturn_t ret = IRQ_NONE;

	if (WARN_ON(!host))
		return IRQ_NONE;

	if (!host->cmd && (aml_card_type_mmc(pdata) ||
			aml_card_type_non_sdio(pdata))) {
		pr_debug("ignore irq.[%s]status:0x%x\n",
			__func__, readl(host->regs + SD_EMMC_STATUS));
		return IRQ_HANDLED;
	}

	irq_en = readl(host->regs + SD_EMMC_IRQ_EN);
	raw_status = readl(host->regs + SD_EMMC_STATUS);
	status = raw_status & irq_en;

	if (status & IRQ_SDIO) {
		if (sdio_host && host->tdma) {
			if (sdio_host->sdio_irq_thread &&
				(!atomic_read(&sdio_host->sdio_irq_thread_abort))) {
				mmc_signal_sdio_irq(sdio_host);
				if (!(status & 0x3fff))
					return IRQ_HANDLED;
			}
		} else {
			if (host->mmc->sdio_irq_thread &&
				(!atomic_read(&host->mmc->sdio_irq_thread_abort))) {
				mmc_signal_sdio_irq(host->mmc);
				if (!(status & 0x3fff))
					return IRQ_HANDLED;
			}
		}
	} else if (!(status & 0x3fff)) {
		return IRQ_HANDLED;
	}

	cmd = host->cmd;
	data = cmd->data;
	if (WARN_ON(!host->cmd)) {
		dev_err(host->dev, "host->cmd is NULL.\n");
		return IRQ_HANDLED;
	}

	cmd->error = 0;
	if (status & IRQ_CRC_ERR) {
		if (!pdata->is_tuning)
			dev_err(host->dev, "%d [0x%x], CRC[0x%04x]\n",
				cmd->opcode, cmd->arg, status);
		if (pdata->debug_flag && !pdata->is_tuning) {
			dev_notice(host->dev, "clktree : 0x%x,host_clock: 0x%x\n",
				   readl(host->clk_tree_base),
				   readl(host->regs));
			dev_notice(host->dev, "adjust: 0x%x,cfg: 0x%x,intf3: 0x%x\n",
				   readl(host->regs + SD_EMMC_V3_ADJUST),
				   readl(host->regs + SD_EMMC_CFG),
				   readl(host->regs + SD_EMMC_INTF3));
			dev_notice(host->dev, "irq_en: 0x%x\n",
				   readl(host->regs + 0x4c));
			dev_notice(host->dev, "delay1: 0x%x,delay2: 0x%x\n",
				   readl(host->regs + SD_EMMC_DELAY1),
				   readl(host->regs + SD_EMMC_DELAY2));
			dev_notice(host->dev, "pinmux: 0x%x\n",
				   readl(host->pin_mux_base));
		}
		cmd->error = -EILSEQ;
		ret = IRQ_WAKE_THREAD;
		goto out;
	}

	if (status & IRQ_TIMEOUTS) {
		if (!pdata->is_tuning && !(cmd->arg == 0xc00 || cmd->arg == 0x80000c08))
			dev_err(host->dev, "%d [0x%x], TIMEOUT[0x%04x]\n",
				cmd->opcode, cmd->arg, status);
		if (pdata->debug_flag && !pdata->is_tuning) {
			dev_notice(host->dev, "clktree : 0x%x,host_clock: 0x%x\n",
				   readl(host->clk_tree_base),
				   readl(host->regs));
			dev_notice(host->dev, "adjust: 0x%x,cfg: 0x%x,intf3: 0x%x\n",
				   readl(host->regs + SD_EMMC_V3_ADJUST),
				   readl(host->regs + SD_EMMC_CFG),
				   readl(host->regs + SD_EMMC_INTF3));
			dev_notice(host->dev, "delay1: 0x%x,delay2: 0x%x\n",
				   readl(host->regs + SD_EMMC_DELAY1),
				   readl(host->regs + SD_EMMC_DELAY2));
			dev_notice(host->dev, "pinmux: 0x%x\n",
				   readl(host->pin_mux_base));
			dev_notice(host->dev, "irq_en: 0x%x\n",
				   readl(host->regs + 0x4c));
		}
		cmd->error = -ETIMEDOUT;
		ret = IRQ_WAKE_THREAD;
		goto out;
	}

	if (status & (IRQ_CRC_ERR | IRQ_TIMEOUTS))
		aml_host_bus_fsm_show(host->mmc, status);

	meson_mmc_read_resp(host->mmc, cmd);

	if (status & IRQ_SDIO) {
		dev_dbg(host->dev, "IRQ: SDIO TODO.\n");
		ret = IRQ_HANDLED;
	}

	if (status & (IRQ_END_OF_CHAIN | IRQ_RESP_STATUS)) {
		if (data && !cmd->error)
			data->bytes_xfered = data->blksz * data->blocks;
		if (meson_mmc_bounce_buf_read(data))
			ret = IRQ_WAKE_THREAD;
		else
			ret = IRQ_HANDLED;
	}

out:
	/* ack all raised interrupts */
	writel(0x7fff, host->regs + SD_EMMC_STATUS);
	if (cmd->error) {
		/* Stop desc in case of errors */
		u32 start = readl(host->regs + SD_EMMC_START);

		if (!pdata->ignore_desc_busy && (start & START_DESC_BUSY)) {
			start &= ~START_DESC_BUSY;
			writel(start, host->regs + SD_EMMC_START);
		}
	}

	if (ret == IRQ_HANDLED) {
		meson_mmc_read_resp(host->mmc, cmd);
		if (cmd->error && !pdata->is_tuning)
			pr_err("cmd = %d, arg = 0x%x, dev_status = 0x%x\n",
					cmd->opcode, cmd->arg, cmd->resp[0]);
		meson_mmc_request_done(host->mmc, cmd->mrq);
	} else if (ret == IRQ_NONE) {
		dev_warn(host->dev,
				"Unexpected IRQ! status=0x%08x, irq_en=0x%08x\n",
				raw_status, irq_en);
	}

	return ret;
}

static int meson_mmc_wait_desc_stop(struct meson_host *host)
{
	u32 status;

	/*
	 * It may sometimes take a while for it to actually halt. Here, we
	 * are giving it 5ms to comply
	 *
	 * If we don't confirm the descriptor is stopped, it might raise new
	 * IRQs after we have called mmc_request_done() which is bad.
	 */

	return readl_poll_timeout(host->regs + SD_EMMC_STATUS, status,
				  !(status & (STATUS_BUSY | STATUS_DESC_BUSY)),
				  100, 10000);
}

static irqreturn_t meson_mmc_irq_thread(int irq, void *dev_id)
{
	struct meson_host *host = dev_id;
	struct mmc_command *cmd = host->cmd;
	struct mmc_data *data;
	unsigned int xfer_bytes;

	if (WARN_ON(!cmd))
		return IRQ_NONE;

	if (cmd->error) {
		meson_mmc_wait_desc_stop(host);
		meson_mmc_request_done(host->mmc, cmd->mrq);

		return IRQ_HANDLED;
	}

	data = cmd->data;
	if (meson_mmc_bounce_buf_read(data)) {
		xfer_bytes = data->blksz * data->blocks;
		WARN_ON(xfer_bytes > host->bounce_buf_size);
		sg_copy_from_buffer(data->sg, data->sg_len,
				    host->bounce_buf, xfer_bytes);
	}

	meson_mmc_read_resp(host->mmc, cmd);
	meson_mmc_request_done(host->mmc, cmd->mrq);

	return IRQ_HANDLED;
}

/*
 * NOTE: we only need this until the GPIO/pinctrl driver can handle
 * interrupts.  For now, the MMC core will use this for polling.
 */
static int meson_mmc_get_cd(struct mmc_host *mmc)
{
	int status = mmc_gpio_get_cd(mmc);

	if (status == -EINVAL)
		return 1; /* assume present */

	return !status;
}

static void meson_mmc_cfg_init(struct meson_host *host)
{
	u32 cfg = 0;

	cfg |= FIELD_PREP(CFG_RESP_TIMEOUT_MASK,
			  ilog2(SD_EMMC_CFG_RESP_TIMEOUT));
	cfg |= FIELD_PREP(CFG_RC_CC_MASK, ilog2(SD_EMMC_CFG_CMD_GAP));
	cfg |= FIELD_PREP(CFG_BLK_LEN_MASK, ilog2(SD_EMMC_CFG_BLK_SIZE));

	/* abort chain on R/W errors */

	writel(cfg, host->regs + SD_EMMC_CFG);

	host->timing = -1;
}

static int meson_mmc_card_busy(struct mmc_host *mmc)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	u32 regval, val;
	int ret = 0;

	if (host->tdma)
		wait_for_completion(&host->drv_completion);

	regval = readl(host->regs + SD_EMMC_STATUS);
	if (!aml_card_type_mmc(pdata) && host->sd_sdio_switch_volat_done) {
		val = readl(host->regs + SD_EMMC_CFG);
		val |= CFG_AUTO_CLK;
		writel(val, host->regs + SD_EMMC_CFG);
		host->sd_sdio_switch_volat_done = 0;
		pdata->ctrl = val;
	}

	/* We are only interrested in lines 0 to 3, so mask the other ones */
	if (!aml_card_type_mmc(pdata))
		ret = !(FIELD_GET(STATUS_DATI, regval) & 0xf);
	else
		ret = !(FIELD_GET(STATUS_DATI, regval) & 0x1);

	if (host->tdma)
		complete(&host->drv_completion);

	return ret;
}

static int meson_mmc_voltage_switch(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	int err;

	if (IS_ERR(mmc->supply.vqmmc) && IS_ERR(mmc->supply.vmmc))
		return 0;
	/* vqmmc regulator is available */
	if (!IS_ERR(mmc->supply.vqmmc)) {
		/*
		 * The usual amlogic setup uses a GPIO to switch from one
		 * regulator to the other. While the voltage ramp up is
		 * pretty fast, care must be taken when switching from 3.3v
		 * to 1.8v. Please make sure the regulator framework is aware
		 * of your own regulator constraints
		 */
		err = mmc_regulator_set_vqmmc(mmc, ios);

		if (!err && ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180)
			host->sd_sdio_switch_volat_done = 1;

		return err;
	}

	/* no vqmmc regulator, assume fixed regulator at 3/3.3V */
	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_330)
		return 0;

	return -EINVAL;
}

static int meson_mmc_execute_tuning(struct mmc_host *mmc, u32 opcode)
{
	struct amlsd_platform *pdata = mmc_priv(mmc);
	struct meson_host *host = pdata->host;
	int err = 0;

	if (host->tdma)
		wait_for_completion(&host->drv_completion);
	pdata->is_tuning = 1;
	err = meson_mmc_fixadj_tuning(mmc, opcode);
	pdata->is_tuning = 0;
	if (host->tdma)
		complete(&host->drv_completion);

	return err;
}

//static void sdio_rescan(struct mmc_host *mmc)
//{
//	int ret;
//
//	mmc->rescan_entered = 0;
//	//mmc->host_rescan_disable = false;
//	mmc_detect_change(mmc, 0);
//	// start the delayed_work
//	ret = flush_work(&mmc->detect.work);
//	// wait for the delayed_work to finish
//	if (!ret)
//		pr_info("Error: delayed_work mmc_rescan() already idle!\n");
//}

//static void sdio_reset_comm(struct mmc_card *card)
//{
//	struct mmc_host *host = card->host;
//	int i = 0, err = 0;
//
//	while (!card->sdio_func[i] && i < SDIO_MAX_FUNCS)
//		i++;
//	if (WARN_ON(i == SDIO_MAX_FUNCS))
//		return;
//	sdio_claim_host(card->sdio_func[i]);
//	err = mmc_sw_reset(host);
//	sdio_release_host(card->sdio_func[i]);
//	if (err)
//		pr_info("%s Failed, error = %d\n", __func__, err);
//	return;
//}
//
//void sdio_reinit(void)
//{
//	if (sdio_host) {
//		struct mmc_ios *ios = &sdio_host->ios;
//
//		if (sdio_host->card) {
//			if (ios)
//				ios->timing = 0;
//			sdio_reset_comm(sdio_host->card);
//		}
//		else
//			sdio_rescan(sdio_host);
//	} else {
//		pr_info("Error: sdio_host is NULL\n");
//	}
//
//	pr_debug("[%s] finish\n", __func__);
//}
//EXPORT_SYMBOL(sdio_reinit);
//
//void sdio_clk_always_on(bool clk_aws_on)
//{
//	struct meson_host *host = NULL;
//	struct amlsd_platform *pdata = NULL;
//	u32 conf = 0;
//
//	if (sdio_host) {
//		pdata = mmc_priv(sdio_host);
//		host = pdata->host;
//		conf = readl(host->regs + SD_EMMC_CFG);
//		if (clk_aws_on)
//			conf &= ~CFG_AUTO_CLK;
//		else
//			conf |= CFG_AUTO_CLK;
//		writel(conf, host->regs + SD_EMMC_CFG);
//		pr_info("[%s] clk:0x%x, cfg:0x%x\n",
//				__func__, readl(host->regs + SD_EMMC_CLOCK),
//				readl(host->regs + SD_EMMC_CFG));
//	} else {
//		pr_info("Error: sdio_host is NULL\n");
//	}
//
//	pr_info("[%s] finish\n", __func__);
//}
//EXPORT_SYMBOL(sdio_clk_always_on);
//
//void sdio_set_max_regs(unsigned int size)
//{
//	if (sdio_host) {
//		sdio_host->max_req_size = size;
//		sdio_host->max_seg_size = sdio_host->max_req_size;
//	} else {
//		pr_info("Error: sdio_host is NULL\n");
//	}
//
//	pr_info("[%s] finish\n", __func__);
//}
//EXPORT_SYMBOL(sdio_set_max_regs);
//
///*this function tells wifi is using sd(sdiob) or sdio(sdioa)*/
//const char *get_wifi_inf(void)
//{
//	if (sdio_host)
//		return mmc_hostname(sdio_host);
//	else
//		return "sdio";
//}
//EXPORT_SYMBOL(get_wifi_inf);
//
//int sdio_get_vendor(void)
//{
//	int vendor = 0;
//
//	if (sdio_host && sdio_host->card)
//		vendor = sdio_host->card->cis.vendor;
//
//	pr_info("sdio vendor is 0x%x\n", vendor);
//	return vendor;
//}
//EXPORT_SYMBOL(sdio_get_vendor);

//static struct pinctrl * __must_check aml_pinctrl_select(struct mmc_host *mmc,
//							const char *name)
//{
//	struct amlsd_platform *pdata = mmc_priv(mmc);
//	struct meson_host *host = pdata->host;
//	struct pinctrl *p = host->pinctrl;
//	struct pinctrl_state *s;
//	int ret = 1;
//
//	if (!p) {
//		dev_err(host->dev, "%s NULL POINT!!\n", __func__);
//		return ERR_PTR(ret);
//	}
//
//	s = pinctrl_lookup_state(p, name);
//	if (IS_ERR(s)) {
//		pr_err("lookup %s fail\n", name);
//		devm_pinctrl_put(p);
//		return ERR_CAST(s);
//	}
//
//	ret = pinctrl_select_state(p, s);
//	if (ret < 0) {
//		pr_err("select %s fail\n", name);
//		devm_pinctrl_put(p);
//		return ERR_PTR(ret);
//	}
//	return p;
//}
//
//static int aml_uart_switch(struct mmc_host *mmc, bool on)
//{
//	//struct amlsd_platform *pdata = mmc_priv(mmc);
//	//struct meson_host *host = pdata->host;
//	struct pinctrl *pc;
//	char *name[2] = {
//		"sd_to_ao_uart_pins",
//		"ao_to_sd_uart_pins",
//	};
//
//	pc = aml_pinctrl_select(mmc, name[on]);
//	return on;
//}
//
//static int aml_is_sduart(struct mmc_host *mmc)
//{
//	struct amlsd_platform *pdata = mmc_priv(mmc);
//	struct meson_host *host = pdata->host;
//	int in = 0, i;
//	int high_cnt = 0, low_cnt = 0;
//	u32 vstat = 0;
//
//	if (pdata->is_uart)
//		return 0;
//	//if (!pdata->sd_uart_init) {
//	//	aml_uart_switch(mmc, 0);
//	//} else {
//	//	in = (readl(host->pin_mux_base) & DATA3_PINMUX_MASK) >>
//	//		__ffs(DATA3_PINMUX_MASK);
//	//	if (in == 2)
//	//		return 1;
//	//	else
//	//		return 0;
//	//}
//	for (i = 0; ; i++) {
//		mdelay(1);
//		vstat = readl(host->regs + SD_EMMC_STATUS) & 0xffffffff;
//		if (vstat & 0x80000) {
//			high_cnt++;
//			low_cnt = 0;
//		} else {
//			low_cnt++;
//			high_cnt = 0;
//		}
//		if (high_cnt > 100 || low_cnt > 100)
//			break;
//	}
//	if (low_cnt > 100)
//		in = 1;
//	return in;
//}

//static int aml_is_card_insert(struct mmc_gpio *ctx)
//{
//	int ret = 0, in_count = 0, out_count = 0, i;
//
//	if (ctx->cd_gpio) {
//		for (i = 0; i < 200; i++) {
//			ret = gpiod_get_value(ctx->cd_gpio);
//			if (ret)
//				out_count++;
//			in_count++;
//			if (out_count > 100 || in_count > 100)
//				break;
//		}
//		if (out_count > 100)
//			ret = 1;
//		else if (in_count > 100)
//			ret = 0;
//	}
////        if (ctx->override_cd_active_level)
//  //              ret = !ret; /* reverse, so ---- 0: no inserted  1: inserted */
//
//	return ret;
//}

//int meson_mmc_cd_detect(struct mmc_host *mmc)
//{
//	int gpio_val, val, ret = 0;
//	struct amlsd_platform *pdata = mmc_priv(mmc);
//	struct meson_host *host = pdata->host;
//	struct mmc_gpio *ctx = mmc->slot.handler_priv;
//
//	if (host->tdma)
//		wait_for_completion(&host->drv_completion);
//	gpio_val = aml_is_card_insert(ctx);
//	dev_notice(host->dev, "card %s\n", gpio_val ? "OUT" : "IN");
//	mmc->trigger_card_event = true;
//	if (!gpio_val) {//card insert
//		if (pdata->card_insert)
//			goto out;
//		pdata->card_insert = 1;
//		val = aml_is_sduart(mmc);
//		dev_notice(host->dev, " %s insert\n", val ? "UART" : "SDCARD");
//		if (val) {//uart insert
//			pdata->is_uart = 1;
//			aml_uart_switch(mmc, 1);
//			mmc->caps &= ~MMC_CAP_4_BIT_DATA;
//			pdata->pins_default = pinctrl_lookup_state(host->pinctrl,
//								  "sd_1bit_pins");
//			if (IS_ERR(pdata->pins_default)) {
//				ret = PTR_ERR(pdata->pins_default);
//				goto out;
//			}
//		} else {//sdcard insert
//			aml_uart_switch(mmc, 0);
//			mmc->caps |= MMC_CAP_4_BIT_DATA;
//			pdata->pins_default = pinctrl_lookup_state(host->pinctrl,
//								  "sd_default");
//		}
//	} else { //card out
//		if (!pdata->card_insert)
//			goto out;
//		if (pdata->is_uart) {
//			pdata->is_uart = 0;
//			devm_free_irq(mmc->parent, host->cd_irq, mmc);
//		}
//		pdata->card_insert = 0;
//		aml_uart_switch(mmc, 0);
//	}
//	if (!pdata->is_uart)
//		mmc_detect_change(mmc, msecs_to_jiffies(200));
//
//out:
//	if (host->tdma)
//		complete(&host->drv_completion);
//	return ret;
//}

static const struct mmc_host_ops meson_mmc_ops = {
	.request	= meson_mmc_request,
	.set_ios	= meson_mmc_set_ios,
	.enable_sdio_irq = aml_sd_emmc_enable_sdio_irq,
	.get_cd         = meson_mmc_get_cd,
	.pre_req	= meson_mmc_pre_req,
	.post_req	= meson_mmc_post_req,
	.execute_tuning = meson_mmc_execute_tuning,
//	.hs400_complete = aml_post_hs400_timming,
	.card_busy	= meson_mmc_card_busy,
	.start_signal_voltage_switch = meson_mmc_voltage_switch,
//	.init_card      = sdio_get_card,
};

static void add_dtbkey(struct work_struct *work)
{
	int ret;
	struct meson_host *host =
		container_of(work, struct meson_host, dtbkey.work);
	struct mmc_host *mmc = host->mmc;

	if (mmc->card) {
		emmc_key_init(mmc->card, &ret);
		if (ret)
			pr_err("%s:%d,emmc_key_init fail\n", __func__, __LINE__);

		amlmmc_dtb_init(mmc->card, &ret);
		if (ret)
			pr_err("%s:%d,amlmmc_dtb_init fail\n", __func__, __LINE__);
	} else {
		schedule_delayed_work(&host->dtbkey, 50);
	}
}

static int g12a_mmc_probe(struct platform_device *pdev)
{
	struct meson_host *host = NULL;
	struct mmc_host *mmc = NULL;
	struct amlsd_platform *pdata = NULL;
	int ret;
	u32 i, dev_index = 0, dev_num = 0;

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENODEV;
	/* The G12A SDIO Controller needs an SRAM bounce buffer */
	host->dram_access_quirk = device_property_read_bool(&pdev->dev,
				"amlogic,dram-access-quirk");
	host->tdma = device_property_read_bool(&pdev->dev,
				"time-sharing-mult");
	host->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, host);
	host->data = (struct meson_mmc_data *)
		of_device_get_match_data(&pdev->dev);

	if (!host->data) {
		ret = -EINVAL;
		goto free_host;
	}
	ret = device_reset_optional(&pdev->dev);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "device reset failed: %d\n", ret);

		return ret;
	}

	host->res[0] = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	host->regs = devm_ioremap_resource(&pdev->dev, host->res[0]);
	if (IS_ERR(host->regs)) {
		ret = PTR_ERR(host->regs);
		goto free_host;
	}

	host->res[1] = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	host->clk_tree_base = ioremap(host->res[1]->start, resource_size(host->res[1]));
	if (IS_ERR(host->clk_tree_base)) {
		ret = PTR_ERR(host->clk_tree_base);
		goto free_host;
	}
	//val = readl(host->clk_tree_base);
	//if (aml_card_type_non_sdio(pdata))
	//	val &= EMMC_SDIO_CLOCK_FELD;
	//else
	//	val &= ~EMMC_SDIO_CLOCK_FELD;
	//writel(val, host->clk_tree_base);

	host->res[2] = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	host->pin_mux_base = ioremap(host->res[2]->start, resource_size(host->res[2]));
	if (IS_ERR(host->pin_mux_base)) {
		ret = PTR_ERR(host->pin_mux_base);
		goto free_host;
	}

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq <= 0) {
		ret = -EINVAL;
		goto free_host;
	}

	host->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(host->pinctrl)) {
		ret = PTR_ERR(host->pinctrl);
		goto free_host;
	}

	host->core_clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(host->core_clk)) {
		ret = PTR_ERR(host->core_clk);
		goto free_host;
	}

	ret = clk_prepare_enable(host->core_clk);
	if (ret)
		goto free_host;

	ret = meson_mmc_clk_init(host);
	if (ret)
		goto err_core_clk;

	/* set config to sane default */
	meson_mmc_cfg_init(host);

	/* Stop execution */
	writel(0, host->regs + SD_EMMC_START);

	/* clear, ack and enable interrupts */
	writel(0, host->regs + SD_EMMC_IRQ_EN);
	writel(IRQ_CRC_ERR | IRQ_TIMEOUTS | IRQ_END_OF_CHAIN,
	       host->regs + SD_EMMC_STATUS);
	writel(IRQ_CRC_ERR | IRQ_TIMEOUTS | IRQ_END_OF_CHAIN,
	       host->regs + SD_EMMC_IRQ_EN);

	ret = request_threaded_irq(host->irq, meson_mmc_irq,
				   meson_mmc_irq_thread, IRQF_ONESHOT,
				   dev_name(&pdev->dev), host);
	if (ret)
		goto err_init_clk;

	/*
	 * At the moment, we don't know how to reliably enable HS400.
	 * From the different datasheets, it is not even clear if this mode
	 * is officially supported by any of the SoCs
	 */
	if (host->dram_access_quirk) {
		/*
		 * The MMC Controller embeds 1,5KiB of internal SRAM
		 * that can be used to be used as bounce buffer.
		 * In the case of the G12A SDIO controller, use these
		 * instead of the DDR memory
		 */
		host->bounce_buf_size = SD_EMMC_SRAM_DATA_BUF_LEN;
		host->bounce_buf = host->regs + SD_EMMC_SRAM_DATA_BUF_OFF;
		host->bounce_dma_addr = host->res[0]->start + SD_EMMC_SRAM_DATA_BUF_OFF;

		host->descs = host->regs + SD_EMMC_SRAM_DESC_BUF_OFF;
		host->descs_dma_addr = host->res[0]->start + SD_EMMC_SRAM_DESC_BUF_OFF;

	} else {
		/* data bounce buffer */
		host->bounce_buf_size = SD_EMMC_MAX_REQ_SIZE;
		host->bounce_buf =
			dma_alloc_coherent(host->dev, host->bounce_buf_size,
					   &host->bounce_dma_addr, GFP_KERNEL);
		if (!host->bounce_buf) {
			dev_err(host->dev, "Unable to map allocate DMA bounce buffer.\n");
			ret = -ENOMEM;
			goto err_free_irq;
		}

		host->descs = dma_alloc_coherent(host->dev, SD_EMMC_DESC_BUF_LEN,
			      &host->descs_dma_addr, GFP_KERNEL);
		if (!host->descs) {
			ret = -ENOMEM;
			goto err_bounce_buf;
		}
	}

	host->blk_test = devm_kzalloc(host->dev, 512 * CALI_BLK_CNT, GFP_KERNEL);
	if (!host->blk_test) {
		ret = -ENOMEM;
		goto err_bounce_buf;
	}
	host->adj_win = devm_kzalloc(host->dev, sizeof(u8) * ADJ_WIN_PRINT_MAXLEN, GFP_KERNEL);
	if (!host->adj_win) {
		ret = -ENOMEM;
		goto err_free_irq;
	}

	if (host->tdma) {
		init_completion(&host->drv_completion);
		host->drv_completion.done = 1;
		dev_num = MMC_DEVICES_NUM;
		dev_index = 1;
	}
	for (i = dev_index; i <= dev_num; i++) {
		mmc = mmc_alloc_host(sizeof(struct amlsd_platform), &pdev->dev);
		if (!mmc)
			return -ENOMEM;

		pdata = mmc_priv(mmc);
		memset(pdata, 0, sizeof(struct amlsd_platform));
		pdata->host = host;
		pdata->mmc = mmc;
		pdata->timing = -1;
		pdata->req_rate = -1;
		pdata->ddr = 0;
		pdata->vqmmc_enabled = false;
		pdata->src_clk = NULL;
		pdata->clkc = readl(host->regs + SD_EMMC_CLOCK);
		pdata->ctrl = readl(host->regs + SD_EMMC_CFG);

		/* Get regulators and the supported OCR mask */
		ret = mmc_regulator_get_supply(mmc);
		if (ret) {
			dev_warn(&pdev->dev, "power regulator get failed!\n");
			goto free_host;
		}

		if (host->tdma)
			ret = tdma_of_parse(mmc, i);
		else
			ret = mmc_of_parse(mmc);
		if (ret) {
			if (ret != -EPROBE_DEFER)
				dev_warn(&pdev->dev, "error parsing DT: %d\n", ret);
			goto free_host;
		}
		amlogic_of_parse(mmc);
		mmc->hold_retune = 1;

		if (aml_card_type_non_sdio(pdata)) {
			if (!IS_ERR(mmc->supply.vqmmc))
				regulator_set_voltage_triplet(mmc->supply.vqmmc,
					1700000, 1800000, 1950000);
		}

		if (aml_card_type_non_sdio(pdata))
			pdata->pins_default = pinctrl_lookup_state(host->pinctrl,
								  "sd_1bit_pins");
		else
			pdata->pins_default = pinctrl_lookup_state(host->pinctrl,
								  "default");
		if (IS_ERR(pdata->pins_default)) {
			ret = PTR_ERR(pdata->pins_default);
			goto free_host;
		}

		if (aml_card_type_non_sdio(pdata))
			pdata->pins_clk_gate =
				pinctrl_lookup_state(host->pinctrl,
						"sd_clk-gate");
		if (aml_card_type_sdio(pdata))
			pdata->pins_clk_gate =
				pinctrl_lookup_state(host->pinctrl,
						"sdio_clk-gate");

		if (IS_ERR(pdata->pins_clk_gate)) {
			dev_warn(&pdev->dev,
				 "can't get clk-gate pinctrl, using clk_stop bit\n");
			pdata->pins_clk_gate = NULL;
		}

		if (aml_card_type_mmc(pdata))
			mmc->caps |= MMC_CAP_CMD23;
		if (host->dram_access_quirk) {
			/* Limit segments to 1 due to low available sram memory */
			mmc->max_segs = 1;
			/* Limit to the available sram memory */
			mmc->max_blk_count = SD_EMMC_SRAM_DATA_BUF_LEN /
					     mmc->max_blk_size;
		} else {
			mmc->max_blk_count = CMD_CFG_LENGTH_MASK;
			mmc->max_segs = SD_EMMC_MAX_SEGS;
		}
		mmc->max_req_size = SD_EMMC_MAX_REQ_SIZE;
		mmc->max_seg_size = mmc->max_req_size;
		mmc->ocr_avail = 0x200080;
		mmc->max_current_180 = 300; /* 300 mA in 1.8V */
		mmc->max_current_330 = 300; /* 300 mA in 3.3V */

		if (aml_card_type_sdio(pdata)) {
			/* do NOT run mmc_rescan for the first time */
			mmc->rescan_entered = 1;
		} else {
			mmc->rescan_entered = 0;
		}
		if (aml_card_type_non_sdio(pdata))
			pdata->pins_default = pinctrl_lookup_state(host->pinctrl, "sd_default");

		mmc->ops = &meson_mmc_ops;
		mmc_add_host(mmc);

		if (aml_card_type_sdio(pdata)) {/* if sdio_wifi */
			sdio_host = mmc;
		}

		if (aml_card_type_mmc(pdata)) {
			INIT_DELAYED_WORK(&host->dtbkey, add_dtbkey);
			schedule_delayed_work(&host->dtbkey, 50);
		}

#if CONFIG_AMLOGIC_KERNEL_VERSION == 13515
#ifdef CONFIG_ANDROID_VENDOR_HOOKS
		if (aml_card_type_non_sdio(pdata)) {
			ret =
			register_trace_android_vh_mmc_sd_update_cmdline_timing(SD_CMD_TIMING,
				NULL);
			if (ret)
				pr_err("register update_cmdline_timing failed, err:%d\n", ret);
			ret =
			register_trace_android_vh_mmc_sd_update_dataline_timing(SD_DATA_TIMING,
				NULL);
			if (ret)
				pr_err("register update_dataline timing failed, err:%d\n", ret);
		}
#endif
#endif
		dev_notice(host->dev, "host probe success!, %s\n", mmc_hostname(mmc));
	}
	return 0;

err_bounce_buf:
	if (!host->dram_access_quirk)
		dma_free_coherent(host->dev, host->bounce_buf_size,
				  host->bounce_buf, host->bounce_dma_addr);
err_free_irq:
	devm_kfree(host->dev, host->adj_win);
	free_irq(host->irq, host);
err_init_clk:
	clk_disable_unprepare(host->mmc_clk);
err_core_clk:
	clk_disable_unprepare(host->core_clk);
free_host:
	dev_notice(host->dev, "host probe failed!\n");
	mmc_free_host(mmc);
	return ret;
}

static int g12a_mmc_remove(struct platform_device *pdev)
{
	struct meson_host *host = dev_get_drvdata(&pdev->dev);

	mmc_remove_host(host->mmc);

	/* disable interrupts */
	writel(0, host->regs + SD_EMMC_IRQ_EN);
	free_irq(host->irq, host);

	dma_free_coherent(host->dev, SD_EMMC_DESC_BUF_LEN,
			  host->descs, host->descs_dma_addr);

	if (!host->dram_access_quirk)
		dma_free_coherent(host->dev, host->bounce_buf_size,
				  host->bounce_buf, host->bounce_dma_addr);

	clk_disable_unprepare(host->mmc_clk);
	clk_disable_unprepare(host->core_clk);

	devm_kfree(host->dev, host->adj_win);
	mmc_free_host(host->mmc);
	return 0;
}

static const struct meson_mmc_data meson_g12a_data = {
	.tx_delay_mask	= CLK_V3_TX_DELAY_MASK,
	.rx_delay_mask	= CLK_V3_RX_DELAY_MASK,
	.always_on	= CLK_V3_ALWAYS_ON,
	.adjust		= SD_EMMC_V3_ADJUST,
};

static const struct of_device_id g12a_mmc_of_match[] = {
	{ .compatible = "amlogic,meson-g12a-mmc", .data = &meson_g12a_data },
	{}
};
MODULE_DEVICE_TABLE(of, g12a_mmc_of_match);

static struct platform_driver g12a_mmc_driver = {
	.probe		= g12a_mmc_probe,
	.remove		= g12a_mmc_remove,
	.driver		= {
		.name = G12A_DRIVER_NAME,
		.of_match_table = of_match_ptr(g12a_mmc_of_match),
	},
};

int __init g12a_mmc_init(void)
{
	return platform_driver_register(&g12a_mmc_driver);
}

void __exit g12a_mmc_exit(void)
{
	platform_driver_unregister(&g12a_mmc_driver);
}

//module_platform_driver(g12a_mmc_driver);

//MODULE_DESCRIPTION("Amlogic G12a SD/eMMC driver");
//MODULE_AUTHOR("Kevin Hilman <khilman@baylibre.com>");
//MODULE_LICENSE("GPL v2");
