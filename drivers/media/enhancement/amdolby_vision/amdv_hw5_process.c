// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/amlogic/media/video_sink/video.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/registers/cpu_version.h>
#ifdef CONFIG_AMLOGIC_MEDIA_ENHANCEMENT_VECM
#include <linux/amlogic/media/amvecm/amvecm.h>
#endif
#include <linux/amlogic/media/vout/vout_notify.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include "amdv_regs_hw5.h"
#include "../amvecm/arch/vpp_regs.h"
#include "../amvecm/arch/vpp_hdr_regs.h"
#include "../amvecm/arch/vpp_dolbyvision_regs.h"
#include "../amvecm/amcsc.h"
#include "../amvecm/reg_helper.h"
#include <linux/amlogic/media/registers/regs/viu_regs.h>
#include <linux/amlogic/media/amdolbyvision/dolby_vision.h>
#include <linux/amlogic/media/vpu/vpu.h>
#include <linux/dma-map-ops.h>
#include <linux/amlogic/iomap.h>
#include "md_config.h"
#include <linux/of.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "amdv.h"

static struct dv_atsc p_atsc_md;

u32 hw5_reg_from_file;
module_param(hw5_reg_from_file, uint, 0664);
MODULE_PARM_DESC(hw5_reg_from_file, "\n hw5_reg_from_file\n");

u32 force_update_top2 = true;
module_param(force_update_top2, uint, 0664);
MODULE_PARM_DESC(force_update_top2, "\n force_update_top2\n");

#define signal_cuva ((vf->signal_type >> 31) & 1)
#define signal_color_primaries ((vf->signal_type >> 16) & 0xff)
#define signal_transfer_characteristic ((vf->signal_type >> 8) & 0xff)

static bool prepare_parser(int reset_flag)
{
	bool parser_ready = false;

	if (!dv_inst[0].metadata_parser) {
		if (p_funcs_tv && p_funcs_tv->multi_mp_init) {
			dv_inst[0].metadata_parser =
			p_funcs_tv->multi_mp_init
				(dolby_vision_flags
				 & FLAG_CHANGE_SEQ_HEAD
				 ? 1 : 0);
			p_funcs_tv->multi_mp_reset(dv_inst[0].metadata_parser, 1);
		} else {
			pr_dv_dbg("p_funcs is null\n");
		}
		if (dv_inst[0].metadata_parser) {
			parser_ready = true;
			if (debug_dolby & 1)
				pr_dv_dbg("metadata parser init OK\n");
		}
	} else {
		//} else if (p_funcs_tv && p_funcs_tv->multi_mp_reset) {
		//	if (p_funcs_tv->multi_mp_reset
		//	    (dv_inst[0].metadata_parser,
		//		reset_flag | metadata_parser_reset_flag) == 0)
		//		metadata_parser_reset_flag = 0;*/ //no need
	}
	return parser_ready;
}

void update_src_format_hw5(enum signal_format_enum src_format, struct vframe_s *vf)
{
	enum signal_format_enum cur_format = dv_inst[0].amdv_src_format;

	if (src_format == FORMAT_DOVI ||
		src_format == FORMAT_DOVI_LL) {
		dv_inst[0].amdv_src_format = 3;
	} else {
		if (vf) {
			if (is_cuva_frame(vf)) {
				if ((signal_transfer_characteristic == 14 ||
				     signal_transfer_characteristic == 18) &&
				    signal_color_primaries == 9)
					dv_inst[0].amdv_src_format = 9;
				else if (signal_transfer_characteristic == 16)
					dv_inst[0].amdv_src_format = 8;
			} else if (is_primesl_frame(vf)) {
				/* need check prime_sl before hdr and sdr */
				dv_inst[0].amdv_src_format = 4;
			} else if (vf_is_hdr10_plus(vf)) {
				dv_inst[0].amdv_src_format = 2;
			} else if (vf_is_hdr10(vf)) {
				dv_inst[0].amdv_src_format = 1;
			} else if (vf_is_hlg(vf)) {
				dv_inst[0].amdv_src_format = 5;
			} else if (is_mvc_frame(vf)) {
				dv_inst[0].amdv_src_format = 7;
			} else {
				dv_inst[0].amdv_src_format = 6;
			}
		}
	}
	if (cur_format != dv_inst[0].amdv_src_format) {
		update_pwm_control();
		pr_dv_dbg
		("update src fmt: %s => %s, signal_type 0x%x, src fmt %d\n",
		input_str[cur_format],
		input_str[dv_inst[0].amdv_src_format],
		vf ? vf->signal_type : 0,
		src_format);
		cur_format = dv_inst[0].amdv_src_format;
	}
}

