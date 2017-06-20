/*
 * Qualcomm PCIe root complex driver
 *
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 * Copyright 2015 Linaro Limited.
 *
 * Author: Stanimir Varbanov <svarbanov@mm-sol.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "pcie-designware.h"

/* DBI registers */
#define PCIE20_AXI_MSTR_RESP_COMP_CTRL0		0x818
#define PCIE20_AXI_MSTR_RESP_COMP_CTRL1		0x81c

#define PCIE20_PLR_IATU_VIEWPORT		0x900
#define PCIE20_PLR_IATU_REGION_OUTBOUND		(0x0 << 31)
#define PCIE20_PLR_IATU_REGION_INDEX(x)		(x << 0)

#define PCIE20_PLR_IATU_CTRL1			0x904
#define PCIE20_PLR_IATU_TYPE_CFG0		(0x4 << 0)
#define PCIE20_PLR_IATU_TYPE_MEM		(0x0 << 0)

#define PCIE20_PLR_IATU_CTRL2			0x908
#define PCIE20_PLR_IATU_ENABLE			BIT(31)

#define PCIE20_PLR_IATU_LBAR			0x90C
#define PCIE20_PLR_IATU_UBAR			0x910
#define PCIE20_PLR_IATU_LAR			0x914
#define PCIE20_PLR_IATU_LTAR			0x918
#define PCIE20_PLR_IATU_UTAR			0x91c

#define MSM_PCIE_DEV_CFG_ADDR			0x01000000

/* PARF registers */
#define PCIE20_PARF_PCS_DEEMPH			0x34
#define PCS_DEEMPH_TX_DEEMPH_GEN1(x)		(x << 16)
#define PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(x)	(x << 8)
#define PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(x)	(x << 0)

#define PCIE20_PARF_PCS_SWING			0x38
#define PCS_SWING_TX_SWING_FULL(x)		(x << 8)
#define PCS_SWING_TX_SWING_LOW(x)		(x << 0)

#define PCIE20_PARF_PHY_CTRL			0x40
#define PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK	(0x1f << 16)
#define PHY_CTRL_PHY_TX0_TERM_OFFSET(x)		(x << 16)

#define PCIE20_PARF_PHY_REFCLK			0x4C
#define REF_SSP_EN				BIT(16)
#define REF_USE_PAD				BIT(12)

#define PCIE20_PARF_CONFIG_BITS			0x50
#define PHY_RX0_EQ(x)				(x << 24)

#define PCIE20_PARF_DBI_BASE_ADDR		0x168
#define PCIE20_PARF_SLV_ADDR_SPACE_SIZE		0x16c
#define PCIE20_PARF_AXI_MSTR_WR_ADDR_HALT	0x178

#define PCIE20_ELBI_SYS_CTRL			0x04
#define PCIE20_ELBI_SYS_CTRL_LT_ENABLE		BIT(0)

#define PCIE20_CAP				0x70

#define PERST_DELAY_US				1000

#define __set(v, a, b)	(((v) << (b)) & GENMASK(a, b))
#define __mask(a, b)	(((1 << ((a) + 1)) - 1) & ~((1 << (b)) - 1))
#define PCIE20_DEV_CAS			0x78
#define PCIE20_MRRS_MASK		__mask(14, 12)
#define PCIE20_MRRS(x)			__set(x, 14, 12)
#define PCIE20_MPS_MASK			__mask(7, 5)
#define PCIE20_MPS(x)			__set(x, 7, 5)

struct qcom_pcie_resources_v0 {
	struct clk *iface_clk;
	struct clk *core_clk;
	struct clk *phy_clk;
	struct clk *aux_clk;
	struct clk *ref_clk;
	struct reset_control *pci_reset;
	struct reset_control *axi_reset;
	struct reset_control *ahb_reset;
	struct reset_control *por_reset;
	struct reset_control *phy_reset;
	struct reset_control *ext_reset;
	struct regulator *vdda;
	struct regulator *vdda_phy;
	struct regulator *vdda_refclk;
	uint8_t phy_tx0_term_offset;
};

struct qcom_pcie_resources_v1 {
	struct clk *iface;
	struct clk *aux;
	struct clk *master_bus;
	struct clk *slave_bus;
	struct reset_control *core;
	struct regulator *vdda;
};

union qcom_pcie_resources {
	struct qcom_pcie_resources_v0 v0;
	struct qcom_pcie_resources_v1 v1;
};

