/*
 * vimc-debayer.c Virtual Media Controller Driver
 *
 * Copyright (C) 2015 Helen Fornazier <helen.fornazier@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/freezer.h>
#include <linux/vmalloc.h>
#include <linux/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "vimc-debayer.h"

/* TODO: add this as a parameter of this module
 * NOTE: the window size need to be an odd number, as the main pixel stays in
 * the center of it, otherwise the next odd number is considered */
#define VIMC_DEB_MEAN_WINDOW_SIZE 3

enum vimc_deb_rgb_colors {
	VIMC_DEB_RED = 0,
	VIMC_DEB_GREEN = 1,
	VIMC_DEB_BLUE = 2,
};

struct vimc_deb_pix_map {
	u32 code;
	enum vimc_deb_rgb_colors order[2][2];
};

struct vimc_deb_device {
	struct vimc_ent_subdevice vsd;
	unsigned int mean_win_size;
	/* The active format */
	struct v4l2_mbus_framefmt src_mbus_fmt;
	struct v4l2_mbus_framefmt sink_mbus_fmt;
	void (*set_rgb_src)(struct vimc_deb_device *vdeb, unsigned int lin,
			    unsigned int col, unsigned int rgb[3]);
	/* Values calculated when the stream starts */
	u8 *src_frame;
	unsigned int src_frame_size;
	const struct vimc_deb_pix_map *sink_pix_map;
	unsigned int sink_bpp;
};


static const struct vimc_deb_pix_map vimc_deb_pix_map_list[] = {
	{
		.code = MEDIA_BUS_FMT_SBGGR8_1X8,
		.order = { {VIMC_DEB_BLUE, VIMC_DEB_GREEN},
			   {VIMC_DEB_GREEN, VIMC_DEB_RED} }
	},
	{
		.code = MEDIA_BUS_FMT_SGBRG8_1X8,
		.order = { {VIMC_DEB_GREEN, VIMC_DEB_BLUE},
			   {VIMC_DEB_RED, VIMC_DEB_GREEN} }
	},
	{
		.code = MEDIA_BUS_FMT_SGRBG8_1X8,
		.order = { {VIMC_DEB_GREEN, VIMC_DEB_RED},
			   {VIMC_DEB_BLUE, VIMC_DEB_GREEN} }
	},
	{
		.code = MEDIA_BUS_FMT_SRGGB8_1X8,
		.order = { {VIMC_DEB_RED, VIMC_DEB_GREEN},
			   {VIMC_DEB_GREEN, VIMC_DEB_BLUE} }
	},
	{
		.code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.order = { {VIMC_DEB_BLUE, VIMC_DEB_GREEN},
			   {VIMC_DEB_GREEN, VIMC_DEB_RED} }
	},
	{
		.code = MEDIA_BUS_FMT_SGBRG10_1X10,
		.order = { {VIMC_DEB_GREEN, VIMC_DEB_BLUE},
			   {VIMC_DEB_RED, VIMC_DEB_GREEN} }
	},
	{
		.code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.order = { {VIMC_DEB_GREEN, VIMC_DEB_RED},
			   {VIMC_DEB_BLUE, VIMC_DEB_GREEN} }
	},
	{
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.order = { {VIMC_DEB_RED, VIMC_DEB_GREEN},
			   {VIMC_DEB_GREEN, VIMC_DEB_BLUE} }
	},
	{
		.code = MEDIA_BUS_FMT_SBGGR12_1X12,
		.order = { {VIMC_DEB_BLUE, VIMC_DEB_GREEN},
			   {VIMC_DEB_GREEN, VIMC_DEB_RED} }
	},
	{
		.code = MEDIA_BUS_FMT_SGBRG12_1X12,
		.order = { {VIMC_DEB_GREEN, VIMC_DEB_BLUE},
			   {VIMC_DEB_RED, VIMC_DEB_GREEN} }
	},
	{
		.code = MEDIA_BUS_FMT_SGRBG12_1X12,
		.order = { {VIMC_DEB_GREEN, VIMC_DEB_RED},
			   {VIMC_DEB_BLUE, VIMC_DEB_GREEN} }
	},
	{
		.code = MEDIA_BUS_FMT_SRGGB12_1X12,
		.order = { {VIMC_DEB_RED, VIMC_DEB_GREEN},
			   {VIMC_DEB_GREEN, VIMC_DEB_BLUE} }
	},
};

