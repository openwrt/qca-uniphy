// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm IPQ6018 UNIPHY PCS driver
 *
 * Copyright (c) 2025 The OpenWrt Project
 *
 * Standalone PCS driver for the UNIPHY SerDes blocks in IPQ6018 SoCs.
 * Each UNIPHY instance provides up to 5 SGMII channels (PSGMII mode)
 * or a single channel for SGMII/USXGMII. The driver is consumed by the
 * IPQ6018 PPE DSA switch driver via qca_uniphy_pcs_get().
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "qca_uniphy.h"

static struct qca_uniphy_pcs *
pcs_to_uniphy_pcs(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct qca_uniphy_pcs, pcs);
}

static unsigned long
qca_uniphy_clk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct qca_uniphy_clk *uclk =
		container_of(hw, struct qca_uniphy_clk, hw);
	struct qca_uniphy *uniphy = uclk->uniphy;
	u32 val;

	/*
	 * UNIPHY switch reference clock based on the configured
	 * PHY mode.
	 */
	regmap_read(uniphy->regmap, UNIPHY_MODE_CTRL, &val);
	if (val & UNIPHY_CH0_PSGMII_QSGMII ||
	    val & UNIPHY_CH0_QSGMII_SGMII)
		return 125000000;
	else if (val & UNIPHY_XPCS_MODE)
		return 312500000;

	return 0;
}

static const struct clk_ops qca_uniphy_clk_ops = {
	.recalc_rate = qca_uniphy_clk_recalc_rate,
};

static int qca_uniphy_clk_register(struct qca_uniphy *uniphy,
				   struct qca_uniphy_clk *uclk,
				   const char *name)
{
	struct clk_init_data init = {
		.name = name,
		.ops = &qca_uniphy_clk_ops,
		/* always derive rate from the UNIPHY register */
		.flags = CLK_GET_RATE_NOCACHE,
	};

	uclk->hw.init = &init;
	uclk->uniphy = uniphy;

	return devm_clk_hw_register(uniphy->dev, &uclk->hw);
}

static unsigned int
qca_uniphy_pcs_inband_caps(struct phylink_pcs *pcs,
			   phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_PSGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_USXGMII:
		return LINK_INBAND_DISABLE | LINK_INBAND_ENABLE;
	case PHY_INTERFACE_MODE_2500BASEX:
		return LINK_INBAND_DISABLE;
	default:
		return 0;
	}
}

static void qca_uniphy_pcs_get_state_sgmii(struct qca_uniphy *uniphy,
					   int channel,
					   struct phylink_link_state *state)
{
	u32 val;

	regmap_read(uniphy->regmap,
		    UNIPHY_CH_INPUT_OUTPUT_6(channel),
		    &val);

	state->link = !!(val & UNIPHY_CH_LINK);
	if (!state->link)
		return;

	state->duplex = (val & UNIPHY_CH_DUPLEX) ? DUPLEX_FULL : DUPLEX_HALF;

	switch (FIELD_GET(UNIPHY_CH_SPEED_MODE, val)) {
	case 0:
		state->speed = SPEED_10;
		break;
	case 1:
		state->speed = SPEED_100;
		break;
	case 2:
		state->speed = SPEED_1000;
		break;
	default:
		state->link = false;
		return;
	}

	state->pause = 0;
	if (val & UNIPHY_CH_RX_PAUSE)
		state->pause |= MLO_PAUSE_RX;
	if (val & UNIPHY_CH_TX_PAUSE)
		state->pause |= MLO_PAUSE_TX;

	state->an_complete = state->link;
}

static void qca_uniphy_pcs_get_state_usxgmii(struct qca_uniphy *uniphy,
					     struct phylink_link_state *state)
{
	unsigned int val;
	int ret;

	ret = regmap_read(uniphy->regmap, XPCS_MII_AN_INTR_STS, &val);
	if (ret) {
		state->link = 0;
		return;
	}

	state->link = !!(val & XPCS_USXG_AN_LINK_STS);

	if (!state->link)
		return;