struct qcom_pcie;

struct qcom_pcie_ops {
	int (*get_resources)(struct qcom_pcie *pcie);
	int (*init)(struct qcom_pcie *pcie);
	void (*configure)(struct qcom_pcie *pcie);
	void (*deinit)(struct qcom_pcie *pcie);
};

struct qcom_pcie {
	struct pcie_port pp;			/* pp.dbi_base is DT dbi */
	void __iomem *parf;			/* DT parf */
	void __iomem *elbi;			/* DT elbi */
	union qcom_pcie_resources res;
	struct phy *phy;
	struct gpio_desc *reset;
	struct qcom_pcie_ops *ops;
};

#define to_qcom_pcie(x)		container_of(x, struct qcom_pcie, pp)

static void qcom_ep_reset_assert(struct qcom_pcie *pcie)
{
	gpiod_set_value(pcie->reset, 1);
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

static void qcom_ep_reset_deassert(struct qcom_pcie *pcie)
{
	gpiod_set_value(pcie->reset, 0);
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

static irqreturn_t qcom_pcie_msi_irq_handler(int irq, void *arg)
{
	struct pcie_port *pp = arg;

	return dw_handle_msi_irq(pp);
}

static int qcom_pcie_establish_link(struct qcom_pcie *pcie)
{
	u32 val;

	if (dw_pcie_link_up(&pcie->pp))
		return 0;

	/* enable link training */
	val = readl(pcie->elbi + PCIE20_ELBI_SYS_CTRL);
	val |= PCIE20_ELBI_SYS_CTRL_LT_ENABLE;
	writel(val, pcie->elbi + PCIE20_ELBI_SYS_CTRL);

	return dw_pcie_wait_for_link(&pcie->pp);
}

static int qcom_pcie_get_resources_v0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v0 *res = &pcie->res.v0;
	struct device *dev = pcie->pp.dev;

	res->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(res->vdda))
		return PTR_ERR(res->vdda);

	res->vdda_phy = devm_regulator_get(dev, "vdda_phy");
	if (IS_ERR(res->vdda_phy))
		return PTR_ERR(res->vdda_phy);

	res->vdda_refclk = devm_regulator_get(dev, "vdda_refclk");
	if (IS_ERR(res->vdda_refclk))
		return PTR_ERR(res->vdda_refclk);

	res->iface_clk = devm_clk_get(dev, "iface");
	if (IS_ERR(res->iface_clk))
		return PTR_ERR(res->iface_clk);

	res->core_clk = devm_clk_get(dev, "core");
	if (IS_ERR(res->core_clk))
		return PTR_ERR(res->core_clk);

	res->phy_clk = devm_clk_get(dev, "phy");
	if (IS_ERR(res->phy_clk))
		return PTR_ERR(res->phy_clk);

	res->aux_clk = devm_clk_get(dev, "aux");
	if (IS_ERR(res->aux_clk))
		return PTR_ERR(res->aux_clk);

	res->ref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(res->ref_clk))
		return PTR_ERR(res->ref_clk);

	res->pci_reset = devm_reset_control_get(dev, "pci");
	if (IS_ERR(res->pci_reset))
		return PTR_ERR(res->pci_reset);

	res->axi_reset = devm_reset_control_get(dev, "axi");
	if (IS_ERR(res->axi_reset))
		return PTR_ERR(res->axi_reset);

	res->ahb_reset = devm_reset_control_get(dev, "ahb");
	if (IS_ERR(res->ahb_reset))
		return PTR_ERR(res->ahb_reset);

	res->por_reset = devm_reset_control_get(dev, "por");
	if (IS_ERR(res->por_reset))
		return PTR_ERR(res->por_reset);

	res->phy_reset = devm_reset_control_get(dev, "phy");
	if (IS_ERR(res->phy_reset))
		return PTR_ERR(res->phy_reset);

	res->ext_reset = devm_reset_control_get(dev, "ext");
	if (IS_ERR(res->ext_reset))
		return PTR_ERR(res->ext_reset);

	if (of_property_read_u8(dev->of_node, "phy-tx0-term-offset",
				&res->phy_tx0_term_offset))
		res->phy_tx0_term_offset = 0;

	return 0;
}

