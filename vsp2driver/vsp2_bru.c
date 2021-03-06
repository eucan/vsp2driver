/*************************************************************************/ /*
 * VSP2
 *
 * Copyright (C) 2015-2017 Renesas Electronics Corporation
 *
 * License        Dual MIT/GPLv2
 *
 * The contents of this file are subject to the MIT license as set out below.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * Alternatively, the contents of this file may be used under the terms of
 * the GNU General Public License Version 2 ("GPL") in which case the provisions
 * of GPL are applicable instead of those above.
 *
 * If you wish to allow use of your version of this file only under the terms of
 * GPL, and not to allow others to use your version of this file under the terms
 * of the MIT license, indicate your decision by deleting the provisions above
 * and replace them with the notice and other provisions required by GPL as set
 * out in the file called "GPL-COPYING" included in this distribution. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under the terms of either the MIT license or GPL.
 *
 * This License is also included in this distribution in the file called
 * "MIT-COPYING".
 *
 * EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
 * PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS
 * OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 * IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * GPLv2:
 * If you wish to use this file under the terms of GPL, following terms are
 * effective.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */ /*************************************************************************/

#include <linux/device.h>
#include <linux/gfp.h>

#include <media/v4l2-subdev.h>

#include "vsp2_device.h"
#include "vsp2_bru.h"
#include "vsp2_pipe.h"
#include "vsp2_rwpf.h"
#include "vsp2_video.h"
#include "vsp2_vspm.h"

#define BRU_MIN_SIZE				1U
#define BRU_MAX_SIZE				8190U

/* -----------------------------------------------------------------------------
 * Controls
 */

static int bru_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vsp2_bru *bru =
		container_of(ctrl->handler, struct vsp2_bru, ctrls);

	switch (ctrl->id) {
	case V4L2_CID_BG_COLOR:
		bru->bgcolor = ctrl->val;
		break;
	}

	return 0;
}

static const struct v4l2_ctrl_ops bru_ctrl_ops = {
	.s_ctrl = bru_s_ctrl,
};

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

/*
 * The BRU can't perform format conversion, all sink and source formats must be
 * identical. We pick the format on the first sink pad (pad 0) and propagate it
 * to all other pads.
 */

static int bru_enum_mbus_code(struct v4l2_subdev *subdev,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	static const unsigned int codes[] = {
		MEDIA_BUS_FMT_ARGB8888_1X32,
		MEDIA_BUS_FMT_AYUV8_1X32,
	};

	return vsp2_subdev_enum_mbus_code(subdev, cfg, code, codes,
					  ARRAY_SIZE(codes));
}

static int bru_enum_frame_size(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_pad_config *cfg,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index)
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_ARGB8888_1X32 &&
	    fse->code != MEDIA_BUS_FMT_AYUV8_1X32)
		return -EINVAL;

	fse->min_width = BRU_MIN_SIZE;
	fse->max_width = BRU_MAX_SIZE;
	fse->min_height = BRU_MIN_SIZE;
	fse->max_height = BRU_MAX_SIZE;

	return 0;
}

static struct v4l2_rect *bru_get_compose(struct vsp2_bru *bru,
					 struct v4l2_subdev_pad_config *cfg,
					 unsigned int pad)
{
	return v4l2_subdev_get_try_compose(&bru->entity.subdev, cfg, pad);
}

static void bru_try_format(struct vsp2_bru *bru,
			   struct v4l2_subdev_pad_config *config,
			   unsigned int pad, struct v4l2_mbus_framefmt *fmt)
{
	struct v4l2_mbus_framefmt *format;

	switch (pad) {
	case BRU_PAD_SINK(0):
		/* Default to YUV if the requested format is not supported. */
		if (fmt->code != MEDIA_BUS_FMT_ARGB8888_1X32 &&
		    fmt->code != MEDIA_BUS_FMT_AYUV8_1X32)
			fmt->code = MEDIA_BUS_FMT_AYUV8_1X32;
		break;

	default:
		/* The BRU can't perform format conversion. */
		format = vsp2_entity_get_pad_format(&bru->entity, config,
						    BRU_PAD_SINK(0));
		fmt->code = format->code;
		break;
	}

	fmt->width = clamp(fmt->width, BRU_MIN_SIZE, BRU_MAX_SIZE);
	fmt->height = clamp(fmt->height, BRU_MIN_SIZE, BRU_MAX_SIZE);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
}