	switch (FIELD_GET(XPCS_USXG_AN_SPEED_MASK, val)) {
	case XPCS_USXG_AN_SPEED_10000:
		state->speed = SPEED_10000;
		break;
	case XPCS_USXG_AN_SPEED_5000:
		state->speed = SPEED_5000;
		break;
	case XPCS_USXG_AN_SPEED_2500:
		state->speed = SPEED_2500;
		break;
	case XPCS_USXG_AN_SPEED_1000:
		state->speed = SPEED_1000;
		break;
	case XPCS_USXG_AN_SPEED_100:
		state->speed = SPEED_100;
		break;
	case XPCS_USXG_AN_SPEED_10:
		state->speed = SPEED_10;
		break;
	default:
		state->link = false;
		return;
	}

	state->duplex = DUPLEX_FULL;
}

static void qca_uniphy_pcs_get_state(struct phylink_pcs *pcs,
				     struct phylink_link_state *state)
{
	struct qca_uniphy_pcs *upcs = pcs_to_uniphy_pcs(pcs);
	struct qca_uniphy *uniphy = upcs->uniphy;

	switch (state->interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_PSGMII:
		qca_uniphy_pcs_get_state_sgmii(uniphy, upcs->channel,
					       state);
		break;
	case PHY_INTERFACE_MODE_USXGMII:
		qca_uniphy_pcs_get_state_usxgmii(uniphy, state);
		break;
	default:
		break;
	}
}

static int qca_uniphy_pcs_config_mode(struct phylink_pcs *pcs,
				      unsigned int neg_mode,
				      phy_interface_t interface,
				      const unsigned long *advertising,
				      bool permit_pause_to_mac)
{
	struct qca_uniphy_pcs *upcs = pcs_to_uniphy_pcs(pcs);
	struct qca_uniphy *uniphy = upcs->uniphy;
	u32 misc2_phy_mode;
	u32 mode_ctrl;
	u32 val;
	int ret;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
		misc2_phy_mode = UNIPHY_MISC2_SGMII;
		mode_ctrl = UNIPHY_SGMII_MODE;
		break;
	case PHY_INTERFACE_MODE_QSGMII:
		mode_ctrl = UNIPHY_CH0_QSGMII_SGMII;
		misc2_phy_mode = UNIPHY_MISC2_SGMII;
		break;
	case PHY_INTERFACE_MODE_PSGMII:
		mode_ctrl = UNIPHY_CH0_PSGMII_QSGMII;
		misc2_phy_mode = 0;
		break;
	case PHY_INTERFACE_MODE_USXGMII:
		misc2_phy_mode = UNIPHY_MISC2_USXGMII;
		mode_ctrl = UNIPHY_XPCS_MODE;
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		misc2_phy_mode = UNIPHY_MISC2_SGMIIPLUS;
		mode_ctrl = UNIPHY_SGPLUS_MODE;
		break;
	default:
		return -EINVAL;
	}

	/* First update misc2 PHY mode... */
	regmap_update_bits(uniphy->regmap, UNIPHY_MISC2_PHY_MODE,
			   UNIPHY_MISC2_PHY_MODE_MASK, misc2_phy_mode);

	/* ...and reset Analog */
	regmap_clear_bits(uniphy->regmap, UNIPHY_PLL_POWER_ON_AND_RESET,
			  UNIPHY_PLL_RESET_ANALOG);
	msleep(100);
	regmap_set_bits(uniphy->regmap, UNIPHY_PLL_POWER_ON_AND_RESET,
			UNIPHY_PLL_RESET_ANALOG);
	msleep(100);

	/* Second assert XPCS... */
	reset_control_assert(uniphy->rst_xpcs);

	/* ...and disable PHY clock */
	clk_disable(uniphy->clks[port_rx_clk_idx(upcs)].clk);
	clk_disable(uniphy->clks[port_tx_clk_idx(upcs)].clk);

	/* Third update the mode ctrl... */
	regmap_update_bits(uniphy->regmap, UNIPHY_MODE_CTRL,
			   UNIPHY_MODE_SEL_MASK | UNIPHY_AUTONEG_MODE_ATH,
			   mode_ctrl);

	/*
	 * ...and execute soft reset...
	 *
	 * (soft reset is XPCS + all the UNIPHY PHY port reset,
	 * XPCS gets indirectly deassert by the soft reset deassert.
	 * It seems that resetting all at once is mandatory as from
	 * lots of testing it has been verified that operating
	 * on the single reset is problematic)
	 */
	reset_control_assert(uniphy->rst_soft);
	msleep(100);
	reset_control_deassert(uniphy->rst_soft);

	/* ...and wait for calibration */
	ret = regmap_read_poll_timeout(uniphy->regmap, UNIPHY_OFFSET_CALIB_4,
				       val, val & UNIPHY_CALIBRATION_DONE,
				       UNIPHY_CALIBRATION_POLL_US,
				       UNIPHY_CALIBRATION_TIMEOUT_US);
	if (ret) {
		dev_err(uniphy->dev, "PCS calibration timeout\n");
		return -EINVAL;
	}

	/* As last step enable PHY clock... */
	clk_enable(uniphy->clks[port_rx_clk_idx(upcs)].clk);
	clk_enable(uniphy->clks[port_tx_clk_idx(upcs)].clk);

	if (interface == PHY_INTERFACE_MODE_USXGMII)
		reset_control_deassert(uniphy->rst_xpcs);

	return 0;
}