static int qcom_pcie_get_resources_v1(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v1 *res = &pcie->res.v1;
	struct device *dev = pcie->pp.dev;

	res->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(res->vdda))
		return PTR_ERR(res->vdda);

	res->iface = devm_clk_get(dev, "iface");
	if (IS_ERR(res->iface))
		return PTR_ERR(res->iface);

	res->aux = devm_clk_get(dev, "aux");
	if (IS_ERR(res->aux))
		return PTR_ERR(res->aux);

	res->master_bus = devm_clk_get(dev, "master_bus");
	if (IS_ERR(res->master_bus))
		return PTR_ERR(res->master_bus);

	res->slave_bus = devm_clk_get(dev, "slave_bus");
	if (IS_ERR(res->slave_bus))
		return PTR_ERR(res->slave_bus);

	res->core = devm_reset_control_get(dev, "core");
	if (IS_ERR(res->core))
		return PTR_ERR(res->core);

	return 0;
}

static void qcom_pcie_deinit_v0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v0 *res = &pcie->res.v0;

	reset_control_assert(res->pci_reset);
	reset_control_assert(res->axi_reset);
	reset_control_assert(res->ahb_reset);
	reset_control_assert(res->por_reset);
	reset_control_assert(res->phy_reset);
	reset_control_assert(res->ext_reset);
	clk_disable_unprepare(res->iface_clk);
	clk_disable_unprepare(res->core_clk);
	clk_disable_unprepare(res->phy_clk);
	clk_disable_unprepare(res->aux_clk);
	clk_disable_unprepare(res->ref_clk);
	regulator_disable(res->vdda);
	regulator_disable(res->vdda_phy);
	regulator_disable(res->vdda_refclk);
}

static void qcom_pcie_prog_viewport_cfg0(struct qcom_pcie *pcie, u32 busdev)
{
	struct pcie_port *pp = &pcie->pp;

	/*
	 * program and enable address translation region 0 (device config
	 * address space); region type config;
	 * axi config address range to device config address range
	 */
	writel(PCIE20_PLR_IATU_REGION_OUTBOUND |
	       PCIE20_PLR_IATU_REGION_INDEX(0),
	       pcie->pp.dbi_base + PCIE20_PLR_IATU_VIEWPORT);

	writel(PCIE20_PLR_IATU_TYPE_CFG0, pcie->pp.dbi_base + PCIE20_PLR_IATU_CTRL1);
	writel(PCIE20_PLR_IATU_ENABLE, pcie->pp.dbi_base + PCIE20_PLR_IATU_CTRL2);
	writel(pp->cfg0_base, pcie->pp.dbi_base + PCIE20_PLR_IATU_LBAR);
	writel((pp->cfg0_base >> 32), pcie->pp.dbi_base + PCIE20_PLR_IATU_UBAR);
	writel((pp->cfg0_base + pp->cfg0_size - 1),
	       pcie->pp.dbi_base + PCIE20_PLR_IATU_LAR);
	writel(busdev, pcie->pp.dbi_base + PCIE20_PLR_IATU_LTAR);
	writel(0, pcie->pp.dbi_base + PCIE20_PLR_IATU_UTAR);
}

static void qcom_pcie_prog_viewport_mem2_outbound(struct qcom_pcie *pcie)
{
	struct pcie_port *pp = &pcie->pp;

	/*
	 * program and enable address translation region 2 (device resource
	 * address space); region type memory;
	 * axi device bar address range to device bar address range
	 */
	writel(PCIE20_PLR_IATU_REGION_OUTBOUND |
	       PCIE20_PLR_IATU_REGION_INDEX(2),
	       pcie->pp.dbi_base + PCIE20_PLR_IATU_VIEWPORT);

	writel(PCIE20_PLR_IATU_TYPE_MEM, pcie->pp.dbi_base + PCIE20_PLR_IATU_CTRL1);
	writel(PCIE20_PLR_IATU_ENABLE, pcie->pp.dbi_base + PCIE20_PLR_IATU_CTRL2);
	writel(pp->mem_base, pcie->pp.dbi_base + PCIE20_PLR_IATU_LBAR);
	writel((pp->mem_base >> 32), pcie->pp.dbi_base + PCIE20_PLR_IATU_UBAR);
	writel(pp->mem_base + pp->mem_size - 1,
	       pcie->pp.dbi_base + PCIE20_PLR_IATU_LAR);
	writel(pp->mem_bus_addr, pcie->pp.dbi_base + PCIE20_PLR_IATU_LTAR);
	writel(upper_32_bits(pp->mem_bus_addr),
	       pcie->pp.dbi_base + PCIE20_PLR_IATU_UTAR);

	/* 256B PCIE buffer setting */
	writel(0x1, pcie->pp.dbi_base + PCIE20_AXI_MSTR_RESP_COMP_CTRL0);
	writel(0x1, pcie->pp.dbi_base + PCIE20_AXI_MSTR_RESP_COMP_CTRL1);
}

