// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 */

#include <linux/of_device.h>
#include <linux/qcom_scm.h>

#include "arm-smmu.h"

struct qcom_smmu {
	struct arm_smmu_device smmu;
};

static const struct of_device_id qcom_smmu_client_of_match[] __maybe_unused = {
	{ .compatible = "qcom,adreno" },
	{ .compatible = "qcom,mdp4" },
	{ .compatible = "qcom,mdss" },
	{ .compatible = "qcom,sc7180-mdss" },
	{ .compatible = "qcom,sc7180-mss-pil" },
	{ .compatible = "qcom,sdm845-mdss" },
	{ .compatible = "qcom,sdm845-mss-pil" },
	{ }
};

static int qcom_smmu_cfg_probe(struct arm_smmu_device *smmu)
{
	unsigned int last_s2cr = ARM_SMMU_GR0_S2CR(smmu->num_mapping_groups - 1);
	u32 smr;
	u32 reg;
	int i;

	/*
	 * With some firmware writes to S2CR of type FAULT are ignored, and
	 * writing BYPASS will end up as FAULT in the register. Perform a write
	 * to S2CR to detect if this is the case with the current firmware.
	 */
	arm_smmu_gr0_write(smmu, last_s2cr, FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_BYPASS) |
					    FIELD_PREP(ARM_SMMU_S2CR_CBNDX, 0xff) |
					    FIELD_PREP(ARM_SMMU_S2CR_PRIVCFG, S2CR_PRIVCFG_DEFAULT));
	reg = arm_smmu_gr0_read(smmu, last_s2cr);
	if (FIELD_GET(ARM_SMMU_S2CR_TYPE, reg) != S2CR_TYPE_BYPASS) {
		smmu->qcom_bypass_quirk = true;

		/*
		 * With firmware ignoring writes of type FAULT, booting the
		 * Linux kernel with disable_bypass disabled (i.e. "enable
		 * bypass") the initialization during probe will leave mappings
		 * in an inconsistent state. Avoid this by configuring all
		 * S2CRs to BYPASS.
		 */
		for (i = 0; i < smmu->num_mapping_groups; i++) {
			smmu->s2crs[i].type = S2CR_TYPE_BYPASS;
			smmu->s2crs[i].privcfg = S2CR_PRIVCFG_DEFAULT;
			smmu->s2crs[i].cbndx = 0xff;
			smmu->s2crs[i].count = 0;
		}
	}

	for (i = 0; i < smmu->num_mapping_groups; i++) {
		smr = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));

		if (FIELD_GET(ARM_SMMU_SMR_VALID, smr)) {
			smmu->smrs[i].id = FIELD_GET(ARM_SMMU_SMR_ID, smr);
			smmu->smrs[i].mask = FIELD_GET(ARM_SMMU_SMR_MASK, smr);
			smmu->smrs[i].valid = true;
		}
	}

	return 0;
}

static int qcom_smmu_def_domain_type(struct device *dev)
{
	const struct of_device_id *match =
		of_match_device(qcom_smmu_client_of_match, dev);

	return match ? IOMMU_DOMAIN_IDENTITY : 0;
}

static int qcom_sdm845_smmu500_reset(struct arm_smmu_device *smmu)
{
	int ret;

	/*
	 * To address performance degradation in non-real time clients,
	 * such as USB and UFS, turn off wait-for-safe on sdm845 based boards,
	 * such as MTP and db845, whose firmwares implement secure monitor
	 * call handlers to turn on/off the wait-for-safe logic.
	 */
	ret = qcom_scm_qsmmu500_wait_safe_toggle(0);
	if (ret)
		dev_warn(smmu->dev, "Failed to turn off SAFE logic\n");

	return ret;
}

static int qcom_smmu500_reset(struct arm_smmu_device *smmu)
{
	const struct device_node *np = smmu->dev->of_node;

	arm_mmu500_reset(smmu);

	if (of_device_is_compatible(np, "qcom,sdm845-smmu-500"))
		return qcom_sdm845_smmu500_reset(smmu);

	return 0;
}

static const struct arm_smmu_impl qcom_smmu_impl = {
	.cfg_probe = qcom_smmu_cfg_probe,
	.def_domain_type = qcom_smmu_def_domain_type,
	.reset = qcom_smmu500_reset,
};

struct arm_smmu_device *qcom_smmu_impl_init(struct arm_smmu_device *smmu)
{
	struct qcom_smmu *qsmmu;

	qsmmu = devm_kzalloc(smmu->dev, sizeof(*qsmmu), GFP_KERNEL);
	if (!qsmmu)
		return ERR_PTR(-ENOMEM);

	qsmmu->smmu = *smmu;

	qsmmu->smmu.impl = &qcom_smmu_impl;
	devm_kfree(smmu->dev, smmu);

	return &qsmmu->smmu;
}
