// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/util_macros.h>

#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_snps_phy.h"

/**
 * DOC: Synopsis PHY support
 *
 * Synopsis PHYs are primarily programmed by looking up magic register values
 * in tables rather than calculating the necessary values at runtime.
 *
 * Of special note is that the SNPS PHYs include a dedicated port PLL, known as
 * an "MPLLB."  The MPLLB replaces the shared DPLL functionality used on other
 * platforms and must be programming directly during the modeset sequence
 * since it is not handled by the shared DPLL framework as on other platforms.
 */

void intel_snps_phy_wait_for_calibration(struct drm_i915_private *dev_priv)
{
	enum phy phy;

	for_each_phy_masked(phy, ~0) {
		if (!intel_phy_is_snps(dev_priv, phy))
			continue;

		if (intel_de_wait_for_clear(dev_priv, ICL_PHY_MISC(phy),
					    DG2_PHY_DP_TX_ACK_MASK, 25))
			DRM_ERROR("SNPS PHY %c failed to calibrate after 25ms.\n",
				  phy_name(phy));
	}
}

void intel_snps_phy_update_psr_power_state(struct drm_i915_private *dev_priv,
					   enum phy phy, bool enable)
{
	u32 val;

	if (!intel_phy_is_snps(dev_priv, phy))
		return;

	val = REG_FIELD_PREP(SNPS_PHY_TX_REQ_LN_DIS_PWR_STATE_PSR,
			     enable ? 2 : 3);
	intel_uncore_rmw(&dev_priv->uncore, SNPS_PHY_TX_REQ(phy),
			 SNPS_PHY_TX_REQ_LN_DIS_PWR_STATE_PSR, val);
}

static const u32 dg2_trans[] = {
	/* VS 0, pre-emph 0 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 26),

	/* VS 0, pre-emph 1 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 33) |
		REG_FIELD_PREP(SNPS_PHY_TX_EQ_POST, 6),

	/* VS 0, pre-emph 2 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 38) |
		REG_FIELD_PREP(SNPS_PHY_TX_EQ_POST, 12),

	/* VS 0, pre-emph 3 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 43) |
		REG_FIELD_PREP(SNPS_PHY_TX_EQ_POST, 19),

	/* VS 1, pre-emph 0 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 39),

	/* VS 1, pre-emph 1 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 44) |
		REG_FIELD_PREP(SNPS_PHY_TX_EQ_POST, 8),

	/* VS 1, pre-emph 2 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 47) |
		REG_FIELD_PREP(SNPS_PHY_TX_EQ_POST, 15),

	/* VS 2, pre-emph 0 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 52),

	/* VS 2, pre-emph 1 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 51) |
		REG_FIELD_PREP(SNPS_PHY_TX_EQ_POST, 10),

	/* VS 3, pre-emph 0 */
	REG_FIELD_PREP(SNPS_PHY_TX_EQ_MAIN, 62),
};

void intel_snps_phy_ddi_vswing_sequence(struct intel_encoder *encoder,
					u32 level)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(dev_priv, encoder->port);
	int n_entries, ln;

	n_entries = ARRAY_SIZE(dg2_trans);
	if (level >= n_entries)
		level = n_entries - 1;

	for (ln = 0; ln < 4; ln++)
		intel_de_write(dev_priv, SNPS_PHY_TX_EQ(ln, phy),
			       dg2_trans[level]);
}

/*
 * Basic DP link rates with 100 MHz reference clock.
 */

static const struct intel_mpllb_state dg2_dp_rbr_100 = {
	.clock = 162000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 20) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 226),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 39321) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 3),
};

static const struct intel_mpllb_state dg2_dp_hbr1_100 = {
	.clock = 270000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 20) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 3),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 184),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 1),
};

static const struct intel_mpllb_state dg2_dp_hbr2_100 = {
	.clock = 540000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 20) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 3),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 184),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 1),
};

static const struct intel_mpllb_state dg2_dp_hbr3_100 = {
	.clock = 810000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 19) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 292),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 1),
};

static const struct intel_mpllb_state *dg2_dp_100_tables[] = {
	&dg2_dp_rbr_100,
	&dg2_dp_hbr1_100,
	&dg2_dp_hbr2_100,
	&dg2_dp_hbr3_100,
	NULL,
};

/*
 * Basic DP link rates with 38.4 MHz reference clock.
 */

static const struct intel_mpllb_state dg2_dp_rbr_38_4 = {
	.clock = 162000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 1),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 5) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 25) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 304),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 1),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 49152),
};

static const struct intel_mpllb_state dg2_dp_hbr1_38_4 = {
	.clock = 270000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 1),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 5) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 25) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 3),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 248),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 1),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 40960),
};