static int qcom_pcie_init_v0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v0 *res = &pcie->res.v0;
	struct device *dev = pcie->pp.dev;
	u32 val;
	int ret;

	ret = regulator_enable(res->vdda);
	if (ret) {
		dev_err(dev, "cannot enable vdda regulator\n");
		return ret;
	}

	ret = regulator_enable(res->vdda_refclk);
	if (ret) {
		dev_err(dev, "cannot enable vdda_refclk regulator\n");
		goto err_refclk;
	}

	ret = regulator_enable(res->vdda_phy);
	if (ret) {
		dev_err(dev, "cannot enable vdda_phy regulator\n");
		goto err_vdda_phy;
	}

	ret = reset_control_assert(res->ahb_reset);
	if (ret) {
		dev_err(dev, "cannot assert ahb reset\n");
		goto err_assert_reset;
	}

	ret = reset_control_deassert(res->ext_reset);
	if (ret) {
		dev_err(dev, "cannot deassert ext reset\n");
		goto err_assert_reset;
	}

	ret = clk_prepare_enable(res->iface_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable iface clock\n");
		goto err_assert_reset;
	}

	ret = clk_prepare_enable(res->phy_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable phy clock\n");
		goto err_clk_phy;
	}

	ret = clk_prepare_enable(res->core_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable core clock\n");
		goto err_clk_core;
	}

	ret = clk_prepare_enable(res->aux_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable aux clock\n");
		goto err_clk_aux;
	}

	ret = clk_prepare_enable(res->ref_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable ref clock\n");
		goto err_clk_ref;
	}

	ret = reset_control_deassert(res->ahb_reset);
	if (ret) {
		dev_err(dev, "cannot deassert ahb reset\n");
		goto err_deassert_ahb;
	}
	udelay(1);

	/* enable PCIe clocks and resets */
	val = readl(pcie->parf + PCIE20_PARF_PHY_CTRL);
	val &= ~BIT(0);
	writel(val, pcie->parf + PCIE20_PARF_PHY_CTRL);

	/* Set Tx termination offset */
	val = readl(pcie->parf + PCIE20_PARF_PHY_CTRL);
	val &= ~PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK;
	val |= PHY_CTRL_PHY_TX0_TERM_OFFSET(res->phy_tx0_term_offset);
	writel(val, pcie->parf + PCIE20_PARF_PHY_CTRL);

	/* PARF programming */
	writel(PCS_DEEMPH_TX_DEEMPH_GEN1(0x18) |
	       PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(0x18) |
	       PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(0x22),
	       pcie->parf + PCIE20_PARF_PCS_DEEMPH);
	writel(PCS_SWING_TX_SWING_FULL(0x78) |
	       PCS_SWING_TX_SWING_LOW(0x78),
	       pcie->parf + PCIE20_PARF_PCS_SWING);
	writel(PHY_RX0_EQ(0x4), pcie->parf + PCIE20_PARF_CONFIG_BITS);

	/* Enable reference clock */
	val = readl(pcie->parf + PCIE20_PARF_PHY_REFCLK);
	val &= ~REF_USE_PAD;
	val |= REF_SSP_EN;
	writel(val, pcie->parf + PCIE20_PARF_PHY_REFCLK);

	/* De-assert PHY, PCIe, POR and AXI resets */
	ret = reset_control_deassert(res->phy_reset);
	if (ret) {
		dev_err(dev, "cannot deassert phy reset\n");
		return ret;
	}

	ret = reset_control_deassert(res->pci_reset);
	if (ret) {
		dev_err(dev, "cannot deassert pci reset\n");
		return ret;
	}

	ret = reset_control_deassert(res->por_reset);
	if (ret) {
		dev_err(dev, "cannot deassert por reset\n");
		return ret;
	}

	ret = reset_control_deassert(res->axi_reset);
	if (ret) {
		dev_err(dev, "cannot deassert axi reset\n");
		return ret;
	}

	/* wait for clock acquisition */
	usleep_range(1000, 1500);

	return 0;

err_deassert_ahb:
	clk_disable_unprepare(res->ref_clk);