static int qca_uniphy_pcs_config_usxgmii(struct phylink_pcs *pcs,
					 unsigned int neg_mode,
					 phy_interface_t interface,
					 const unsigned long *advertising,
					 bool permit_pause_to_mac)
{
	struct qca_uniphy_pcs *upcs = pcs_to_uniphy_pcs(pcs);
	struct qca_uniphy *uniphy = upcs->uniphy;
	int ret;

	ret = qca_uniphy_pcs_config_mode(pcs, neg_mode, interface, advertising, permit_pause_to_mac);
	if (ret)
		return ret;

	ret = regmap_set_bits(uniphy->regmap, XPCS_DIG_CTRL, XPCS_USXG_EN);
	if (ret)
		return ret;

	ret = regmap_set_bits(uniphy->regmap, XPCS_MII_AN_CTRL, XPCS_MII_AN_8BIT);
	if (ret)
		return ret;

	return regmap_set_bits(uniphy->regmap, XPCS_MII_CTRL, XPCS_MII_AN_EN);
}

static int qca_uniphy_pcs_config(struct phylink_pcs *pcs,
				 unsigned int neg_mode,
				 phy_interface_t interface,
				 const unsigned long *advertising,
				 bool permit_pause_to_mac)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_PSGMII:
		return qca_uniphy_pcs_config_mode(pcs, neg_mode, interface, advertising, permit_pause_to_mac);
	case PHY_INTERFACE_MODE_USXGMII:
		return qca_uniphy_pcs_config_usxgmii(pcs, neg_mode, interface, advertising, permit_pause_to_mac);
	default:
		return -EOPNOTSUPP;
	}
}

static int uniphy_link_up_usxgmii(struct phylink_pcs *pcs, int speed)
{
	struct qca_uniphy_pcs *upcs = pcs_to_uniphy_pcs(pcs);
	struct qca_uniphy *uniphy = upcs->uniphy;
	unsigned int val, uniphy_rate;
	int ret;

	switch (speed) {
	case SPEED_10000:
		val = XPCS_SPEED_10000;
		uniphy_rate = 312500000;
		break;
	case SPEED_5000:
		val = XPCS_SPEED_5000;
		uniphy_rate = 156250000;
		break;
	case SPEED_2500:
		val = XPCS_SPEED_2500;
		uniphy_rate = 78125000;
		break;
	case SPEED_1000:
		val = XPCS_SPEED_1000;
		uniphy_rate = 125000000;
		break;
	case SPEED_100:
		val = XPCS_SPEED_100;
		uniphy_rate = 12500000;
		break;
	case SPEED_10:
		val = XPCS_SPEED_10;
		uniphy_rate = 1250000;
		break;
	default:
		dev_err(uniphy->dev, "Invalid USXGMII speed %d\n", speed);
		return -EINVAL;
	}

	clk_set_rate(uniphy->clks[port_rx_clk_idx(upcs)].clk, uniphy_rate);
	clk_set_rate(uniphy->clks[port_tx_clk_idx(upcs)].clk, uniphy_rate);

	/* Configure XPCS speed */
	ret = regmap_update_bits(uniphy->regmap, XPCS_MII_CTRL,
				 XPCS_SPEED_MASK, val | XPCS_DUPLEX_FULL);
	if (ret)
		return ret;

	/* XPCS adapter reset */
	return regmap_set_bits(uniphy->regmap, XPCS_DIG_CTRL, XPCS_USXG_ADPT_RESET);
}

