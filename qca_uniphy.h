/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCA_UNIPHY_H
#define __QCA_UNIPHY_H

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/phylink.h>
#include <linux/reset.h>

#define QCA_UNIPHY_CHANNELS		5

#define UNIPHY_OFFSET_CALIB_4		0x1e0
#define   UNIPHY_CALIBRATION_DONE	BIT(7)

#define UNIPHY_MISC2_PHY_MODE		0x218
#define   UNIPHY_MISC2_PHY_MODE_MASK	GENMASK(6, 4)
#define     UNIPHY_MISC2_SGMII		FIELD_PREP_CONST(UNIPHY_MISC2_PHY_MODE_MASK, 0x3)
#define     UNIPHY_MISC2_SGMIIPLUS	FIELD_PREP_CONST(UNIPHY_MISC2_PHY_MODE_MASK, 0x5)

#define UNIPHY_MODE_CTRL		0x46c
#define   UNIPHY_MODE_SEL_MASK		GENMASK(12, 8)
#define   UNIPHY_SGPLUS_MODE		BIT(11)
#define   UNIPHY_SGMII_MODE		BIT(10)
#define   UNIPHY_CH0_PSGMII_QSGMII	BIT(9)
#define   UNIPHY_CH0_QSGMII_SGMII	BIT(8)
#define   UNIPHY_AUTONEG_MODE_ATH	BIT(0)

#define UNIPHY_PLL_POWER_ON_AND_RESET	0x780
#define   UNIPHY_PLL_RESET_ANALOG	BIT(6)

#define UNIPHY_CH_BASE(ch)		(0x480 + (ch) * 0x18)
#define UNIPHY_CH_INPUT_OUTPUT_4(ch)	(UNIPHY_CH_BASE(ch) + 0x0)
#define UNIPHY_CH_INPUT_OUTPUT_6(ch)	(UNIPHY_CH_BASE(ch) + 0x8)
#define   UNIPHY_CH_ADP_SW_RSTN		BIT(11)
#define   UNIPHY_CH_RX_PAUSE		BIT(0)
#define   UNIPHY_CH_TX_PAUSE		BIT(1)
#define   UNIPHY_CH_SPEED_MODE		GENMASK(5, 4)
#define   UNIPHY_CH_DUPLEX		BIT(6)
#define   UNIPHY_CH_LINK		BIT(7)

#define UNIPHY_CALIBRATION_TIMEOUT_US	100000
#define UNIPHY_CALIBRATION_POLL_US	1000

struct qca_uniphy;

struct qca_uniphy_clk {
	struct clk_hw hw;
	struct qca_uniphy *uniphy;
};

struct qca_uniphy_pcs {
	struct phylink_pcs pcs;
	struct qca_uniphy *uniphy;
	int channel;
};

struct qca_uniphy {
	struct device *dev;
	struct regmap *regmap;
	struct reset_control *rst_soft;
	struct reset_control *rst_xpcs;
	struct reset_control_bulk_data rst_ports[QCA_UNIPHY_CHANNELS];
	struct clk_bulk_data *clks;
	int num_clks;
	struct qca_uniphy_pcs port_pcs[QCA_UNIPHY_CHANNELS];
	struct qca_uniphy_clk rx_clk;
	struct qca_uniphy_clk tx_clk;
};

#define port_rx_clk_idx(upcs)	((upcs)->channel * 2) + 2
#define port_tx_clk_idx(upcs)	(((upcs)->channel * 2) + 1) + 2

struct phylink_pcs *qca_uniphy_pcs_get(struct device *dev,
				       struct device_node *np,
				       int channel);
void qca_uniphy_pcs_put(struct phylink_pcs *pcs);

#endif