err_clk_ref:
	clk_disable_unprepare(res->aux_clk);
err_clk_aux:
	clk_disable_unprepare(res->core_clk);
err_clk_core:
	clk_disable_unprepare(res->phy_clk);
err_clk_phy:
	clk_disable_unprepare(res->iface_clk);
err_assert_reset:
	regulator_disable(res->vdda_phy);
err_vdda_phy:
	regulator_disable(res->vdda_refclk);
err_refclk:
	regulator_disable(res->vdda);

	return ret;
}

static void qcom_pcie_configure_v0(struct qcom_pcie *pcie)
{
	qcom_pcie_prog_viewport_cfg0(pcie, MSM_PCIE_DEV_CFG_ADDR);
	qcom_pcie_prog_viewport_mem2_outbound(pcie);
}

static void qcom_pcie_deinit_v1(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v1 *res = &pcie->res.v1;

	reset_control_assert(res->core);
	clk_disable_unprepare(res->slave_bus);
	clk_disable_unprepare(res->master_bus);
	clk_disable_unprepare(res->iface);
	clk_disable_unprepare(res->aux);
	regulator_disable(res->vdda);
}

static int qcom_pcie_init_v1(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v1 *res = &pcie->res.v1;
	struct device *dev = pcie->pp.dev;
	int ret;

	ret = reset_control_deassert(res->core);
	if (ret) {
		dev_err(dev, "cannot deassert core reset\n");
		return ret;
	}

	ret = clk_prepare_enable(res->aux);
	if (ret) {
		dev_err(dev, "cannot prepare/enable aux clock\n");
		goto err_res;
	}

	ret = clk_prepare_enable(res->iface);
	if (ret) {
		dev_err(dev, "cannot prepare/enable iface clock\n");
		goto err_aux;
	}

	ret = clk_prepare_enable(res->master_bus);
	if (ret) {
		dev_err(dev, "cannot prepare/enable master_bus clock\n");
		goto err_iface;
	}

	ret = clk_prepare_enable(res->slave_bus);
	if (ret) {
		dev_err(dev, "cannot prepare/enable slave_bus clock\n");
		goto err_master;
	}

	ret = regulator_enable(res->vdda);
	if (ret) {
		dev_err(dev, "cannot enable vdda regulator\n");
		goto err_slave;
	}

	/* change DBI base address */
	writel(0, pcie->parf + PCIE20_PARF_DBI_BASE_ADDR);

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		u32 val = readl(pcie->parf + PCIE20_PARF_AXI_MSTR_WR_ADDR_HALT);

		val |= BIT(31);
		writel(val, pcie->parf + PCIE20_PARF_AXI_MSTR_WR_ADDR_HALT);
	}

	return 0;
err_slave:
	clk_disable_unprepare(res->slave_bus);
err_master:
	clk_disable_unprepare(res->master_bus);
err_iface:
	clk_disable_unprepare(res->iface);
err_aux:
	clk_disable_unprepare(res->aux);
err_res:
	reset_control_assert(res->core);

	return ret;
}

static int qcom_pcie_link_up(struct pcie_port *pp)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pp);
	u16 val = readw(pcie->pp.dbi_base + PCIE20_CAP + PCI_EXP_LNKSTA);

	return !!(val & PCI_EXP_LNKSTA_DLLLA);
}

static void qcom_pcie_host_init(struct pcie_port *pp)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pp);
	int ret;

	qcom_ep_reset_assert(pcie);

	ret = pcie->ops->init(pcie);
	if (ret)
		goto err_deinit;

	ret = phy_power_on(pcie->phy);
	if (ret)
		goto err_deinit;

	dw_pcie_setup_rc(pp);

	if (IS_ENABLED(CONFIG_PCI_MSI))
		dw_pcie_msi_init(pp);

	qcom_ep_reset_deassert(pcie);

	ret = qcom_pcie_establish_link(pcie);
	if (ret)
		goto err;

	if (pcie->ops->init)
		pcie->ops->init(pcie);

	return;
err:
	qcom_ep_reset_assert(pcie);
	phy_power_off(pcie->phy);
err_deinit:
	pcie->ops->deinit(pcie);
}