int parse_sei_and_meta_ext_hw5(struct vframe_s *vf,
					 char *aux_buf,
					 int aux_size,
					 int *total_comp_size,
					 int *total_md_size,
					 void *fmt,
					 int *ret_flags,
					 char *md_buf,
					 char *comp_buf)
{
	int i;
	char *p;
	unsigned int size = 0u;
	unsigned int type = 0;
	int md_size = 0;
	int comp_size = 0;
	int ret = 2;
	bool parser_overflow = false;
	int rpu_ret = 0;
	u32 reset_flag = 0;
	unsigned int rpu_size = 0;
	enum signal_format_enum *src_format = (enum signal_format_enum *)fmt;
	static int parse_process_count;
	char meta_buf[1024];
	static u32 last_play_id;

	if (!aux_buf || aux_size == 0 || !fmt || !md_buf || !comp_buf ||
	    !total_comp_size || !total_md_size || !ret_flags)
		return 1;

	parse_process_count++;
	if (parse_process_count > 1) {
		pr_err("parser not support multi instance\n");
		ret = 1;
		goto parse_err;
	}
	if (!p_funcs_tv) {
		ret = 1;
		goto parse_err;
	}

	/* release metadata_parser when new playing */
	if (vf && vf->src_fmt.play_id != last_play_id) {
		if (dv_inst[0].metadata_parser) {
			if (p_funcs_tv && p_funcs_tv->multi_mp_release)/*idk5.1*/
				p_funcs_tv->multi_mp_release(&dv_inst[0].metadata_parser);

			dv_inst[0].metadata_parser = NULL;
			pr_dv_dbg("new play, release parser\n");
			amdv_clear_buf(0);
		}
		last_play_id = vf->src_fmt.play_id;
		if (debug_dolby & 2)
			pr_dv_dbg("update play id=%d:\n", last_play_id);
	}
	p = aux_buf;
	while (p < aux_buf + aux_size - 8) {
		size = *p++;
		size = (size << 8) | *p++;
		size = (size << 8) | *p++;
		size = (size << 8) | *p++;
		type = *p++;
		type = (type << 8) | *p++;
		type = (type << 8) | *p++;
		type = (type << 8) | *p++;
		if (debug_dolby & 4)
			pr_dv_dbg("metadata type=%08x, size=%d:\n",
				     type, size);
		if (size == 0 || size > aux_size) {
			pr_dv_dbg("invalid aux size %d\n", size);
			ret = 1;
			goto parse_err;
		}
		if (type == DV_SEI || /* hevc t35 sei */
			(type & 0xffff0000) == AV1_SEI) { /* av1 t35 obu */
			*total_comp_size = 0;
			*total_md_size = 0;

			if ((type & 0xffff0000) == AV1_SEI &&
			    p[0] == 0xb5 &&
			    p[1] == 0x00 &&
			    p[2] == 0x3b &&
			    p[3] == 0x00 &&
			    p[4] == 0x00 &&
			    p[5] == 0x08 &&
			    p[6] == 0x00 &&
			    p[7] == 0x37 &&
			    p[8] == 0xcd &&
			    p[9] == 0x08) {
				/* AV1 dv meta in obu */
				*src_format = FORMAT_DOVI;
				meta_buf[0] = 0;
				meta_buf[1] = 0;
				meta_buf[2] = 0;
				meta_buf[3] = 0x01;
				meta_buf[4] = 0x19;
				if (p[11] & 0x10) {
					rpu_size = 0x100;
					rpu_size |= (p[11] & 0x0f) << 4;
					rpu_size |= (p[12] >> 4) & 0x0f;
					if (p[12] & 0x08) {
						pr_dv_error
							("rpu in obu exceed 512 bytes\n");
						break;
					}
					for (i = 0; i < rpu_size; i++) {
						meta_buf[5 + i] =
							(p[12 + i] & 0x07) << 5;
						meta_buf[5 + i] |=
							(p[13 + i] >> 3) & 0x1f;
					}
					rpu_size += 5;
				} else {
					rpu_size = (p[10] & 0x1f) << 3;
					rpu_size |= (p[11] >> 5) & 0x07;
					for (i = 0; i < rpu_size; i++) {
						meta_buf[5 + i] =
							(p[11 + i] & 0x0f) << 4;
						meta_buf[5 + i] |=
							(p[12 + i] >> 4) & 0x0f;
					}
					rpu_size += 5;
				}
			} else {
				/* HEVC dv meta in sei */
				*src_format = FORMAT_DOVI;
				if (size > (sizeof(meta_buf) - 3))
					size = (sizeof(meta_buf) - 3);
				meta_buf[0] = 0;
				meta_buf[1] = 0;
				meta_buf[2] = 0;
				memcpy(&meta_buf[3], p + 1, size - 1);
				rpu_size = size + 2;
			}
			if ((debug_dolby & 4) && dump_enable_f(0)) {
				pr_dv_dbg("metadata(%d):\n", rpu_size);
				for (i = 0; i < size; i += 16)
					pr_info("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
						meta_buf[i],
						meta_buf[i + 1],
						meta_buf[i + 2],
						meta_buf[i + 3],
						meta_buf[i + 4],
						meta_buf[i + 5],
						meta_buf[i + 6],
						meta_buf[i + 7],
						meta_buf[i + 8],
						meta_buf[i + 9],
						meta_buf[i + 10],
						meta_buf[i + 11],
						meta_buf[i + 12],
						meta_buf[i + 13],
						meta_buf[i + 14],
						meta_buf[i + 15]);
			}

			/* prepare metadata parser */
			if (!prepare_parser(reset_flag)) {
				pr_dv_error
				("meta(%d), pts(%lld) -> parser init fail\n",
					rpu_size, vf ? vf->pts_us64 : 0);
				ret = 1;
				goto parse_err;
			}

			md_size = 0;
			comp_size = 0;

			if (p_funcs_tv && p_funcs_tv->multi_mp_process)
				rpu_ret =
				p_funcs_tv->multi_mp_process
				(dv_inst[0].metadata_parser, meta_buf, rpu_size,
				 comp_buf + *total_comp_size,
				 &comp_size, md_buf + *total_md_size,
				 &md_size, true, DV_TYPE_DOVI);
			if (rpu_ret < 0) {
				if (vf)
					pr_dv_error
					("meta(%d), pts(%lld) -> metadata parser process fail\n",
					 rpu_size, vf->pts_us64);
				ret = 3;
			} else {
				if (*total_comp_size + comp_size
					< COMP_BUF_SIZE)
					*total_comp_size += comp_size;
				else
					parser_overflow = true;

				if (*total_md_size + md_size
					< MD_BUF_SIZE)
					*total_md_size += md_size;
				else
					parser_overflow = true;
				if (rpu_ret == 1)
					*ret_flags = 1;
				ret = 0;
			}
			if (parser_overflow) {
				ret = 2;
				break;
			}
			/*dv type just appears once in metadata
			 *after parsing dv type,breaking the
			 *circulation directly
			 */
			break;
		}
		p += size;
	}

	/*continue to check atsc/dvb dv */
	if (*src_format != FORMAT_DOVI) {
		struct dv_vui_parameters vui_param = {0};
		static u32 len_2086_sei;
		u32 len_2094_sei = 0;
		static u8 payload_2086_sei[MAX_LEN_2086_SEI];
		u8 payload_2094_sei[MAX_LEN_2094_SEI];
		unsigned char nal_type;
		unsigned char sei_payload_type = 0;
		unsigned char sei_payload_size = 0;

		if (vf) {
			vui_param.video_fmt_i = (vf->signal_type >> 26) & 7;
			vui_param.video_fullrange_b = (vf->signal_type >> 25) & 1;
			vui_param.color_description_b = (vf->signal_type >> 24) & 1;
			vui_param.color_primaries_i = (vf->signal_type >> 16) & 0xff;
			vui_param.trans_characteristic_i =
							(vf->signal_type >> 8) & 0xff;
			vui_param.matrix_coeff_i = (vf->signal_type) & 0xff;
			if (debug_dolby & 2)
				pr_dv_dbg("vui_param %d, %d, %d, %d, %d, %d\n",
					vui_param.video_fmt_i,
					vui_param.video_fullrange_b,
					vui_param.color_description_b,
					vui_param.color_primaries_i,
					vui_param.trans_characteristic_i,
					vui_param.matrix_coeff_i);
		}
		p = aux_buf;
		if ((debug_dolby & 0x200) && dump_enable_f(0)) {
			pr_dv_dbg("aux_buf(%d):\n", aux_size);
			for (i = 0; i < aux_size; i += 8)
				pr_info("\t%02x %02x %02x %02x %02x %02x %02x %02x\n",
					p[i],
					p[i + 1],
					p[i + 2],
					p[i + 3],
					p[i + 4],
					p[i + 5],
					p[i + 6],
					p[i + 7]);
		}
		while (p < aux_buf + aux_size - 8) {
			size = *p++;
			size = (size << 8) | *p++;
			size = (size << 8) | *p++;
			size = (size << 8) | *p++;
			type = *p++;
			type = (type << 8) | *p++;
			type = (type << 8) | *p++;
			type = (type << 8) | *p++;
			if (debug_dolby & 2)
				pr_dv_dbg("type: 0x%x\n", type);

			/*4 byte size + 4 byte type*/
			/*1 byte nal_type + 1 byte (layer_id+temporal_id)*/
			/*1 byte payload type + 1 byte size + payload data*/
			if (type == 0x02000000) {
				nal_type = ((*p) & 0x7E) >> 1; /*nal unit type*/
				if (debug_dolby & 2)
					pr_dv_dbg("nal_type: %d\n",
						     nal_type);

				if (nal_type == PREFIX_SEI_NUT_NAL ||
					nal_type == SUFFIX_SEI_NUT_NAL) {
					sei_payload_type = *(p + 2);
					sei_payload_size = *(p + 3);
					if (debug_dolby & 2)
						pr_dv_dbg("type %d, size %d\n",
							     sei_payload_type,
							     sei_payload_size);
					if (sei_payload_type ==
					SEI_TYPE_MASTERING_DISP_COLOUR_VOLUME) {
						len_2086_sei =
							sei_payload_size;
						memcpy(payload_2086_sei, p + 4,
						       len_2086_sei);
					} else if (sei_payload_type == SEI_ITU_T_T35 &&
						sei_payload_size >= 8 && check_atsc_dvb(p)) {
						len_2094_sei = sei_payload_size;
						memcpy(payload_2094_sei, p + 4,
						       len_2094_sei);
					}
					if (len_2086_sei > 0 &&
					    len_2094_sei > 0)
						break;
				}
			}
			p += size;
		}
		if (len_2094_sei > 0) {
			/* source is VS10 */
			*total_comp_size = 0;
			*total_md_size = 0;
			*src_format = FORMAT_DOVI;
			p_atsc_md.vui_param = vui_param;
			p_atsc_md.len_2086_sei = len_2086_sei;
			memcpy(p_atsc_md.sei_2086, payload_2086_sei,
			       len_2086_sei);
			p_atsc_md.len_2094_sei = len_2094_sei;
			memcpy(p_atsc_md.sei_2094, payload_2094_sei,
			       len_2094_sei);
			size = sizeof(struct dv_atsc);
			if (size > sizeof(meta_buf))
				size = sizeof(meta_buf);
			memcpy(meta_buf, (unsigned char *)(&p_atsc_md), size);
			if ((debug_dolby & 4) && dump_enable_f(0)) {
				pr_dv_dbg("metadata(%d):\n", size);
				for (i = 0; i < size; i += 8)
					pr_info("\t%02x %02x %02x %02x %02x %02x %02x %02x\n",
						meta_buf[i],
						meta_buf[i + 1],
						meta_buf[i + 2],
						meta_buf[i + 3],
						meta_buf[i + 4],
						meta_buf[i + 5],
						meta_buf[i + 6],
						meta_buf[i + 7]);
			}
			/* prepare metadata parser */
			reset_flag = 2; /*flag: bit0 flag, bit1 0->dv, 1->atsc*/
			if (!prepare_parser(reset_flag)) {
				if (vf)
					pr_dv_error
					("meta(%d), pts(%lld) -> metadata parser init fail\n",
					 size, vf->pts_us64);
				ret = 1;
				goto parse_err;
			}

			md_size = 0;
			comp_size = 0;

			if (p_funcs_tv && p_funcs_tv->multi_mp_process)
				rpu_ret =
				p_funcs_tv->multi_mp_process
				(dv_inst[0].metadata_parser, meta_buf, size,
				comp_buf + *total_comp_size,
				&comp_size, md_buf + *total_md_size,
				&md_size, true, DV_TYPE_ATSC);

			if (rpu_ret < 0) {
				if (vf)
					pr_dv_error
					("meta(%d), pts(%lld) -> metadata parser process fail\n",
					size, vf->pts_us64);
				ret = 3;
			} else {
				if (*total_comp_size + comp_size
					< COMP_BUF_SIZE)
					*total_comp_size += comp_size;
				else
					parser_overflow = true;

				if (*total_md_size + md_size
					< MD_BUF_SIZE)
					*total_md_size += md_size;
				else
					parser_overflow = true;
				if (rpu_ret == 1)
					*ret_flags = 1;
				ret = 0;
			}
			if (parser_overflow)
				ret = 2;
		} else {
			len_2086_sei = 2;
		}
	}

	if (*total_md_size) {
		if ((debug_dolby & 1) && vf)
			pr_dv_dbg
			("meta(%d), pts(%lld) -> md(%d), comp(%d)\n",
			 size, vf->pts_us64,
			 *total_md_size, *total_comp_size);
		if ((debug_dolby & 4) && dump_enable_f(0))  {
			pr_dv_dbg("parsed md(%d):\n", *total_md_size);
			for (i = 0; i < *total_md_size + 7; i += 8) {
				pr_info("%02x %02x %02x %02x %02x %02x %02x %02x\n",
					md_buf[i],
					md_buf[i + 1],
					md_buf[i + 2],
					md_buf[i + 3],
					md_buf[i + 4],
					md_buf[i + 5],
					md_buf[i + 6],
					md_buf[i + 7]);
			}
		}
	}
parse_err:
	parse_process_count--;
	return ret;
}