static const struct intel_mpllb_state dg2_dp_hbr2_38_4 = {
	.clock = 540000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 1),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 5) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 25) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 3),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 248),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 1),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 40960),
};

static const struct intel_mpllb_state dg2_dp_hbr3_38_4 = {
	.clock = 810000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 1),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 6) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 26) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 388),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 1),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 61440),
};

static const struct intel_mpllb_state *dg2_dp_38_4_tables[] = {
	&dg2_dp_rbr_38_4,
	&dg2_dp_hbr1_38_4,
	&dg2_dp_hbr2_38_4,
	&dg2_dp_hbr3_38_4,
	NULL,
};

/*
 * eDP link rates with 100 MHz reference clock.
 */

static const struct intel_mpllb_state dg2_edp_r216 = {
	.clock = 216000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 19) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 312),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 52428) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 4),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_PEAK, 50961),
	.mpllb_sscstep =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_STEPSIZE, 65752),
};

static const struct intel_mpllb_state dg2_edp_r243 = {
	.clock = 243000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 20) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 356),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 26214) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 2),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_PEAK, 57331),
	.mpllb_sscstep =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_STEPSIZE, 73971),
};

static const struct intel_mpllb_state dg2_edp_r324 = {
	.clock = 324000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 20) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 226),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 39321) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 3),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_PEAK, 38221),
	.mpllb_sscstep =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_STEPSIZE, 49314),
};

static const struct intel_mpllb_state dg2_edp_r432 = {
	.clock = 432000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 19) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 65) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 127),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 312),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 52428) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 4),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_PEAK, 50961),
	.mpllb_sscstep =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_STEPSIZE, 65752),
};

static const struct intel_mpllb_state *dg2_edp_tables[] = {
	&dg2_dp_rbr_100,
	&dg2_edp_r216,
	&dg2_edp_r243,
	&dg2_dp_hbr1_100,
	&dg2_edp_r324,
	&dg2_edp_r432,
	&dg2_dp_hbr2_100,
	&dg2_dp_hbr3_100,
	NULL,
};

/*
 * HDMI link rates with 100 MHz reference clock.
 */

static const struct intel_mpllb_state dg2_hdmi_25_175 = {
	.clock = 25175,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 5) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 15) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 64) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 124),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 5) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 128) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_HDMI_DIV, 1),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 143),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 36663) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 71),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_UP_SPREAD, 1),
};

static const struct intel_mpllb_state dg2_hdmi_27_0 = {
	.clock = 27000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 5) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 15) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 64) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 124),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 5) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 140) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_HDMI_DIV, 1),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 26214) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 2),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_UP_SPREAD, 1),
};

static const struct intel_mpllb_state dg2_hdmi_74_25 = {
	.clock = 74250,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 15) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 64) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 124),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 3) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 3),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 86) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_HDMI_DIV, 1),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 26214) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 2),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_UP_SPREAD, 1),
};

static const struct intel_mpllb_state dg2_hdmi_148_5 = {
	.clock = 148500,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 15) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 64) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 124),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 3),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 86) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_HDMI_DIV, 1),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 26214) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 2),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_UP_SPREAD, 1),
};

static const struct intel_mpllb_state dg2_hdmi_594 = {
	.clock = 594000,
	.ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, 3),
	.mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, 4) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, 15) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, 64) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, 124),
	.mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, 2) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, 3),
	.mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, 86) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_HDMI_DIV, 1),
	.mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, 5),
	.mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, 26214) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, 2),
	.mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_UP_SPREAD, 1),
};

static const struct intel_mpllb_state *dg2_hdmi_tables[] = {
	&dg2_hdmi_25_175,
	&dg2_hdmi_27_0,
	&dg2_hdmi_74_25,
	&dg2_hdmi_148_5,
	&dg2_hdmi_594,
	NULL,
};

static const struct intel_mpllb_state **
intel_mpllb_tables_get(struct intel_crtc_state *crtc_state,
		       struct intel_encoder *encoder)
{
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP)) {
		return dg2_edp_tables;
	} else if (intel_crtc_has_dp_encoder(crtc_state)) {
		/*
		 * FIXME: Initially we're just enabling the "combo" outputs on
		 * port A-D.  The MPLLB for those ports takes an input from the
		 * "Display Filter PLL" which always has an output frequency
		 * of 100 MHz, hence the use of the _100 tables below.
		 *
		 * Once we enable port TC1 it will either use the same 100 MHz
		 * "Display Filter PLL" (when strapped to support a native
		 * display connection) or different 38.4 MHz "Filter PLL" when
		 * strapped to support a USB connection, so we'll need to check
		 * that to determine which table to use.
		 */
		if (0)
			return dg2_dp_38_4_tables;
		else
			return dg2_dp_100_tables;
	} else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI)) {
		return dg2_hdmi_tables;
	}

	MISSING_CASE(encoder->type);
	return NULL;
}