static int qcom_pcie_rd_own_conf(struct pcie_port *pp, int where, int size,
				 u32 *val)
{
	/* the device class is not reported correctly from the register */
	if (where == PCI_CLASS_REVISION && size == 4) {
		*val = readl(pp->dbi_base + PCI_CLASS_REVISION);
		*val &= 0xff;	/* keep revision id */
		*val |= PCI_CLASS_BRIDGE_PCI << 16;
		return PCIBIOS_SUCCESSFUL;
	}

	return dw_pcie_cfg_read(pp->dbi_base + where, size, val);
}

static struct pcie_host_ops qcom_pcie_dw_ops = {
	.link_up = qcom_pcie_link_up,
	.host_init = qcom_pcie_host_init,
	.rd_own_conf = qcom_pcie_rd_own_conf,
};

static const struct qcom_pcie_ops ops_v0 = {
	.get_resources = qcom_pcie_get_resources_v0,
	.init = qcom_pcie_init_v0,
	.configure = qcom_pcie_configure_v0,
	.deinit = qcom_pcie_deinit_v0,
};

static const struct qcom_pcie_ops ops_v1 = {
	.get_resources = qcom_pcie_get_resources_v1,
	.init = qcom_pcie_init_v1,
	.deinit = qcom_pcie_deinit_v1,
};

static int qcom_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct qcom_pcie *pcie;
	struct pcie_port *pp;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pp = &pcie->pp;
	pcie->ops = (struct qcom_pcie_ops *)of_device_get_match_data(dev);

	pcie->reset = devm_gpiod_get_optional(dev, "perst", GPIOD_OUT_LOW);
	if (IS_ERR(pcie->reset))
		return PTR_ERR(pcie->reset);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "parf");
	pcie->parf = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->parf))
		return PTR_ERR(pcie->parf);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	pp->dbi_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pp->dbi_base))
		return PTR_ERR(pp->dbi_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	pcie->elbi = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->elbi))
		return PTR_ERR(pcie->elbi);

	pcie->phy = devm_phy_optional_get(dev, "pciephy");
	if (IS_ERR(pcie->phy))
		return PTR_ERR(pcie->phy);

	pp->dev = dev;
	ret = pcie->ops->get_resources(pcie);
	if (ret)
		return ret;

	pp->root_bus_nr = -1;
	pp->ops = &qcom_pcie_dw_ops;

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq_byname(pdev, "msi");
		if (pp->msi_irq < 0)
			return pp->msi_irq;

		ret = devm_request_irq(dev, pp->msi_irq,
				       qcom_pcie_msi_irq_handler,
				       IRQF_SHARED, "qcom-pcie-msi", pp);
		if (ret) {
			dev_err(dev, "cannot request msi irq\n");
			return ret;
		}
	}

	ret = phy_init(pcie->phy);
	if (ret)
		return ret;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "cannot initialize host\n");
		return ret;
	}

	return 0;
}

static void qcom_pcie_fixup_final(struct pci_dev *dev)
{
	int cap, err;
	u16 ctl, reg_val;

	cap = pci_pcie_cap(dev);
	if (!cap)
		return;

	err = pci_read_config_word(dev, cap + PCI_EXP_DEVCTL, &ctl);

	if (err)
		return;

	reg_val = ctl;

	if (((reg_val & PCIE20_MRRS_MASK) >> 12) > 1)
		reg_val = (reg_val & ~(PCIE20_MRRS_MASK)) | PCIE20_MRRS(0x1);

	if (((ctl & PCIE20_MPS_MASK) >> 5) > 1)
		reg_val = (reg_val & ~(PCIE20_MPS_MASK)) | PCIE20_MPS(0x1);

	err = pci_write_config_word(dev, cap + PCI_EXP_DEVCTL, reg_val);

	if (err)
		pr_err("pcie config write failed %d\n", err);
}
DECLARE_PCI_FIXUP_FINAL(PCI_ANY_ID, PCI_ANY_ID, qcom_pcie_fixup_final);

static const struct of_device_id qcom_pcie_match[] = {
	{ .compatible = "qcom,pcie-ipq8064", .data = &ops_v0 },
	{ .compatible = "qcom,pcie-apq8064", .data = &ops_v0 },
	{ .compatible = "qcom,pcie-apq8084", .data = &ops_v1 },
	{ }
};

static struct platform_driver qcom_pcie_driver = {
	.probe = qcom_pcie_probe,
	.driver = {
		.name = "qcom-pcie",
		.suppress_bind_attrs = true,
		.of_match_table = qcom_pcie_match,
	},
};
builtin_platform_driver(qcom_pcie_driver);