int amdv_parse_metadata_hw5(struct vframe_s *vf,
					      u8 toggle_mode,
					      bool bypass_release,
					      bool drop_flag)
{
	const struct vinfo_s *vinfo = get_current_vinfo();
	struct provider_aux_req_s req;
	struct provider_aux_req_s el_req;
	int flag;
	enum signal_format_enum src_format = FORMAT_SDR;
	enum signal_format_enum check_format;
	int total_md_size = 0;
	int total_comp_size = 0;
	bool el_flag = 0;
	u32 w = 0xffff;
	u32 h = 0xffff;
	int meta_flag_bl = 1;
	int src_chroma_format = 0;
	int src_bdp = 12;
	bool video_frame = false;
	int i;
	struct vframe_master_display_colour_s *p_mdc;
	unsigned int current_mode = dolby_vision_mode;
	enum input_mode_enum input_mode = IN_MODE_OTT;
	int ret_flags = 0;
	int ret = -1;
	bool mel_flag = false;
	int vsem_size = 0;
	int vsem_if_size = 0;
	bool dump_emp = false;
	bool dv_vsem = false;
	bool hdr10_src_primary_changed = false;
	unsigned long time_use = 0;
	struct timeval start;
	struct timeval end;
	char *pic_mode;
	bool run_control_path = true;
	bool vf_changed = true;
	struct ambient_cfg_s *p_ambient = NULL;
	u32 cur_id = 0;

	if (!p_funcs_tv || !p_funcs_tv->tv_hw5_control_path || !tv_hw5_setting)
		return -1;

	memset(&req, 0, (sizeof(struct provider_aux_req_s)));
	memset(&el_req, 0, (sizeof(struct provider_aux_req_s)));

	if (vf) {
		video_frame = true;
		w = (vf->type & VIDTYPE_COMPRESS) ?
			vf->compWidth : vf->width;
		h = (vf->type & VIDTYPE_COMPRESS) ?
			vf->compHeight : vf->height;
	}

	if (is_aml_tvmode() && vf &&
	    vf->source_type == VFRAME_SOURCE_TYPE_HDMI) {
		req.vf = vf;
		req.bot_flag = 0;
		req.aux_buf = NULL;
		req.aux_size = 0;
		req.dv_enhance_exist = 0;
		req.low_latency = 0;
		vf_notify_provider_by_name("dv_vdin",
					   VFRAME_EVENT_RECEIVER_GET_AUX_DATA,
					   (void *)&req);
		input_mode = IN_MODE_HDMI;

		if ((dolby_vision_flags & FLAG_CERTIFICATION) && enable_vf_check)
			vf_changed = check_vf_changed(vf);

		/* meta */
		if ((dolby_vision_flags & FLAG_RX_EMP_VSEM) &&
			vf->emp.size > 0) {
			vsem_size = vf->emp.size * VSEM_PKT_SIZE;
			memcpy(vsem_if_buf, vf->emp.addr, vsem_size);
			if (vsem_if_buf[0] == 0x7f &&
			    vsem_if_buf[10] == 0x46 &&
			    vsem_if_buf[11] == 0xd0) {
				dv_vsem = true;
				if (!vsem_check(vsem_if_buf, vsem_md_buf)) {
					vsem_if_size = vsem_size;
					if (!vsem_md_buf[10]) {
						req.aux_buf =
							&vsem_md_buf[13];
						req.aux_size =
							(vsem_md_buf[5] << 8)
							+ vsem_md_buf[6]
							- 6 - 4;
						/* cancel vsem, use md */
						/* vsem_if_size = 0; */
					} else {
						req.low_latency = 1;
					}
				} else {
					/* emp error, use previous md */
					pr_dv_dbg("EMP packet error %d\n",
						vf->emp.size);
					dump_emp = true;
					vsem_if_size = 0;
					req.aux_buf = NULL;
					req.aux_size = dv_inst[0].last_total_md_size;
				}
			} else if (debug_dolby & 4) {
				pr_dv_dbg("EMP packet not DV vsem %d\n",
					vf->emp.size);
				dump_emp = true;
			}
			if ((debug_dolby & 4) || dump_emp) {
				pr_info("vsem pkt count = %d\n", vf->emp.size);
				for (i = 0; i < vsem_size; i += 8) {
					pr_info("%02x %02x %02x %02x %02x %02x %02x %02x\n",
						vsem_if_buf[i],	vsem_if_buf[i + 1],
						vsem_if_buf[i + 2],	vsem_if_buf[i + 3],
						vsem_if_buf[i + 4],	vsem_if_buf[i + 5],
						vsem_if_buf[i + 6],	vsem_if_buf[i + 7]);
				}
			}
		}

		/* w/t vsif and no dv_vsem */
		if (vf->vsif.size && !dv_vsem) {
			memset(vsem_if_buf, 0, VSEM_IF_BUF_SIZE);
			memcpy(vsem_if_buf, vf->vsif.addr, vf->vsif.size);
			vsem_if_size = vf->vsif.size;
			if (debug_dolby & 4) {
				pr_info("vsif size = %d\n", vf->vsif.size);
				for (i = 0; i < vsem_if_size; i += 8) {
					pr_info("%02x %02x %02x %02x %02x %02x %02x %02x\n",
						vsem_if_buf[i],	vsem_if_buf[i + 1],
						vsem_if_buf[i + 2],	vsem_if_buf[i + 3],
						vsem_if_buf[i + 4],	vsem_if_buf[i + 5],
						vsem_if_buf[i + 6],	vsem_if_buf[i + 7]);
				}
			}
		}
		if ((debug_dolby & 1) && (dv_vsem || vsem_if_size))
			pr_dv_dbg("vdin get %s:%d, md:%p %d,ll:%d,bit %x,type %x\n",
				dv_vsem ? "vsem" : "vsif",
				dv_vsem ? vsem_size : vsem_if_size,
				req.aux_buf, req.aux_size,
				req.low_latency,
				vf->bitdepth, vf->source_type);

		/*check vsem_if_buf */
		if (vsem_if_size &&
			vsem_if_buf[0] != 0x81) {
			/*not vsif, continue to check vsem*/
			if (!(vsem_if_buf[0] == 0x7F &&
				vsem_if_buf[1] == 0x80 &&
				vsem_if_buf[10] == 0x46 &&
				vsem_if_buf[11] == 0xd0 &&
				vsem_if_buf[12] == 0x00)) {
				vsem_if_size = 0;
				pr_dv_dbg("vsem_if_buf is invalid!\n");
				pr_dv_dbg("%x %x %x %x %x %x %x %x %x %x %x %x\n",
					vsem_if_buf[0], vsem_if_buf[1], vsem_if_buf[2],
					vsem_if_buf[3],	vsem_if_buf[4],	vsem_if_buf[5],
					vsem_if_buf[6],	vsem_if_buf[7],	vsem_if_buf[8],
					vsem_if_buf[9],	vsem_if_buf[10], vsem_if_buf[11]);
			}
		}
		if ((dolby_vision_flags & FLAG_FORCE_DV_LL) ||
		    req.low_latency == 1) {
			src_format = FORMAT_DOVI_LL;
			src_chroma_format = 0;
			for (i = 0; i < 2; i++) {
				if (dv_inst[0].md_buf[i])
					memset(dv_inst[0].md_buf[i], 0, MD_BUF_SIZE);
				if (dv_inst[0].comp_buf[i])
					memset(dv_inst[0].comp_buf[i], 0, COMP_BUF_SIZE);
			}
			req.aux_size = 0;
			req.aux_buf = NULL;
		} else if (req.aux_size) {
			if (req.aux_buf) {
				dv_inst[0].current_id = dv_inst[0].current_id ^ 1;
				memcpy(dv_inst[0].md_buf[dv_inst[0].current_id],
				       req.aux_buf, req.aux_size);
			}
			src_format = FORMAT_DOVI;
		} else {
			if (toggle_mode == 2)
				src_format =  tv_hw5_setting->input.src_format;
			if (vf->type & VIDTYPE_VIU_422)
				src_chroma_format = 1;
			p_mdc =	&vf->prop.master_display_colour;
			if (is_hdr10_frame(vf) || force_hdmin_fmt == 1) {
				src_format = FORMAT_HDR10;
				/* prepare parameter from hdmi for hdr10 */
				p_mdc->luminance[0] *= 10000;
				prepare_hdr10_param
					(p_mdc, &dv_inst[0].hdr10_param);
				req.dv_enhance_exist = 0;
				src_bdp = 12;
			}
			if (src_format != FORMAT_DOVI &&
				(is_hlg_frame(vf) || force_hdmin_fmt == 2)) {
				src_format = FORMAT_HLG;
				src_bdp = 12;
			}
			if (src_format == FORMAT_SDR &&
				!req.dv_enhance_exist)
				src_bdp = 12;
		}
		if ((debug_dolby & 4) && req.aux_size) {
			pr_dv_dbg("metadata(%d):\n", req.aux_size);
			for (i = 0; i < req.aux_size + 8; i += 8)
				pr_info("\t%02x %02x %02x %02x %02x %02x %02x %02x\n",
					dv_inst[0].md_buf[dv_inst[0].current_id][i],
					dv_inst[0].md_buf[dv_inst[0].current_id][i + 1],
					dv_inst[0].md_buf[dv_inst[0].current_id][i + 2],
					dv_inst[0].md_buf[dv_inst[0].current_id][i + 3],
					dv_inst[0].md_buf[dv_inst[0].current_id][i + 4],
					dv_inst[0].md_buf[dv_inst[0].current_id][i + 5],
					dv_inst[0].md_buf[dv_inst[0].current_id][i + 6],
					dv_inst[0].md_buf[dv_inst[0].current_id][i + 7]);
		}
		total_md_size = req.aux_size;
		total_comp_size = 0;
		meta_flag_bl = 0;
		if (req.aux_buf && req.aux_size) {
			dv_inst[0].last_total_md_size = total_md_size;
			dv_inst[0].last_total_comp_size = total_comp_size;
		} else if (toggle_mode == 2) {
			total_md_size = dv_inst[0].last_total_md_size;
			total_comp_size = dv_inst[0].last_total_comp_size;
		}
		if (debug_dolby & 1)
			pr_dv_dbg("frame %d, %p, pts %lld, format: %s\n",
			dv_inst[0].frame_count, vf, vf->pts_us64,
			(src_format == FORMAT_HDR10) ? "HDR10" :
			((src_format == FORMAT_DOVI) ? "DOVI" :
			((src_format == FORMAT_DOVI_LL) ? "DOVI_LL" :
			((src_format == FORMAT_HLG) ? "HLG" : "SDR"))));

		if (toggle_mode == 1) {
			if (debug_dolby & 2)
				pr_dv_dbg("+++ get bl(%p-%lld) +++\n",
						  vf, vf->pts_us64);
			amdvdolby_vision_vf_add(vf, NULL);
		}
	} else if (vf && (vf->source_type == VFRAME_SOURCE_TYPE_OTHERS)) {
		enum vframe_signal_fmt_e fmt;

		input_mode = IN_MODE_OTT;

		req.vf = vf;
		req.bot_flag = 0;
		req.aux_buf = NULL;
		req.aux_size = 0;
		req.dv_enhance_exist = 0;

		/* check source format */
		fmt = get_vframe_src_fmt(vf);
		if ((fmt == VFRAME_SIGNAL_FMT_DOVI ||
		    fmt == VFRAME_SIGNAL_FMT_INVALID) &&
		    !vf->discard_dv_data) {
			vf_notify_provider_by_name
				(dv_provider[0],
				 VFRAME_EVENT_RECEIVER_GET_AUX_DATA,
				  (void *)&req);
		}
		/* use callback aux date first, if invalid, use sei_ptr */
		if ((!req.aux_buf || !req.aux_size) &&
		    fmt == VFRAME_SIGNAL_FMT_DOVI) {
			u32 sei_size = 0;
			char *sei;

			if (debug_dolby & 1)
				pr_dv_dbg("no aux %p %x, el %d from %s, use sei_ptr\n",
					     req.aux_buf, req.aux_size,
					     req.dv_enhance_exist, dv_provider[0]);

			sei = (char *)get_sei_from_src_fmt(vf, &sei_size);
			if (sei && sei_size) {
				req.aux_buf = sei;
				req.aux_size = sei_size;
				req.dv_enhance_exist =
					vf->src_fmt.dual_layer;
			}
		}
		if (debug_dolby & 1)
			pr_dv_dbg("%s get vf %p(%d), fmt %d, aux %p %x, el %d\n",
				     dv_provider[0], vf, vf->discard_dv_data, fmt,
				     req.aux_buf, req.aux_size, req.dv_enhance_exist);
		/* parse meta in base layer */
		if (toggle_mode != 2) {
			ret = get_md_from_src_fmt(vf);
			if (ret == 1) { /*parse succeeded*/
				meta_flag_bl = 0;
				src_format = FORMAT_DOVI;
				dv_inst[0].current_id = dv_inst[0].current_id ^ 1;
				cur_id = dv_inst[0].current_id;
				memcpy(dv_inst[0].md_buf[cur_id],
				       vf->src_fmt.md_buf,
				       vf->src_fmt.md_size);
				memcpy(dv_inst[0].comp_buf[cur_id],
				       vf->src_fmt.comp_buf,
				       vf->src_fmt.comp_size);
				total_md_size =  vf->src_fmt.md_size;
				total_comp_size =  vf->src_fmt.comp_size;
				ret_flags = vf->src_fmt.parse_ret_flags;

				dv_inst[0].md_size[cur_id] =  total_md_size;
				dv_inst[0].comp_size[cur_id] =  total_comp_size;
				dv_inst[0].last_total_md_size = total_md_size;
				dv_inst[0].last_total_comp_size = total_comp_size;

				if ((debug_dolby & 4) && dump_enable_f(0)) {
					pr_dv_dbg("get md_buf %p, size(%d):\n",
						vf->src_fmt.md_buf, vf->src_fmt.md_size);
					for (i = 0; i < total_md_size; i += 8)
						pr_info("%02x %02x %02x %02x %02x %02x %02x %02x\n",
							dv_inst[0].md_buf[cur_id][i],
							dv_inst[0].md_buf[cur_id][i + 1],
							dv_inst[0].md_buf[cur_id][i + 2],
							dv_inst[0].md_buf[cur_id][i + 3],
							dv_inst[0].md_buf[cur_id][i + 4],
							dv_inst[0].md_buf[cur_id][i + 5],
							dv_inst[0].md_buf[cur_id][i + 6],
							dv_inst[0].md_buf[cur_id][i + 7]);
				}
			} else {  /*no parse or parse failed*/
				if (get_vframe_src_fmt(vf) ==
				    VFRAME_SIGNAL_FMT_HDR10PRIME)
					src_format = FORMAT_PRIMESL;
				else
					meta_flag_bl =
					parse_sei_and_meta
						(vf, &req,
						 &total_comp_size,
						 &total_md_size,
						 &src_format,
						 &ret_flags, drop_flag, 0);
			}
			if (force_mel)
				ret_flags = 1;

			if (ret_flags && req.dv_enhance_exist) {
				if (!strcmp(dv_provider[0], "dvbldec"))
					vf_notify_provider_by_name
					(dv_provider[0],
					 VFRAME_EVENT_RECEIVER_DOLBY_BYPASS_EL,
					 (void *)&req);
				amdv_el_disable = 1;
				pr_dv_dbg("bypass mel\n");
			}
			if (ret_flags == 1)
				mel_flag = true;
			if (!is_dv_standard_es(req.dv_enhance_exist,
					       ret_flags, w)) {
				src_format = FORMAT_SDR;
				/* dovi_setting.src_format = src_format; */
				total_comp_size = 0;
				total_md_size = 0;
				src_bdp = 10;
				bypass_release = true;
			}
			if (is_aml_tvmode() &&
				(req.dv_enhance_exist && !mel_flag)) {
				src_format = FORMAT_SDR;
				/* dovi_setting.src_format = src_format; */
				total_comp_size = 0;
				total_md_size = 0;
				src_bdp = 10;
				bypass_release = true;
				if (debug_dolby & 1)
					pr_dv_dbg("tv: bypass fel\n");
			}
		} else {
			src_format = tv_hw5_setting->input.src_format;
		}

		if (src_format != FORMAT_DOVI && is_primesl_frame(vf)) {
			src_format = FORMAT_PRIMESL;
			src_bdp = 10;
		}
		if (src_format != FORMAT_DOVI && is_hdr10_frame(vf)) {
			src_format = FORMAT_HDR10;
			/* prepare parameter from SEI for hdr10 */
			p_mdc =	&vf->prop.master_display_colour;
			prepare_hdr10_param(p_mdc, &hdr10_param);
			/* for 962x with v1.4 or stb with v2.3 may use 12 bit */
			src_bdp = 10;
			req.dv_enhance_exist = 0;
		}

		if (src_format != FORMAT_DOVI && is_hlg_frame(vf)) {
			src_format = FORMAT_HLG;
			src_bdp = 10;
		}
		if (src_format != FORMAT_DOVI && is_hdr10plus_frame(vf))
			src_format = FORMAT_HDR10PLUS;

		if (src_format != FORMAT_DOVI && is_mvc_frame(vf))
			src_format = FORMAT_MVC;

		if (src_format != FORMAT_DOVI && is_cuva_frame(vf))
			src_format = FORMAT_CUVA;

		if (src_format == FORMAT_SDR &&	!req.dv_enhance_exist)
			src_bdp = 10;

		if (((debug_dolby & 1) || dv_inst[0].frame_count == 0) && toggle_mode == 1)
			pr_info
			("DV:[%d,%lld,%d,%s,%d,%d]\n",
			 dv_inst[0].frame_count, vf->pts_us64, src_bdp,
			 (src_format == FORMAT_HDR10) ? "HDR10" :
			 (src_format == FORMAT_DOVI ? "DOVI" :
			 (src_format == FORMAT_HLG ? "HLG" :
			 (src_format == FORMAT_HDR10PLUS ? "HDR10+" :
			 (src_format == FORMAT_CUVA ? "CUVA" :
			 (src_format == FORMAT_PRIMESL ? "PRIMESL" :
			 (req.dv_enhance_exist ? "DOVI (el meta)" : "SDR")))))),
			 req.aux_size, req.dv_enhance_exist);
		if (src_format != FORMAT_DOVI && !req.dv_enhance_exist)
			memset(&req, 0, sizeof(req));

		if (toggle_mode == 1) {
			if (debug_dolby & 2)
				pr_dv_dbg("+++ get bl(%p-%lld) +++\n",
					     vf, vf->pts_us64);
			amdvdolby_vision_vf_add(vf, NULL);
		}

		if (req.dv_enhance_exist)
			el_flag = 1;

		if (toggle_mode != 2) {
			dv_inst[0].last_mel_mode = mel_flag;
		} else if (meta_flag_bl) {
			total_md_size = dv_inst[0].last_total_md_size;
			total_comp_size = dv_inst[0].last_total_comp_size;
			el_flag = tv_hw5_setting->input.el_flag;
			mel_flag = dv_inst[0].last_mel_mode;
			if (debug_dolby & 2)
				pr_dv_dbg("update el_flag %d, melFlag %d\n",
					     el_flag, mel_flag);
			meta_flag_bl = 0;
		}

		if (el_flag && !mel_flag &&
		    ((dolby_vision_flags & FLAG_CERTIFICATION) == 0)) {
			el_flag = 0;
			amdv_el_disable = 1;
		}
		if (src_format != FORMAT_DOVI) {
			el_flag = 0;
			mel_flag = 0;
		}
	}
	if (src_format == FORMAT_DOVI && meta_flag_bl) {
		/* dovi frame no meta or meta error */
		/* use old setting for this frame   */
		pr_dv_dbg("no meta or meta err!\n");
		return -1;
	}

	/* if not DOVI, release metadata_parser */
	//if (vf && src_format != FORMAT_DOVI &&
	    //metadata_parser && !bypass_release) {
		//p_funcs_tv->multi_mp_release(&metadata_parser);
		//metadata_parser = NULL;
		//pr_dv_dbg("parser release\n");
	//}

	if (drop_flag) {
		pr_dv_dbg("drop frame_count %d\n", dv_inst[0].frame_count);
		return 2;
	}

	check_format = src_format;
	if (vf)
		update_src_format(check_format, vf);

	if (dolby_vision_request_mode != 0xff) {
		dolby_vision_mode = dolby_vision_request_mode;
		dolby_vision_request_mode = 0xff;
	}
	current_mode = dolby_vision_mode;
	if (amdv_policy_process
		(vf, &current_mode, check_format)) {
		if (!dv_inst[0].amdv_wait_init)
			amdv_set_toggle_flag(1);
		pr_dv_dbg("[%s]output change from %d to %d(%d, %p, %d)\n",
			     __func__, dolby_vision_mode, current_mode,
			     toggle_mode, vf, src_format);
		amdv_target_mode = current_mode;
		dolby_vision_mode = current_mode;
	} else {
		/*not clear target mode when:*/
		/*no mode change && no vf && target is not bypass */
		if ((!vf && amdv_target_mode != dolby_vision_mode &&
		    amdv_target_mode !=
		    AMDV_OUTPUT_MODE_BYPASS)) {
			if (debug_dolby & 8)
				pr_dv_dbg("not update target mode %d\n",
					     amdv_target_mode);
		} else if (amdv_target_mode != dolby_vision_mode) {
			amdv_target_mode = dolby_vision_mode;
			if (debug_dolby & 8)
				pr_dv_dbg("update target mode %d\n",
					     amdv_target_mode);
		}
	}

	if (vf && (debug_dolby & 8))
		pr_dv_dbg("parse_metadata: vf %p(index %d), mode %d\n",
			      vf, vf->omx_index, dolby_vision_mode);

	if (dolby_vision_mode == AMDV_OUTPUT_MODE_BYPASS) {
		if (amdv_target_mode == AMDV_OUTPUT_MODE_BYPASS)
			amdv_wait_on = false;
		if (debug_dolby & 8)
			pr_dv_dbg("now bypass mode, target %d, wait %d\n",
				  amdv_target_mode,
				  amdv_wait_on);
		if (get_hdr_module_status(VD1_PATH, VPP_TOP0) == HDR_MODULE_BYPASS)
			return 1;
		return -1;
	}

	if (src_format != tv_hw5_setting->input.src_format ||
		(dolby_vision_flags & FLAG_CERTIFICATION)) {
		pq_config_set_flag = false;
		best_pq_config_set_flag = false;
	}
	if (!pq_config_set_flag) {
		if (debug_dolby & 2)
			pr_dv_dbg("update def_tgt_display_cfg\n");
		if (!get_load_config_status()) {
			//memcpy(&(((struct pq_config_dvp *)pq_config_dvp_fake)->tdc),
			//       &def_tgt_display_cfg_ll,
			//       sizeof(def_tgt_display_cfg_ll));//todo
		}
		pq_config_set_flag = true;
	}
	if (force_best_pq && !best_pq_config_set_flag) {
		pr_dv_dbg("update best def_tgt_display_cfg\n");
		//memcpy(&(((struct pq_config_dvp *)
		//	pq_config_dvp_fake)->tdc),
		//	&def_tgt_display_cfg_bestpq,
		//	sizeof(def_tgt_display_cfg_bestpq));//todo
		best_pq_config_set_flag = true;

		p_funcs_tv->tv_hw5_control_path(invalid_hw5_setting);
	}
	calculate_panel_max_pq(src_format, vinfo,
			       &(((struct pq_config_dvp *)
			       pq_config_dvp_fake)->tdc));

	((struct pq_config_dvp *)
		pq_config_dvp_fake)->tdc.tuning_mode =
		amdv_tuning_mode;
	if (dolby_vision_flags & FLAG_DISABLE_COMPOSER) {
		((struct pq_config_dvp *)pq_config_dvp_fake)
			->tdc.tuning_mode |=
			TUNING_MODE_EL_FORCE_DISABLE;
	} else {
		((struct pq_config_dvp *)pq_config_dvp_fake)
			->tdc.tuning_mode &=
			(~TUNING_MODE_EL_FORCE_DISABLE);
	}
	if ((dolby_vision_flags & FLAG_CERTIFICATION) && sdr_ref_mode) {
		((struct pq_config_dvp *)
		pq_config_dvp_fake)->tdc.ambient_config.ambient =
		0;
		((struct pq_config_dvp *)pq_config_dvp_fake)
			->tdc.ref_mode_dark_id = 0;
	}
	if (is_hdr10_src_primary_changed()) {
		hdr10_src_primary_changed = true;
		pr_dv_dbg("hdr10 src primary changed!\n");
	}
	if (src_format != tv_hw5_setting->input.src_format ||
		tv_hw5_setting->input.video_width != w ||
		tv_hw5_setting->input.video_height != h ||
		hdr10_src_primary_changed) {
		pr_dv_dbg("reset control_path fmt %d->%d, w %d->%d, h %d->%d\n",
			tv_hw5_setting->input.src_format, src_format,
			tv_hw5_setting->input.video_width, w,
			tv_hw5_setting->input.video_height, h);
		/*for hdmi in cert*/
		if (dolby_vision_flags & FLAG_CERTIFICATION)
			vf_changed = true;
		p_funcs_tv->tv_hw5_control_path(invalid_hw5_setting);
	}
	pic_mode = get_cur_pic_mode_name();
	if (!(dolby_vision_flags & FLAG_CERTIFICATION) && pic_mode &&
	    (strstr(pic_mode, "dark") ||
	    strstr(pic_mode, "Dark") ||
	    strstr(pic_mode, "DARK"))) {
		memcpy(tv_input_info,
		       brightness_off,
		       sizeof(brightness_off));
		/*for HDR10/HLG, only has DM4, ko only use value from tv_input_info[3][1]*/
		/*and tv_input_info[4][1]. To avoid ko code changed, we reuse these*/
		/*parameter for both HDMI and OTT mode, that means need copy HDR10 to */
		/*tv_input_info[3][1] and copy HLG to tv_input_info[4][1] for HDMI mode*/
		if (input_mode == IN_MODE_HDMI) {
			tv_input_info->brightness_off[3][1] = brightness_off[3][0];
			tv_input_info->brightness_off[4][1] = brightness_off[4][0];
		}
	} else {
		memset(tv_input_info, 0, sizeof(brightness_off));
	}
	/*config source fps and gd_rf_adjust, dmcfg_id*/
	tv_input_info->content_fps = 24 * (1 << 16);
	tv_input_info->gd_rf_adjust = gd_rf_adjust;
	tv_input_info->tid = get_pic_mode();
	if (debug_dolby & 0x400)
		do_gettimeofday(&start);
	/*for hdmi in cert, only run control_path for different frame*/
	if ((dolby_vision_flags & FLAG_CERTIFICATION) &&
	    !vf_changed && input_mode == IN_MODE_HDMI) {
		run_control_path = false;
	}

	if (ambient_update) {
		/*only if cfg enables darkdetail we allow the API to set values*/
		if (((struct pq_config_dvp *)pq_config_dvp_fake)->
			tdc.ambient_config.dark_detail) {
			ambient_config_new.dark_detail =
			cfg_info[cur_pic_mode].dark_detail;
		}
		p_ambient = &ambient_config_new;
	} else {
		if (ambient_test_mode == 1 && toggle_mode == 1 &&
		    dv_inst[0].frame_count < AMBIENT_CFG_FRAMES) {
			p_ambient = &ambient_test_cfg[dv_inst[0].frame_count];
		} else if (ambient_test_mode == 2 && toggle_mode == 1 &&
			   dv_inst[0].frame_count < AMBIENT_CFG_FRAMES) {
			p_ambient = &ambient_test_cfg_2[dv_inst[0].frame_count];
		} else if (ambient_test_mode == 3 && toggle_mode == 1 &&
			   hdmi_frame_count < AMBIENT_CFG_FRAMES) {
			p_ambient = &ambient_test_cfg_3[hdmi_frame_count];
		} else if (((struct pq_config_dvp *)pq_config_dvp_fake)->
			tdc.ambient_config.dark_detail) {
			/*only if cfg enables darkdetail we allow the API to set*/
			ambient_darkdetail.dark_detail =
				cfg_info[cur_pic_mode].dark_detail;
			p_ambient = &ambient_darkdetail;
		}
	}
	if (debug_dolby & 0x200)
		pr_dv_dbg("[count %d %d]dark_detail from cfg:%d,from api:%d\n",
			     hdmi_frame_count, dv_inst[0].frame_count,
			     ((struct pq_config_dvp *)pq_config_dvp_fake)->
			     tdc.ambient_config.dark_detail,
			     cfg_info[cur_pic_mode].dark_detail);

	update_l1l4_hist_setting();

	tv_hw5_setting->input.video_width = w;
	tv_hw5_setting->input.video_height = h;

	if (debug_cp_res > 0) {
		tv_hw5_setting->input.video_width = (debug_cp_res & 0xffff0000) >> 16;
		tv_hw5_setting->input.video_height = debug_cp_res & 0xffff;
	}
	tv_hw5_setting->input.input_mode = input_mode;
	tv_hw5_setting->input.in_md = dv_inst[0].md_buf[dv_inst[0].current_id];
	tv_hw5_setting->input.in_md_size = (src_format == FORMAT_DOVI) ? total_md_size : 0;
	tv_hw5_setting->input.in_comp = dv_inst[0].comp_buf[dv_inst[0].current_id];
	tv_hw5_setting->input.in_comp_size = (src_format == FORMAT_DOVI) ? total_comp_size : 0;
	tv_hw5_setting->input.set_bit_depth = src_bdp;
	tv_hw5_setting->input.set_chroma_format = src_chroma_format;
	tv_hw5_setting->input.set_yuv_range = SIGNAL_RANGE_SMPTE;
	tv_hw5_setting->input.hdr10_param = &dv_inst[0].hdr10_param;
	tv_hw5_setting->input.vsem_if = vsem_if_buf;
	tv_hw5_setting->input.vsem_if_size = vsem_if_size;
	tv_hw5_setting->input.el_flag = el_flag;
	tv_hw5_setting->pq_config = (struct pq_config_dvp *)pq_config_dvp_fake;
	tv_hw5_setting->menu_param = &menu_param;
	tv_hw5_setting->ambient_cfg = p_ambient;
	tv_hw5_setting->input_info = tv_input_info;
	tv_hw5_setting->enable_debug = debug_ko;

	if (run_control_path) {
		if (enable_top1) {
			/*step1: top1 frame N*/
			tv_hw5_setting->analyzer = 1;
			flag = p_funcs_tv->tv_hw5_control_path(tv_hw5_setting);

			/*step2: top2 frame N-1*/
			tv_hw5_setting->analyzer = 0;
			tv_hw5_setting->input.in_md = dv_inst[0].md_buf[dv_inst[0].current_id ^ 1];
			tv_hw5_setting->input.in_comp =
				dv_inst[0].comp_buf[dv_inst[0].current_id ^ 1];
			tv_hw5_setting->input.in_md_size = (src_format == FORMAT_DOVI) ?
				dv_inst[0].md_size[dv_inst[0].current_id ^ 1] : 0;
			tv_hw5_setting->input.in_comp_size = (src_format == FORMAT_DOVI) ?
				dv_inst[0].comp_size[dv_inst[0].current_id ^ 1] : 0;
			flag = p_funcs_tv->tv_hw5_control_path(tv_hw5_setting);
		} else {
			tv_hw5_setting->analyzer = 0;
			flag = p_funcs_tv->tv_hw5_control_path(tv_hw5_setting);
		}

		if (debug_dolby & 0x400) {
			do_gettimeofday(&end);
			time_use = (end.tv_sec - start.tv_sec) * 1000000 +
				(end.tv_usec - start.tv_usec);

			pr_info("controlpath time: %5ld us\n", time_use);
		}
		if (flag >= 0) {
			if (input_mode == IN_MODE_HDMI) {
				//if (h > 1080)//todo
				//	tv_hw5_setting->core1_reg_lut[1] =
				//		0x0000000100000043;
				//else
				//	tv_hw5_setting->core1_reg_lut[1] =
				//		0x0000000100000042;
			} else {
				//if (src_format == FORMAT_HDR10)
				//	tv_dovi_setting->core1_reg_lut[1] =
				//		0x000000010000404c;
				//else
				//	tv_dovi_setting->core1_reg_lut[1] =
				//		0x0000000100000044;
			}
			/* enable CRC */
			if ((dolby_vision_flags & FLAG_CERTIFICATION) &&
				!(dolby_vision_flags & FLAG_DISABLE_CRC))
				tv_hw5_setting->top2_reg[573] =
					0x0000023d00000001;

			tv_dovi_setting_change_flag = true;
			top2_info.amdv_setting_video_flag = video_frame;

			if (debug_dolby & 1) {
				pr_dv_dbg
				("tv setting %s-%d:flag=%x,md=%d,comp=%d\n",
					 input_mode == IN_MODE_HDMI ?
					 "hdmi" : "ott",
					 src_format,
					 flag,
					 total_md_size,
					 total_comp_size);
			}
			dump_tv_setting(tv_hw5_setting,
				dv_inst[0].frame_count, debug_dolby);
			dv_inst[0].last_mel_mode = mel_flag;
			ret = 0; /* setting updated */
		} else {
			tv_hw5_setting->input.video_width = 0;
			tv_hw5_setting->input.video_height = 0;
			pr_dv_error("tv_hw5_control_path() failed\n");
		}
	} else { /*for cert: vf no change, not run cp*/
		//if (h > 1080)//todo
		//	tv_hw5_setting->core1_reg_lut[1] =
		//		0x0000000100000043;
		//else
		//	tv_hw5_setting->core1_reg_lut[1] =
		//		0x0000000100000042;

		/* enable CRC */
		if ((dolby_vision_flags & FLAG_CERTIFICATION) &&
			!(dolby_vision_flags & FLAG_DISABLE_CRC))
			tv_hw5_setting->top2_reg[573] = 0x0000023d00000001;
		tv_dovi_setting_change_flag = true;
		top2_info.amdv_setting_video_flag = video_frame;
		ret = 0;
	}
	return ret;
}