static void qca_uniphy_pcs_link_up(struct phylink_pcs *pcs,
				   unsigned int neg_mode,
				   phy_interface_t interface,
				   int speed, int duplex)
{
	struct qca_uniphy_pcs *upcs = pcs_to_uniphy_pcs(pcs);
	struct qca_uniphy *uniphy = upcs->uniphy;
	unsigned long uniphy_rate;
	int ret = 0;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_PSGMII:
		switch (speed) {
		case SPEED_10:
			uniphy_rate = 2500000;
			break;
		case SPEED_100:
			uniphy_rate = 25000000;
			break;
		case SPEED_1000:
			uniphy_rate = 125000000;
			break;
		default:
			return;
		}

		clk_set_rate(uniphy->clks[port_rx_clk_idx(upcs)].clk, uniphy_rate);
		clk_set_rate(uniphy->clks[port_tx_clk_idx(upcs)].clk, uniphy_rate);

		regmap_clear_bits(uniphy->regmap, UNIPHY_CH_INPUT_OUTPUT_4(upcs->channel),
			  UNIPHY_CH_ADP_SW_RSTN);
		usleep_range(1000, 2000);
		regmap_set_bits(uniphy->regmap, UNIPHY_CH_INPUT_OUTPUT_4(upcs->channel),
				UNIPHY_CH_ADP_SW_RSTN);
		break;
	case PHY_INTERFACE_MODE_USXGMII:
		ret = uniphy_link_up_usxgmii(pcs, speed);
		break;
	default:
		return;
	}

	if (ret)
		dev_err(uniphy->dev, "PCS link up fail for interface %s\n",
			phy_modes(interface));
}

static const struct phylink_pcs_ops qca_uniphy_pcs_ops = {
	.pcs_inband_caps = qca_uniphy_pcs_inband_caps,
	.pcs_get_state = qca_uniphy_pcs_get_state,
	.pcs_config = qca_uniphy_pcs_config,
	.pcs_link_up = qca_uniphy_pcs_link_up,
};

static const struct of_device_id qca_uniphy_of_match[] = {
	{ .compatible = "qualcomm,ipq6018-uniphy" },
	{ .compatible = "qualcomm,ipq8074-uniphy" },
	{ },
};
MODULE_DEVICE_TABLE(of, qca_uniphy_of_match);

struct phylink_pcs *qca_uniphy_pcs_get(struct device *dev,
				       struct device_node *np,
				       int channel)
{
	struct platform_device *pdev;
	struct qca_uniphy *uniphy;

	if (!np)
		return NULL;

	if (!of_device_is_available(np))
		return ERR_PTR(-ENODEV);

	if (!of_match_node(qca_uniphy_of_match, np))
		return ERR_PTR(-EINVAL);

	pdev = of_find_device_by_node(np);
	if (!pdev || !platform_get_drvdata(pdev)) {
		if (pdev)
			put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	uniphy = platform_get_drvdata(pdev);

	if (channel < 0 || channel >= QCA_UNIPHY_CHANNELS) {
		put_device(&pdev->dev);
		return ERR_PTR(-EINVAL);
	}

	device_link_add(dev, uniphy->dev, DL_FLAG_AUTOREMOVE_CONSUMER);

	return &uniphy->port_pcs[channel].pcs;
}
EXPORT_SYMBOL_GPL(qca_uniphy_pcs_get);

void qca_uniphy_pcs_put(struct phylink_pcs *pcs)
{
	struct qca_uniphy *uniphy;
	struct qca_uniphy_pcs *upcs;

	if (!pcs)
		return;

	upcs = pcs_to_uniphy_pcs(pcs);
	uniphy = upcs->uniphy;

	put_device(uniphy->dev);
}
EXPORT_SYMBOL_GPL(qca_uniphy_pcs_put);

static void qca_uniphy_clk_disable_unprepare(void *data)
{
	struct qca_uniphy *uniphy = data;

	clk_bulk_disable_unprepare(uniphy->num_clks, uniphy->clks);
}

static int uniphy_pcs_regmap_read(void *context, unsigned int reg,
				  unsigned int *val)
{
	struct qca_uniphy *uniphy = context;

	/* PCS uses direct AHB access while XPCS uses indirect AHB access */
	if (reg >= XPCS_INDIRECT_ADDR) {
		writel(FIELD_GET(XPCS_INDIRECT_ADDR_H, reg),
		       uniphy->base + XPCS_INDIRECT_AHB_ADDR);
		*val = readl(uniphy->base + XPCS_INDIRECT_DATA_ADDR(reg));
	} else {
		*val = readl(uniphy->base + reg);
	}

	return 0;
}

static int uniphy_pcs_regmap_write(void *context, unsigned int reg,
				   unsigned int val)
{
	struct qca_uniphy *uniphy = context;

	/* PCS uses direct AHB access while XPCS uses indirect AHB access */
	if (reg >= XPCS_INDIRECT_ADDR) {
		writel(FIELD_GET(XPCS_INDIRECT_ADDR_H, reg),
		       uniphy->base + XPCS_INDIRECT_AHB_ADDR);
		writel(val, uniphy->base + XPCS_INDIRECT_DATA_ADDR(reg));
	} else {
		writel(val, uniphy->base + reg);
	}

	return 0;
}

static const struct regmap_config uniphy_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_read = uniphy_pcs_regmap_read,
	.reg_write = uniphy_pcs_regmap_write,
	.fast_io = true,
};