static const struct vimc_deb_pix_map *vimc_deb_pix_map_by_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vimc_deb_pix_map_list); i++)
		if (vimc_deb_pix_map_list[i].code == code)
			return &vimc_deb_pix_map_list[i];

	return NULL;
}

static int vimc_deb_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	struct vimc_deb_device *vdeb = v4l2_get_subdevdata(sd);
	struct media_pad *pad;

	/* Check if it is a valid pad */
	if (code->pad >= vdeb->vsd.sd.entity.num_pads)
		return -EINVAL;

	pad = &vdeb->vsd.sd.entity.pads[code->pad];
	if ((pad->flags & MEDIA_PAD_FL_SOURCE))
		code->code = vdeb->src_mbus_fmt.code;
	else if ((pad->flags & MEDIA_PAD_FL_SINK))
		code->code = vdeb->sink_mbus_fmt.code;
	else
		return -EINVAL;

	return 0;
}

static int vimc_deb_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct vimc_deb_device *vdeb = v4l2_get_subdevdata(sd);
	struct media_pad *pad;

	/* Check if it is a valid pad */
	if (fse->pad >= vdeb->vsd.sd.entity.num_pads)
		return -EINVAL;

	/* TODO: Add support to other formats sizes */

	pad = &vdeb->vsd.sd.entity.pads[fse->pad];
	if ((pad->flags & MEDIA_PAD_FL_SOURCE)) {
		fse->min_width = vdeb->src_mbus_fmt.width;
		fse->max_width = vdeb->src_mbus_fmt.width;
		fse->min_height = vdeb->src_mbus_fmt.height;
		fse->max_height = vdeb->src_mbus_fmt.height;
	} else if ((pad->flags & MEDIA_PAD_FL_SINK)) {
		fse->min_width = vdeb->sink_mbus_fmt.width;
		fse->max_width = vdeb->sink_mbus_fmt.width;
		fse->min_height = vdeb->sink_mbus_fmt.height;
		fse->max_height = vdeb->sink_mbus_fmt.height;
	} else
		return -EINVAL;

	return 0;
}

static int vimc_deb_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct vimc_deb_device *vdeb = v4l2_get_subdevdata(sd);
	struct media_pad *pad;

	/* Check if it is a valid pad */
	if (format->pad >= vdeb->vsd.sd.entity.num_pads)
		return -EINVAL;

	pad = &vdeb->vsd.sd.entity.pads[format->pad];
	if ((pad->flags & MEDIA_PAD_FL_SOURCE))
		format->format = vdeb->src_mbus_fmt;
	else if ((pad->flags & MEDIA_PAD_FL_SINK))
		format->format = vdeb->sink_mbus_fmt;
	else
		return -EINVAL;

	return 0;
}

static const struct v4l2_subdev_pad_ops vimc_deb_pad_ops = {
	.enum_mbus_code		= vimc_deb_enum_mbus_code,
	.enum_frame_size	= vimc_deb_enum_frame_size,
	.get_fmt		= vimc_deb_get_fmt,
	/* TODO: Add support to other formats */
	.set_fmt		= vimc_deb_get_fmt,
};

static void vimc_deb_set_rgb_mbus_fmt_rgb888_1x24(struct vimc_deb_device *vdeb,
						  unsigned int lin,
						  unsigned int col,
						  unsigned int rgb[3])
{
	unsigned int i, index;

	index = VIMC_FRAME_INDEX(lin, col, vdeb->src_mbus_fmt.width, 3);
	for (i = 0; i < 3; i++)
		vdeb->src_frame[index + i] = rgb[i];
}