int intel_mpllb_calc_state(struct intel_crtc_state *crtc_state,
			   struct intel_encoder *encoder)
{
	const struct intel_mpllb_state **tables;
	int i;

	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI)) {
		if (intel_snps_phy_check_hdmi_link_rate(crtc_state->port_clock)
		    != MODE_OK) {
			/*
			 * FIXME: Can only support fixed HDMI frequencies
			 * until we have a proper algorithm under a valid
			 * license.
			 */
			DRM_DEBUG_KMS("Can't support HDMI link rate %d\n",
				      crtc_state->port_clock);
			return -EINVAL;
		}
	}

	tables = intel_mpllb_tables_get(crtc_state, encoder);
	if (!tables)
		return -EINVAL;

	for (i = 0; tables[i]; i++) {
		if (crtc_state->port_clock <= tables[i]->clock) {
			crtc_state->mpllb_state = *tables[i];
			return 0;
		}
	}

	return -EINVAL;
}

void intel_mpllb_enable(struct intel_encoder *encoder,
			const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	const struct intel_mpllb_state *pll_state = &crtc_state->mpllb_state;
	enum phy phy = intel_port_to_phy(dev_priv, encoder->port);
	i915_reg_t enable_reg = (phy <= PHY_D ?
				 DG2_PLL_ENABLE(phy) : MG_PLL_ENABLE(0));

	/*
	 * 3. Software programs the following PLL registers for the desired
	 * frequency.
	 */
	intel_de_write(dev_priv, SNPS_PHY_MPLLB_CP(phy), pll_state->mpllb_cp);
	intel_de_write(dev_priv, SNPS_PHY_MPLLB_DIV(phy), pll_state->mpllb_div);
	intel_de_write(dev_priv, SNPS_PHY_MPLLB_DIV2(phy), pll_state->mpllb_div2);
	intel_de_write(dev_priv, SNPS_PHY_MPLLB_SSCEN(phy), pll_state->mpllb_sscen);
	intel_de_write(dev_priv, SNPS_PHY_MPLLB_SSCSTEP(phy), pll_state->mpllb_sscstep);
	intel_de_write(dev_priv, SNPS_PHY_MPLLB_FRACN1(phy), pll_state->mpllb_fracn1);
	intel_de_write(dev_priv, SNPS_PHY_MPLLB_FRACN2(phy), pll_state->mpllb_fracn2);

	/*
	 * 4. If the frequency will result in a change to the voltage
	 * requirement, follow the Display Voltage Frequency Switching -
	 * Sequence Before Frequency Change.
	 *
	 * We handle this step in bxt_set_cdclk().
	 */

	/* 5. Software sets DPLL_ENABLE [PLL Enable] to "1". */
	intel_uncore_rmw(&dev_priv->uncore, enable_reg, 0, PLL_ENABLE);

	/*
	 * 9. Software sets SNPS_PHY_MPLLB_DIV dp_mpllb_force_en to "1". This
	 * will keep the PLL running during the DDI lane programming and any
	 * typeC DP cable disconnect. Do not set the force before enabling the
	 * PLL because that will start the PLL before it has sampled the
	 * divider values.
	 */
	intel_de_write(dev_priv, SNPS_PHY_MPLLB_DIV(phy),
		       pll_state->mpllb_div | SNPS_PHY_MPLLB_FORCE_EN);

	/*
	 * 10. Software polls on register DPLL_ENABLE [PLL Lock] to confirm PLL
	 * is locked at new settings. This register bit is sampling PHY
	 * dp_mpllb_state interface signal.
	 */
	if (intel_de_wait_for_set(dev_priv, enable_reg, PLL_LOCK, 5))
		DRM_ERROR("Port %c PLL not locked\n", phy_name(phy));

	/*
	 * 11. If the frequency will result in a change to the voltage
	 * requirement, follow the Display Voltage Frequency Switching -
	 * Sequence After Frequency Change.
	 *
	 * We handle this step in bxt_set_cdclk().
	 */
}