static int qca_uniphy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_bulk_data *clks;
	struct qca_uniphy *uniphy;
	const char *name;
	int ret, i, num_clks;

	uniphy = devm_kzalloc(dev, sizeof(*uniphy), GFP_KERNEL);
	if (!uniphy)
		return -ENOMEM;

	uniphy->dev = dev;

	uniphy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(uniphy->base))
		return dev_err_probe(dev, PTR_ERR(uniphy->base), "failed to ioremap resource");

	uniphy->regmap = devm_regmap_init(dev, NULL, uniphy, &uniphy_regmap_cfg);
	if (IS_ERR(uniphy->regmap))
		return dev_err_probe(dev, PTR_ERR(uniphy->regmap),
				     "failed to init regmap");

	num_clks = devm_clk_bulk_get_all(dev, &clks);
	if (num_clks < 0)
		return dev_err_probe(dev, num_clks, "failed to get clocks\n");

	ret = clk_bulk_prepare_enable(num_clks, clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	uniphy->clks = clks;
	uniphy->num_clks = num_clks;

	ret = devm_add_action_or_reset(dev, qca_uniphy_clk_disable_unprepare,
				       uniphy);
	if (ret)
		return ret;

	uniphy->rst_soft = devm_reset_control_get_exclusive(dev, "soft");
	if (IS_ERR(uniphy->rst_soft))
		return dev_err_probe(dev, PTR_ERR(uniphy->rst_soft),
				     "failed to get soft reset\n");

	uniphy->rst_xpcs = devm_reset_control_get_exclusive(dev, "xpcs");
	if (IS_ERR(uniphy->rst_xpcs))
		return dev_err_probe(dev, PTR_ERR(uniphy->rst_xpcs),
				     "failed to get xpcs reset\n");

	if (of_property_read_string_index(dev->of_node, "clock-output-names",
					  0, &name))
		return -ENODEV;

	ret = qca_uniphy_clk_register(uniphy, &uniphy->rx_clk, name);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register RX clock\n");

	if (of_property_read_string_index(dev->of_node, "clock-output-names",
					  1, &name))
			return -ENODEV;

	ret = qca_uniphy_clk_register(uniphy, &uniphy->tx_clk, name);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register TX clock\n");

	for (i = 0; i < QCA_UNIPHY_CHANNELS; i++) {
		uniphy->port_pcs[i].pcs.ops = &qca_uniphy_pcs_ops;
		uniphy->port_pcs[i].pcs.neg_mode = true;
		uniphy->port_pcs[i].pcs.poll = true;
		uniphy->port_pcs[i].uniphy = uniphy;
		uniphy->port_pcs[i].channel = i;
	}

	platform_set_drvdata(pdev, uniphy);

	return 0;
}

static struct platform_driver qca_uniphy_driver = {
	.driver = {
		.name			= "qca-uniphy",
		.suppress_bind_attrs	= true,
		.of_match_table		= qca_uniphy_of_match,
	},
	.probe = qca_uniphy_probe,
};
module_platform_driver(qca_uniphy_driver);

MODULE_DESCRIPTION("Qualcomm IPQ6018 UNIPHY PCS driver");
MODULE_LICENSE("GPL");