static int vimc_deb_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vimc_deb_device *vdeb = v4l2_get_subdevdata(sd);

	if (enable) {
		const struct vimc_pix_map *vpix;

		if (vdeb->src_frame)
			return -EINVAL;

		/* Calculate the frame size of the source pad */
		vpix = vimc_pix_map_by_code(vdeb->src_mbus_fmt.code);
		/* This should never be NULL, as we won't allow any format
		 * other then the ones in the vimc_pix_map_list table */
		BUG_ON(!vpix);
		vdeb->src_frame_size = vdeb->src_mbus_fmt.width *
				       vpix->bpp * vdeb->src_mbus_fmt.height;

		/* Save the bytes per pixel of the sink */
		vpix = vimc_pix_map_by_code(vdeb->sink_mbus_fmt.code);
		/* This should never be NULL, as we won't allow any format
		 * other then the ones in the vimc_pix_map_list table */
		BUG_ON(!vpix);
		vdeb->sink_bpp = vpix->bpp;

		/* Get the corresponding pixel map from the table */
		vdeb->sink_pix_map = vimc_deb_pix_map_by_code(
						vdeb->sink_mbus_fmt.code);
		/* This should never be NULL, as we won't allow any format
		 * in sink pad other then the ones in the
		 * vimc_deb_pix_map_list table */
		BUG_ON(!vdeb->sink_pix_map);

		/* Allocate the frame buffer. Use vmalloc to be able to
		 * allocate a large amount of memory*/
		vdeb->src_frame = vmalloc(vdeb->src_frame_size);
		if (!vdeb->src_frame)
			return -ENOMEM;

		/* Turn the stream on in the subdevices directly connected */
		if (vimc_pipeline_s_stream(&vdeb->vsd.sd.entity, 1)) {
			vfree(vdeb->src_frame);
			vdeb->src_frame = NULL;
			return -EINVAL;
		}

	} else {
		if (!vdeb->src_frame)
			return -EINVAL;
		vfree(vdeb->src_frame);
		vdeb->src_frame = NULL;
		vimc_pipeline_s_stream(&vdeb->vsd.sd.entity, 0);
	}

	return 0;
}

struct v4l2_subdev_video_ops vimc_deb_video_ops = {
	.s_stream = vimc_deb_s_stream,
};

static const struct v4l2_subdev_ops vimc_deb_ops = {
	.pad = &vimc_deb_pad_ops,
	.video = &vimc_deb_video_ops,
};

static unsigned int vimc_deb_get_val(const u8 *bytes,
				     const unsigned int n_bytes)
{
	unsigned int i;
	unsigned int acc = 0;

	for (i = 0; i < n_bytes; i++)
		acc = acc + (bytes[i] << (8 * i));

	return acc;
}

static void vimc_deb_calc_rgb_sink(struct vimc_deb_device *vdeb,
				   const u8 *frame,
				   const unsigned int lin,
				   const unsigned int col,
				   unsigned int rgb[3])
{
	unsigned int i, seek, wlin, wcol;
	unsigned int n_rgb[3] = {0, 0, 0};

	for (i = 0; i < 3; i++)
		rgb[i] = 0;

	/* Calculate how many we need to subtract to get to the pixel in
	 * the top left corner of the mean window (considering the current
	 * pixel as the center) */
	seek = vdeb->mean_win_size / 2;

	/* Sum the values of the colors in the mean window */

	dev_dbg(vdeb->vsd.dev,
		"deb: %s: --- Calc pixel %dx%d, window mean %d, seek %d ---\n",
		vdeb->vsd.sd.name, lin, col, vdeb->sink_mbus_fmt.height, seek);

	/* Iterate through all the lines in the mean window, start
	 * with zero if the pixel is outside the frame and don't pass
	 * the height when the pixel is in the bottom border of the
	 * frame */
	for (wlin = seek > lin ? 0 : lin - seek;
	     wlin < lin + seek + 1 && wlin < vdeb->sink_mbus_fmt.height;
	     wlin++) {

		/* Iterate through all the columns in the mean window, start
		 * with zero if the pixel is outside the frame and don't pass
		 * the width when the pixel is in the right border of the
		 * frame */
		for (wcol = seek > col ? 0 : col - seek;
		     wcol < col + seek + 1 && wcol < vdeb->sink_mbus_fmt.width;
		     wcol++) {
			enum vimc_deb_rgb_colors color;
			unsigned int index;

			/* Check which color this pixel is */
			color = vdeb->sink_pix_map->order[wlin % 2][wcol % 2];

			index = VIMC_FRAME_INDEX(wlin, wcol,
						 vdeb->sink_mbus_fmt.width,
						 vdeb->sink_bpp);

			dev_dbg(vdeb->vsd.dev,
				"deb: %s: RGB CALC: frame index %d, win pos %dx%d, color %d\n",
				vdeb->vsd.sd.name, index, wlin, wcol, color);

			/* Get its value */
			rgb[color] = rgb[color] +
				vimc_deb_get_val(&frame[index], vdeb->sink_bpp);

			/* Save how many values we already added */
			n_rgb[color]++;

			dev_dbg(vdeb->vsd.dev,
				"deb: %s: RGB CALC: val %d, n %d\n",
				vdeb->vsd.sd.name, rgb[color], n_rgb[color]);
		}
	}

	/* Calculate the mean */
	for (i = 0; i < 3; i++) {
		dev_dbg(vdeb->vsd.dev, "deb: %s: PRE CALC: %dx%d Color %d, val %d, n %d\n",
			vdeb->vsd.sd.name, lin, col, i, rgb[i], n_rgb[i]);

		if (n_rgb[i])
			rgb[i] = rgb[i] / n_rgb[i];

		dev_dbg(vdeb->vsd.dev, "deb: %s: FINAL CALC: %dx%d Color %d, val %d\n",
			vdeb->vsd.sd.name, lin, col, i, rgb[i]);
	}
}