void intel_mpllb_disable(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(dev_priv, encoder->port);
	i915_reg_t enable_reg = (phy <= PHY_D ?
				 DG2_PLL_ENABLE(phy) : MG_PLL_ENABLE(0));

	/*
	 * 1. If the frequency will result in a change to the voltage
	 * requirement, follow the Display Voltage Frequency Switching -
	 * Sequence Before Frequency Change.
	 *
	 * We handle this step in bxt_set_cdclk().
	 */

	/* 2. Software programs DPLL_ENABLE [PLL Enable] to "0" */
	intel_uncore_rmw(&dev_priv->uncore, enable_reg, PLL_ENABLE, 0);

	/*
	 * 4. Software programs SNPS_PHY_MPLLB_DIV dp_mpllb_force_en to "0".
	 * This will allow the PLL to stop running.
	 */
	intel_uncore_rmw(&dev_priv->uncore, SNPS_PHY_MPLLB_DIV(phy),
			 SNPS_PHY_MPLLB_FORCE_EN, 0);

	/*
	 * 5. Software polls DPLL_ENABLE [PLL Lock] for PHY acknowledgment
	 * (dp_txX_ack) that the new transmitter setting request is completed.
	 */
	if (intel_de_wait_for_clear(dev_priv, enable_reg, PLL_LOCK, 5))
		DRM_ERROR("Port %c PLL not locked\n", phy_name(phy));

	/*
	 * 6. If the frequency will result in a change to the voltage
	 * requirement, follow the Display Voltage Frequency Switching -
	 * Sequence After Frequency Change.
	 *
	 * We handle this step in bxt_set_cdclk().
	 */
}

int intel_mpllb_calc_port_clock(struct intel_encoder *encoder,
				const struct intel_mpllb_state *pll_state)
{
	unsigned int frac_quot = 0, frac_rem = 0, frac_den = 1;
	unsigned int multiplier, tx_clk_div, refclk;
	bool frac_en;

	if (0)
		refclk = 38400;
	else
		refclk = 100000;

	refclk >>= REG_FIELD_GET(SNPS_PHY_MPLLB_REF_CLK_DIV, pll_state->mpllb_div2) - 1;

	frac_en = REG_FIELD_GET(SNPS_PHY_MPLLB_FRACN_EN, pll_state->mpllb_fracn1);

	if (frac_en) {
		frac_quot = REG_FIELD_GET(SNPS_PHY_MPLLB_FRACN_QUOT, pll_state->mpllb_fracn2);
		frac_rem = REG_FIELD_GET(SNPS_PHY_MPLLB_FRACN_REM, pll_state->mpllb_fracn2);
		frac_den = REG_FIELD_GET(SNPS_PHY_MPLLB_FRACN_DEN, pll_state->mpllb_fracn1);
	}

	multiplier = REG_FIELD_GET(SNPS_PHY_MPLLB_MULTIPLIER, pll_state->mpllb_div2) / 2 + 16;

	tx_clk_div = REG_FIELD_GET(SNPS_PHY_MPLLB_TX_CLK_DIV, pll_state->mpllb_div);

	return DIV_ROUND_CLOSEST_ULL(mul_u32_u32(refclk, (multiplier << 16) + frac_quot) +
				     DIV_ROUND_CLOSEST(refclk * frac_rem, frac_den),
				     10 << (tx_clk_div + 16));
}

void intel_mpllb_readout_hw_state(struct intel_encoder *encoder,
				  struct intel_mpllb_state *pll_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(dev_priv, encoder->port);

	pll_state->mpllb_cp = intel_de_read(dev_priv, SNPS_PHY_MPLLB_CP(phy));
	pll_state->mpllb_div = intel_de_read(dev_priv, SNPS_PHY_MPLLB_DIV(phy));
	pll_state->mpllb_div2 = intel_de_read(dev_priv, SNPS_PHY_MPLLB_DIV2(phy));
	pll_state->mpllb_sscen = intel_de_read(dev_priv, SNPS_PHY_MPLLB_SSCEN(phy));
	pll_state->mpllb_sscstep = intel_de_read(dev_priv, SNPS_PHY_MPLLB_SSCSTEP(phy));
	pll_state->mpllb_fracn1 = intel_de_read(dev_priv, SNPS_PHY_MPLLB_FRACN1(phy));
	pll_state->mpllb_fracn2 = intel_de_read(dev_priv, SNPS_PHY_MPLLB_FRACN2(phy));

	/*
	 * REF_CONTROL is under firmware control and never programmed by the
	 * driver; we read it only for sanity checking purposes.  The bspec
	 * only tells us the expected value for one field in this register,
	 * so we'll only read out those specific bits here.
	 */
	pll_state->ref_control = intel_de_read(dev_priv, SNPS_PHY_REF_CONTROL(phy)) &
		SNPS_PHY_REF_CONTROL_REF_RANGE;

	/*
	 * MPLLB_DIV is programmed twice, once with the software-computed
	 * state, then again with the MPLLB_FORCE_EN bit added.  Drop that
	 * extra bit during readout so that we return the actual expected
	 * software state.
	 */
	pll_state->mpllb_div &= ~SNPS_PHY_MPLLB_FORCE_EN;
}

int intel_snps_phy_check_hdmi_link_rate(int clock)
{
	const struct intel_mpllb_state **tables = dg2_hdmi_tables;
	int i;

	for (i = 0; tables[i]; i++) {
		if (clock == tables[i]->clock)
			return MODE_OK;
	}

	return MODE_CLOCK_RANGE;
}