static int bru_set_format(
	struct v4l2_subdev *subdev, struct v4l2_subdev_pad_config *cfg,
	struct v4l2_subdev_format *fmt)
{
	struct vsp2_bru *bru = to_bru(subdev);
	struct v4l2_subdev_pad_config *config;
	struct v4l2_mbus_framefmt *format;
	int ret = 0;

	mutex_lock(&bru->entity.lock);

	config = vsp2_entity_get_pad_config(&bru->entity, cfg, fmt->which);
	if (!config) {
		ret = -EINVAL;
		goto done;
	}

	bru_try_format(bru, config, fmt->pad, &fmt->format);

	format = vsp2_entity_get_pad_format(&bru->entity, config, fmt->pad);
	*format = fmt->format;

	/* Reset the compose rectangle */
	if (fmt->pad != BRU_PAD_SOURCE) {
		struct v4l2_rect *compose;

		compose = bru_get_compose(bru, config, fmt->pad);
		compose->left = 0;
		compose->top = 0;
		compose->width = format->width;
		compose->height = format->height;
	}

	/* Propagate the format code to all pads */
	if (fmt->pad == BRU_PAD_SINK(0)) {
		unsigned int i;

		for (i = 0; i <= BRU_PAD_SOURCE; ++i) {
			format = vsp2_entity_get_pad_format(&bru->entity,
							    config, i);
			format->code = fmt->format.code;
		}
	}

done:
	mutex_unlock(&bru->entity.lock);
	return ret;
}

static int bru_get_selection(struct v4l2_subdev *subdev,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_selection *sel)
{
	struct vsp2_bru *bru = to_bru(subdev);
	struct v4l2_subdev_pad_config *config;

	if (sel->pad == BRU_PAD_SOURCE)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = BRU_MAX_SIZE;
		sel->r.height = BRU_MAX_SIZE;
		return 0;

	case V4L2_SEL_TGT_COMPOSE:
		config = vsp2_entity_get_pad_config(&bru->entity, cfg,
						    sel->which);
		if (!config)
			return -EINVAL;

		mutex_lock(&bru->entity.lock);
		sel->r = *bru_get_compose(bru, config, sel->pad);
		mutex_unlock(&bru->entity.lock);
		return 0;

	default:
		return -EINVAL;
	}
}