static void vimc_deb_process_frame(struct vimc_ent_device *ved,
				   struct media_pad *sink,
				   const void *sink_frame)
{
	struct vimc_deb_device *vdeb = container_of(ved,
					struct vimc_deb_device, vsd.ved);
	unsigned int rgb[3];
	unsigned int i, j;

	/* If the stream in this node is not active, just return */
	if (!vdeb->src_frame)
		return;

	for (i = 0; i < vdeb->src_mbus_fmt.height; i++)
		for (j = 0; j < vdeb->src_mbus_fmt.width; j++) {
			vimc_deb_calc_rgb_sink(vdeb, sink_frame, i, j, rgb);
			vdeb->set_rgb_src(vdeb, i, j, rgb);
		}

	/* Propagate the frame thought all source pads */
	for (i = 0; i < vdeb->vsd.sd.entity.num_pads; i++) {
		struct media_pad *pad = &vdeb->vsd.sd.entity.pads[i];

		if (pad->flags & MEDIA_PAD_FL_SOURCE)
			vimc_propagate_frame(vdeb->vsd.dev,
					     pad, vdeb->src_frame);
	}
}

static void vimc_deb_destroy(struct vimc_ent_device *ved)
{
	struct vimc_deb_device *vdeb = container_of(ved, struct vimc_deb_device,
						    vsd.ved);

	vimc_ent_sd_cleanup(&vdeb->vsd);
}

struct vimc_ent_device *vimc_deb_create(struct v4l2_device *v4l2_dev,
					const char *const name,
					u16 num_pads,
					const unsigned long *pads_flag)
{
	int ret;
	struct vimc_deb_device *vdeb;
	struct vimc_ent_subdevice *vsd;

	vsd = vimc_ent_sd_init(sizeof(struct vimc_deb_device),
			       v4l2_dev, name, num_pads, pads_flag,
			       &vimc_deb_ops, vimc_deb_destroy);
	if (IS_ERR(vsd))
		return (struct vimc_ent_device *)vsd;

	vdeb = container_of(vsd, struct vimc_deb_device, vsd);

	/* Set the default active frame format (this is hardcoded for now) */
	vdeb->sink_mbus_fmt.width = 64;
	vdeb->sink_mbus_fmt.height = 64;
	vdeb->sink_mbus_fmt.field = V4L2_FIELD_NONE;
	vdeb->sink_mbus_fmt.colorspace = V4L2_COLORSPACE_SRGB;
	vdeb->sink_mbus_fmt.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	vdeb->sink_mbus_fmt.xfer_func = V4L2_XFER_FUNC_SRGB;
	vdeb->sink_mbus_fmt.code = MEDIA_BUS_FMT_SRGGB8_1X8;

	vdeb->src_mbus_fmt.width = 640;
	vdeb->src_mbus_fmt.height = 480;
	vdeb->src_mbus_fmt.field = V4L2_FIELD_NONE;
	vdeb->src_mbus_fmt.colorspace = V4L2_COLORSPACE_SRGB;
	vdeb->src_mbus_fmt.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	vdeb->src_mbus_fmt.xfer_func = V4L2_XFER_FUNC_SRGB;
	vdeb->src_mbus_fmt.code = MEDIA_BUS_FMT_RGB888_1X24;
	vdeb->set_rgb_src = vimc_deb_set_rgb_mbus_fmt_rgb888_1x24;

	/* Set the window size to calculate the mean */
	vdeb->mean_win_size = VIMC_DEB_MEAN_WINDOW_SIZE;

	/* Set the process frame callback */
	vdeb->vsd.ved.process_frame = vimc_deb_process_frame;

	/* Register the subdev with the v4l2 and the media framework */
	ret = v4l2_device_register_subdev(vdeb->vsd.v4l2_dev, &vdeb->vsd.sd);
	if (ret) {
		dev_err(vdeb->vsd.dev,
			"subdev register failed (err=%d)\n", ret);

		vimc_ent_sd_cleanup(vsd);

		return ERR_PTR(ret);
	}

	return &vdeb->vsd.ved;
}