int amdv_wait_metadata_hw5(struct vframe_s *vf)
{
	int ret = 0;
	unsigned int mode = dolby_vision_mode;
	enum signal_format_enum check_format;
	bool vd1_on = false;

	if (single_step_enable_v2(0, VD1_PATH)) {
		if (dolby_vision_flags & FLAG_SINGLE_STEP)
			/* wait fake el for "step" */
			return 1;

		dolby_vision_flags |= FLAG_SINGLE_STEP;
	}

	if (dolby_vision_flags & FLAG_CERTIFICATION) {
		bool ott_mode = true;

		if (is_aml_tvmode() && tv_hw5_setting)
			ott_mode = tv_hw5_setting->input.input_mode !=
				IN_MODE_HDMI;
		if (debug_dolby & 0x1000)
			pr_dv_dbg("setting_update_count %d, crc_count %d, flag %x\n",
				setting_update_count, crc_count, dolby_vision_flags);
		if (setting_update_count > crc_count &&
			!(dolby_vision_flags & FLAG_DISABLE_CRC)) {
			if (ott_mode)
				return 1;
		}
	}

	if (enable_top1) {
		if (!top1_done) {//todo
			if (vf && (debug_dolby & 8))
				pr_dv_dbg("wait top1\n");
			return 1;
		}
	}

	if (!dv_inst[0].amdv_wait_init && !top2_info.top2_on) {
		ret = is_amdv_frame(vf);
		if (ret) {
			check_format = FORMAT_DOVI;
			ret = 0;
		} else if (is_primesl_frame(vf)) {
			check_format = FORMAT_PRIMESL;
		} else if (is_hdr10_frame(vf)) {
			check_format = FORMAT_HDR10;
		} else if (is_hlg_frame(vf)) {
			check_format = FORMAT_HLG;
		} else if (is_hdr10plus_frame(vf)) {
			check_format = FORMAT_HDR10PLUS;
		} else if (is_mvc_frame(vf)) {
			check_format = FORMAT_MVC;
		} else if (is_cuva_frame(vf)) {
			check_format = FORMAT_CUVA;
		} else {
			check_format = FORMAT_SDR;
		}

		if (vf)
			update_src_format(check_format, vf);

		if (amdv_policy_process(vf, &mode, check_format)) {
			if (mode != AMDV_OUTPUT_MODE_BYPASS &&
			    dolby_vision_mode ==
			    AMDV_OUTPUT_MODE_BYPASS) {
				dv_inst[0].amdv_wait_init = true;
				amdv_target_mode = mode;
				amdv_wait_on = true;
				pr_dv_dbg("dolby_vision_need_wait src=%d mode=%d\n",
					check_format, mode);
			}
		}
		if (is_aml_t3x()) {
			if (READ_VPP_DV_REG(VD1_BLEND_SRC_CTRL) & (0xf << 0))
				vd1_on = true;
		}
		/* don't use run mode when sdr -> dv and vd1 not disable */
		if (/*dv_inst[0].amdv_wait_init && */vd1_on && !force_runmode)
			top2_info.run_mode_count = amdv_run_mode_delay + 1;
		if (debug_dolby & 8)
			pr_dv_dbg("amdv_on_count %d, vd1_on %d\n",
				      top2_info.run_mode_count, vd1_on);
	}

	if (top2_info.top2_on &&
		top2_info.run_mode_count <= amdv_run_mode_delay)
		ret = 1;

	if (vf && (debug_dolby & 8))
		pr_dv_dbg("wait return %d, vf %p(index %d), runcount %d\n",
			      ret, vf, vf->omx_index, top2_info.run_mode_count);

	return ret;
}