static int bru_set_selection(struct v4l2_subdev *subdev,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_selection *sel)
{
	struct vsp2_bru *bru = to_bru(subdev);
	struct v4l2_subdev_pad_config *config;
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *compose;
	int ret = 0;

	if (sel->pad == BRU_PAD_SOURCE)
		return -EINVAL;

	if (sel->target != V4L2_SEL_TGT_COMPOSE)
		return -EINVAL;

	mutex_lock(&bru->entity.lock);

	config = vsp2_entity_get_pad_config(&bru->entity, cfg, sel->which);
	if (!config) {
		ret = -EINVAL;
		goto done;
	}

	/* The compose rectangle top left corner must be inside the output
	 * frame.
	 */
	format = vsp2_entity_get_pad_format(&bru->entity, config,
					    BRU_PAD_SOURCE);
	sel->r.left = clamp_t(unsigned int, sel->r.left, 0, format->width - 1);
	sel->r.top = clamp_t(unsigned int, sel->r.top, 0, format->height - 1);

	/* Scaling isn't supported, the compose rectangle size must be identical
	 * to the sink format size.
	 */
	format = vsp2_entity_get_pad_format(&bru->entity, config, sel->pad);
	sel->r.width = format->width;
	sel->r.height = format->height;

	compose = bru_get_compose(bru, config, sel->pad);
	*compose = sel->r;

done:
	mutex_unlock(&bru->entity.lock);
	return ret;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static const struct v4l2_subdev_pad_ops bru_pad_ops = {
	.init_cfg = vsp2_entity_init_cfg,
	.enum_mbus_code = bru_enum_mbus_code,
	.enum_frame_size = bru_enum_frame_size,
	.get_fmt = vsp2_subdev_get_pad_format,
	.set_fmt = bru_set_format,
	.get_selection = bru_get_selection,
	.set_selection = bru_set_selection,
};

static const struct v4l2_subdev_ops bru_ops = {
	.pad    = &bru_pad_ops,
};

/* -----------------------------------------------------------------------------
 * VSP2 Entity Operations
 */

static void bru_configure(struct vsp2_entity *entity,
			  struct vsp2_pipeline *pipe)
{
	struct vsp2_bru *bru = to_bru(&entity->subdev);
	struct v4l2_mbus_framefmt *format;
	unsigned int flags;
	unsigned int i;
	struct vsp_start_t *vsp_par =
		bru->entity.vsp2->vspm->ip_par.par.vsp;
	struct vsp_bru_t *vsp_bru = vsp_par->ctrl_par->bru;
	u32 inctrl;

	format = vsp2_entity_get_pad_format(&bru->entity, bru->entity.config,
					    BRU_PAD_SOURCE);

	/* The hardware is extremely flexible but we have no userspace API to
	 * expose all the parameters, nor is it clear whether we would have use
	 * cases for all the supported modes. Let's just harcode the parameters
	 * to sane default values for now.
	 */

	/* Disable dithering and enable color data normalization unless the
	 * format at the pipeline output is premultiplied.
	 */
	flags = pipe->output ? pipe->output->format.flags : 0;
	inctrl = flags & V4L2_PIX_FMT_FLAG_PREMUL_ALPHA ?
		 0 : VI6_BRU_INCTRL_NRM;
	vsp_bru->adiv    = (inctrl & (1 << 28)) >> 28;

	/* Set the background position to cover the whole output image and
	 * configure its color.
	 */
	vsp_bru->blend_virtual->width		= format->width;
	vsp_bru->blend_virtual->height		= format->height;
	vsp_bru->blend_virtual->x_position	= 0;
	vsp_bru->blend_virtual->y_position	= 0;
	vsp_bru->blend_virtual->pwd		= VSP_LAYER_PARENT;
	vsp_bru->blend_virtual->color =
		bru->bgcolor | (0xff << VI6_BRU_VIRRPF_COL_A_SHIFT);

	/* Route BRU input 1 as SRC input to the ROP unit and configure the ROP
	 * unit with a NOP operation to make BRU input 1 available as the
	 * Blend/ROP unit B SRC input.
	 */
	vsp_bru->rop_unit = NULL;

	for (i = 0; i < BRU_PAD_SOURCE; ++i) {
		bool premultiplied = false;
		u32 ctrl = 0;
		struct vsp_bld_ctrl_t *vsp_bru_ctrl = NULL;

		switch (i) {
		case 0:
			vsp_bru_ctrl = vsp_bru->blend_unit_a;
			break;
		case 1:
			vsp_bru_ctrl = vsp_bru->blend_unit_b;
			break;
		case 2:
			vsp_bru_ctrl = vsp_bru->blend_unit_c;
			break;
		case 3:
			vsp_bru_ctrl = vsp_bru->blend_unit_d;
			break;
		case 4:
			vsp_bru_ctrl = vsp_bru->blend_unit_e;
			break;
		default:
			/* Invalid index. */
			break;
		}

		/* Configure all Blend/ROP units corresponding to an enabled BRU
		 * input for alpha blending. Blend/ROP units corresponding to
		 * disabled BRU inputs are used in ROP NOP mode to ignore the
		 * SRC input.
		 */
		if (bru->inputs[i].rpf) {
			ctrl |= VI6_BRU_CTRL_RBC;

			premultiplied = bru->inputs[i].rpf->format.flags
				      & V4L2_PIX_FMT_FLAG_PREMUL_ALPHA;
		} else {
			ctrl |= VI6_BRU_CTRL_CROP(VI6_ROP_NOP)
			     |  VI6_BRU_CTRL_AROP(VI6_ROP_NOP);
		}

		/* Select the virtual RPF as the Blend/ROP unit A DST input to
		 * serve as a background color.
		 */
		if (i == 0)
			ctrl |= VI6_BRU_CTRL_DSTSEL_VRPF;

		/* Route BRU inputs 0 to 3 as SRC inputs to Blend/ROP units A to
		 * D in that order. The Blend/ROP unit B SRC is hardwired to the
		 * ROP unit output, the corresponding register bits must be set
		 * to 0.
		 */
		if (i != 1)
			ctrl |= VI6_BRU_CTRL_SRCSEL_BRUIN(i);

		vsp_bru_ctrl->rbc  = (ctrl & (1 << 31)) >> 31;
		vsp_bru_ctrl->crop = (ctrl & (0xF <<  4)) >>  4;
		vsp_bru_ctrl->arop = (ctrl & (0xF <<  0)) >>  0;

		/* Harcode the blending formula to
		 *
		 *	DSTc = DSTc * (1 - SRCa) + SRCc * SRCa
		 *	DSTa = DSTa * (1 - SRCa) + SRCa
		 *
		 * when the SRC input isn't premultiplied, and to
		 *
		 *	DSTc = DSTc * (1 - SRCa) + SRCc
		 *	DSTa = DSTa * (1 - SRCa) + SRCa
		 *
		 * otherwise.
		 */
		vsp_bru_ctrl->blend_formula = VSP_FORM_BLEND0;
		vsp_bru_ctrl->blend_coefx = VSP_COEFFICIENT_BLENDX4;
		vsp_bru_ctrl->blend_coefy = (premultiplied) ?
			VSP_COEFFICIENT_BLENDY5 : VSP_COEFFICIENT_BLENDY3;
		vsp_bru_ctrl->aformula = VSP_FORM_ALPHA0;
		vsp_bru_ctrl->acoefx = VSP_COEFFICIENT_ALPHAX4;
		vsp_bru_ctrl->acoefy = VSP_COEFFICIENT_ALPHAY5;
		vsp_bru_ctrl->acoefx_fix = 0;    /* Set coefficient x. */
		vsp_bru_ctrl->acoefy_fix = 0xFF; /* Set coefficient y. */
	}
}

static const struct vsp2_entity_operations bru_entity_ops = {
	.configure = bru_configure,
};

/* -----------------------------------------------------------------------------
 * Initialization and Cleanup
 */

struct vsp2_bru *vsp2_bru_create(struct vsp2_device *vsp2)
{
	struct vsp2_bru *bru;
	int ret;

	bru = devm_kzalloc(vsp2->dev, sizeof(*bru), GFP_KERNEL);
	if (!bru)
		return ERR_PTR(-ENOMEM);

	bru->entity.ops = &bru_entity_ops;
	bru->entity.type = VSP2_ENTITY_BRU;

	ret = vsp2_entity_init(vsp2, &bru->entity, "bru",
			       (BRU_PAD_SOURCE + 1), &bru_ops,
			       MEDIA_ENT_F_PROC_VIDEO_COMPOSER);
	if (ret < 0)
		return ERR_PTR(ret);

	/* Initialize the control handler. */
	v4l2_ctrl_handler_init(&bru->ctrls, 1);
	v4l2_ctrl_new_std(&bru->ctrls, &bru_ctrl_ops, V4L2_CID_BG_COLOR,
			  0, 0xffffff, 1, 0);

	bru->bgcolor = 0;

	bru->entity.subdev.ctrl_handler = &bru->ctrls;

	if (bru->ctrls.error) {
		dev_err(vsp2->dev, "bru: failed to initialize controls\n");
		ret = bru->ctrls.error;
		vsp2_entity_destroy(&bru->entity);
		return ERR_PTR(ret);
	}

	return bru;
}
