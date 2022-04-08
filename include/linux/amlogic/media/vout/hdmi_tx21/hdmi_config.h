/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __PLAT_MESON_HDMI_CONFIG_H
#define __PLAT_MESON_HDMI_CONFIG_H

struct hdmi_phy_set_data {
	unsigned long freq;
	unsigned long addr;
	unsigned long data;
};

struct vendor_info_data {
	u8 *vendor_name; /* Max Chars: 8 */
	/* vendor_id, 3 Bytes, Refer to
	 * http://standards.ieee.org/develop/regauth/oui/oui.txt
	 */
	u8 *product_desc; /* Max Chars: 16 */
	u8 *cec_osd_string; /* Max Chars: 14 */
	u32 cec_config; /* 4 bytes: use to control cec switch on/off */
	u32 vendor_id;
};

enum pwr_type {
	NONE = 0, CPU_GPO = 1, PMU,
};

struct pwr_cpu_gpo {
	u32 pin;
	u32 val;
};

struct pwr_pmu {
	u32 pin;
	u32 val;
};

struct pwr_ctl_var {
	enum pwr_type type;
	union {
		struct pwr_cpu_gpo gpo;
		struct pwr_pmu pmu;
	} var;
};

struct hdmi_pwr_ctl {
	struct pwr_ctl_var pwr_5v_on;
	struct pwr_ctl_var pwr_5v_off;
	struct pwr_ctl_var pwr_3v3_on;
	struct pwr_ctl_var pwr_3v3_off;
	struct pwr_ctl_var pwr_hpll_vdd_on;
	struct pwr_ctl_var pwr_hpll_vdd_off;
	int pwr_level;
};

struct hdmi_config_platform_data {
	void (*hdmi_sspll_ctrl)(u32 level); /* SSPLL control level */
	/* For some boards, HDMI PHY setting may diff from ref board. */
	struct hdmi_phy_set_data *phy_data;
	struct vendor_info_data *vend_data;
	struct hdmi_pwr_ctl *pwr_ctl;
};

#endif /* __PLAT_MESON_HDMI_CONFIG_H */