int amdv_update_src_format_hw5(struct vframe_s *vf, u8 toggle_mode)
{
	unsigned int mode = dolby_vision_mode;
	enum signal_format_enum check_format;
	int ret = 0;

	if (!is_amdv_enable() || !vf)
		return -1;

	/* src_format is valid, need not re-init */
	if (dv_inst[0].amdv_src_format != 0)
		return 0;

	/* vf is not in the dv queue, new frame case */
	if (amdv_vf_check(vf))
		return 0;

	ret = is_amdv_frame(vf);
	if (ret)
		check_format = FORMAT_DOVI;
	else if (is_primesl_frame(vf))
		check_format = FORMAT_PRIMESL;
	else if (is_hdr10_frame(vf))
		check_format = FORMAT_HDR10;
	else if (is_hlg_frame(vf))
		check_format = FORMAT_HLG;
	else if (is_hdr10plus_frame(vf))
		check_format = FORMAT_HDR10PLUS;
	else if (is_mvc_frame(vf))
		check_format = FORMAT_MVC;
	else if (is_cuva_frame(vf))
		check_format = FORMAT_CUVA;
	else
		check_format = FORMAT_SDR;

	if (vf)
		update_src_format(check_format, vf);

	if (!dv_inst[0].amdv_wait_init &&
	    !top2_info.top2_on &&
	    dv_inst[0].amdv_src_format != 0) {
		if (amdv_policy_process
			(vf, &mode, check_format)) {
			if (mode != AMDV_OUTPUT_MODE_BYPASS &&
			    dolby_vision_mode ==
			    AMDV_OUTPUT_MODE_BYPASS) {
				dv_inst[0].amdv_wait_init = true;
				amdv_target_mode = mode;
				amdv_wait_on = true;
				pr_dv_dbg
					("dolby_vision_need_wait src=%d mode=%d\n",
					 check_format, mode);
			}
		}
	}
	pr_dv_dbg
		("%s done vf:%p, src=%d, toggle mode:%d\n",
		__func__, vf, dv_inst[0].amdv_src_format, toggle_mode);
	return 1;
}

int amdolby_vision_process_hw5(struct vframe_s *vf,
			 u32 display_size,
			 u8 toggle_mode, u8 pps_state)
{
	int src_chroma_format = 0;
	u32 h_size = (display_size >> 16) & 0xffff;
	u32 v_size = display_size & 0xffff;
	bool reset_flag = false;
	bool force_set = false;
	unsigned int mode = dolby_vision_mode;
	static bool video_turn_off = true;
	static bool video_on[VD_PATH_MAX];
	int video_status = 0;
	int policy_changed = 0;
	int format_changed = 0;
	bool src_is_42210bit = false;
	static bool reverse_status;
	bool reverse = false;
	bool reverse_changed = false;
	static u8 last_toggle_mode;
	static struct vframe_s *last_vf;

	if (dolby_vision_flags & FLAG_CERTIFICATION) {
		if (vf) {
			h_size = (vf->type & VIDTYPE_COMPRESS) ?
				vf->compWidth : vf->width;
			v_size = (vf->type & VIDTYPE_COMPRESS) ?
				vf->compHeight : vf->height;
		} else {
			h_size = 0;
			v_size = 0;
		}
		top2_info.run_mode_count = 1 +	amdv_run_mode_delay;
	} else {
		if (vf && vf != last_vf && tv_hw5_setting)
			update_aoi_flag(vf, display_size);
		last_vf = vf;
	}

	if (vf && (debug_dolby & 0x8))
		pr_dv_dbg("%s: vf %p(index %d),mode %d,top2_on %d,toggle %d,flag %x\n",
			     __func__, vf, vf->omx_index,
			     dolby_vision_mode, top2_info.top2_on, toggle_mode, dolby_vision_flags);

	if (dolby_vision_flags & FLAG_TOGGLE_FRAME) {
		h_size = (display_size >> 16) & 0xffff;
		v_size = display_size & 0xffff;
		/* tv control path case */
		if (top2_info.top2_disp_hsize != h_size ||
			top2_info.top2_disp_vsize != v_size) {
			/* tvcore need force config for res change */
			force_set = true;
			if (debug_dolby & 8)
				pr_dv_dbg
				("tv update disp size %d %d -> %d %d\n",
				 top2_info.top2_disp_hsize,
				 top2_info.top2_disp_vsize, h_size, v_size);
			top2_info.top2_disp_hsize = h_size;
			top2_info.top2_disp_vsize = v_size;
		}
		if (!vf || toggle_mode != 1) {
			/* log to monitor if has dv toggles not needed */
			pr_dv_dbg("NULL/RPT frame %p, hdr module %s, video %s\n",
				     vf,
				     get_hdr_module_status(VD1_PATH, VPP_TOP0)
				     == HDR_MODULE_ON ? "on" : "off",
				     get_video_enabled(0) ? "on" : "off");
		}
	}
	last_toggle_mode = toggle_mode;

	calculate_crc();

	video_status = is_video_turn_on(video_on, VD1_PATH);
	if (video_status == -1) {
		video_turn_off = true;
		pr_dv_dbg("VD1 video off, video_status -1\n");
	} else if (video_status == 1) {
		pr_dv_dbg("VD1 video on, video_status 1\n");
		video_turn_off = false;
	}

	if (dolby_vision_mode != amdv_target_mode)
		format_changed = 1;

	/* monitor policy changes */
	policy_changed = is_policy_changed();

	if (policy_changed || format_changed)
		amdv_set_toggle_flag(1);

	if (is_aml_tvmode()) {
		reverse = get_video_reverse();
		if (reverse != reverse_status) {
			reverse_status = reverse;
			reverse_changed = true;
		}
		if (policy_changed || format_changed ||
			video_status == 1 || reverse_changed)
			pr_dv_dbg("tv %s %s %s %s\n",
				policy_changed ? "policy changed" : "",
				video_status ? "video_status changed" : "",
				format_changed ? "format_changed" : "",
				reverse_changed ? "reverse_changed" : "");
	}

	if (policy_changed || format_changed ||
	    (video_status == 1 && !(dolby_vision_flags & FLAG_CERTIFICATION)) ||
	    need_update_cfg || reverse_changed) {
		if (debug_dolby & 1)
			pr_dv_dbg("video %s,osd %s,vf %p,toggle %d\n",
				     video_turn_off ? "off" : "on",
				     is_graphics_output_off() ? "off" : "on",
				     vf, toggle_mode);
		/* do not toggle a new el vf */
		if (toggle_mode == 1)
			toggle_mode = 0;
		if (vf &&
		    !amdv_parse_metadata
		    (vf, VD1_PATH, toggle_mode, false, false)) {
			h_size = (display_size >> 16) & 0xffff;
			v_size = display_size & 0xffff;
			amdv_set_toggle_flag(1);
		}
		need_update_cfg = false;
	}

	if (debug_dolby & 8)
		pr_dv_dbg("vf %p, turn_off %d, video_status %d, toggle %d, flag %x\n",
			vf, video_turn_off, video_status,
			toggle_mode, dolby_vision_flags);

	if ((!vf && video_turn_off) ||
	    (video_status == -1)) {
		if (amdv_policy_process(vf, &mode, FORMAT_SDR)) {
			pr_dv_dbg("Fake SDR, mode->%d\n", mode);
			if (dolby_vision_policy == AMDV_FOLLOW_SOURCE &&
			    mode == AMDV_OUTPUT_MODE_BYPASS) {
				amdv_target_mode =
					AMDV_OUTPUT_MODE_BYPASS;
				dolby_vision_mode =
					AMDV_OUTPUT_MODE_BYPASS;
				amdv_set_toggle_flag(0);
				amdv_wait_on = false;
				dv_inst[0].amdv_wait_init = false;
			} else {
				amdv_set_toggle_flag(1);
			}
		}
		if ((dolby_vision_flags & FLAG_TOGGLE_FRAME) ||
		((video_status == -1) && top2_info.top2_on)) {
			pr_dv_dbg("update when video off\n");
			amdv_parse_metadata
				(NULL, VD1_PATH, 1, false, false);
			amdv_set_toggle_flag(1);
		}
		if (!vf && video_turn_off &&
			!top2_info.top2_on &&
			dv_inst[0].amdv_src_format != 0) {
			pr_dv_dbg("update src_fmt when video off\n");
			dv_inst[0].amdv_src_format = 0;
		}
	}

	if (dolby_vision_mode == AMDV_OUTPUT_MODE_BYPASS) {
		if (dolby_vision_status != BYPASS_PROCESS)
			enable_amdv(0);
		dolby_vision_flags &= ~FLAG_TOGGLE_FRAME;
		return 0;
	}
	if ((dolby_vision_flags & FLAG_CERTIFICATION) ||
	    (dolby_vision_flags & FLAG_BYPASS_VPP))
		video_effect_bypass(1);

	if (!p_funcs_stb && (!p_funcs_tv && !hw5_reg_from_file)) {
		dolby_vision_flags &= ~FLAG_TOGGLE_FRAME;
		tv_dovi_setting_change_flag = false;
		return 0;
	}

	if (dolby_vision_flags & FLAG_TOGGLE_FRAME) {
		if (!(dolby_vision_flags & FLAG_CERTIFICATION))
			reset_flag =
				(amdv_reset & 1) &&
				(!top2_info.top2_on) &&
				(top2_info.run_mode_count == 0);
		if (is_aml_tvmode()) {
			if (tv_dovi_setting_change_flag || force_set) {
				if (vf && (vf->type & VIDTYPE_VIU_422))
					src_chroma_format = 2;
				else if (vf)
					src_chroma_format = 1;
				if (tv_hw5_setting &&
					(tv_hw5_setting->input.src_format ==
					FORMAT_HDR10 ||
					tv_hw5_setting->input.src_format ==
					FORMAT_HLG ||
					tv_hw5_setting->input.src_format ==
					FORMAT_SDR))
					src_is_42210bit = true;

				if (tv_hw5_setting)
					tv_top_set
					(tv_hw5_setting->top1_reg,
					 tv_hw5_setting->top1b_reg,
					 tv_hw5_setting->top2_reg,
					 h_size, v_size,
					 top2_info.amdv_setting_video_flag, /* video enable */
					 src_chroma_format,
					 tv_hw5_setting->input.input_mode == IN_MODE_HDMI,
					 src_is_42210bit, reset_flag, true);
				else if (hw5_reg_from_file)
					tv_top_set
					(NULL, NULL, NULL,
					 h_size, v_size,
					 top2_info.amdv_setting_video_flag, /* video enable */
					 src_chroma_format,
					 false,
					 src_is_42210bit, reset_flag, true);

				if (!h_size || !v_size)
					top2_info.amdv_setting_video_flag = false;
				if (top2_info.amdv_setting_video_flag &&
				    top2_info.run_mode_count == 0)
					pr_dv_dbg("first frame reset %d\n",
						     reset_flag);
				enable_amdv(1);
				if (tv_hw5_setting) {
					if (tv_hw5_setting->backlight !=
					    tv_backlight ||
					    (top2_info.amdv_setting_video_flag &&
					    top2_info.run_mode_count == 0) ||
					    tv_backlight_force_update) {
						pr_dv_dbg("backlight %d -> %d\n",
							tv_backlight,
							tv_hw5_setting->backlight);
						tv_backlight =
							tv_hw5_setting->backlight;
						tv_backlight_changed = true;
						bl_delay_cnt = 0;
						tv_backlight_force_update = false;
					}
					update_amdv_status(tv_hw5_setting->input.src_format);
				}
				tv_dovi_setting_change_flag = false;
			}
		}
		dolby_vision_flags &= ~FLAG_TOGGLE_FRAME;
	} else if (top2_info.top2_on &&
		!(dolby_vision_flags & FLAG_CERTIFICATION)) {
		if (force_set || force_update_top2) {
			if (force_set)
				reset_flag = true;
			if (tv_hw5_setting &&
				(tv_hw5_setting->input.src_format ==
				FORMAT_HDR10 ||
				tv_hw5_setting->input.src_format ==
				FORMAT_HLG ||
				tv_hw5_setting->input.src_format ==
				FORMAT_SDR))
				src_is_42210bit = true;

			if (tv_hw5_setting)
				tv_top_set
				(tv_hw5_setting->top1_reg,
				 tv_hw5_setting->top1b_reg,
				 tv_hw5_setting->top2_reg,
				 h_size, v_size,
				 top2_info.amdv_setting_video_flag, /* BL enable */
				 src_chroma_format,
				 tv_hw5_setting->input.input_mode == IN_MODE_HDMI,
				 src_is_42210bit, reset_flag, force_set ? true : false);
			else if (hw5_reg_from_file)
				tv_top_set
				(NULL, NULL, NULL,
				 h_size, v_size,
				 top2_info.amdv_setting_video_flag, /* BL enable */
				 src_chroma_format,
				 false,
				 src_is_42210bit, reset_flag, force_set ? true : false);
		}
	}
	if (top2_info.top2_on) {
		if (top2_info.run_mode_count <= amdv_run_mode_delay + 1)
			top2_info.run_mode_count++;
	} else {
		top2_info.run_mode_count = 0;
	}

	if (debug_dolby & 8)
		pr_dv_dbg("%s: run_mode_count %d\n",
		     __func__, top2_info.run_mode_count);
	return 0;
}

//todo
void update_top1_status(struct vframe_s *vf)
{
	if (vf) {
		if (vf->width < 480)
			enable_top1 = false;
		if (!(vf->type & VIDTYPE_COMPRESS))
			enable_top1 = false;
	}
}

bool get_top1_status(void)
{
	return enable_top1;
}
