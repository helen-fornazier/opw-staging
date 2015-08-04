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

#include <linux/vmalloc.h>
#include <linux/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "vimc-debayer.h"

/* TODO: add this as a parameter in a V4L2 subdev control
 * NOTE: the window size need to be an odd number, as the main pixel stays in
 * the center of it, otherwise the next odd number is considered
 */
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
	struct v4l2_mbus_framefmt sink_fmt;
	u32 src_code;
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
		.order = { { VIMC_DEB_BLUE, VIMC_DEB_GREEN },
			   { VIMC_DEB_GREEN, VIMC_DEB_RED } }
	},
	{
		.code = MEDIA_BUS_FMT_SGBRG8_1X8,
		.order = { { VIMC_DEB_GREEN, VIMC_DEB_BLUE },
			   { VIMC_DEB_RED, VIMC_DEB_GREEN } }
	},
	{
		.code = MEDIA_BUS_FMT_SGRBG8_1X8,
		.order = { { VIMC_DEB_GREEN, VIMC_DEB_RED },
			   { VIMC_DEB_BLUE, VIMC_DEB_GREEN } }
	},
	{
		.code = MEDIA_BUS_FMT_SRGGB8_1X8,
		.order = { { VIMC_DEB_RED, VIMC_DEB_GREEN },
			   { VIMC_DEB_GREEN, VIMC_DEB_BLUE } }
	},
	{
		.code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.order = { { VIMC_DEB_BLUE, VIMC_DEB_GREEN },
			   { VIMC_DEB_GREEN, VIMC_DEB_RED } }
	},
	{
		.code = MEDIA_BUS_FMT_SGBRG10_1X10,
		.order = { { VIMC_DEB_GREEN, VIMC_DEB_BLUE },
			   { VIMC_DEB_RED, VIMC_DEB_GREEN } }
	},
	{
		.code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.order = { { VIMC_DEB_GREEN, VIMC_DEB_RED },
			   { VIMC_DEB_BLUE, VIMC_DEB_GREEN } }
	},
	{
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.order = { { VIMC_DEB_RED, VIMC_DEB_GREEN },
			   { VIMC_DEB_GREEN, VIMC_DEB_BLUE } }
	},
	{
		.code = MEDIA_BUS_FMT_SBGGR12_1X12,
		.order = { { VIMC_DEB_BLUE, VIMC_DEB_GREEN },
			   { VIMC_DEB_GREEN, VIMC_DEB_RED } }
	},
	{
		.code = MEDIA_BUS_FMT_SGBRG12_1X12,
		.order = { { VIMC_DEB_GREEN, VIMC_DEB_BLUE },
			   { VIMC_DEB_RED, VIMC_DEB_GREEN } }
	},
	{
		.code = MEDIA_BUS_FMT_SGRBG12_1X12,
		.order = { { VIMC_DEB_GREEN, VIMC_DEB_RED },
			   { VIMC_DEB_BLUE, VIMC_DEB_GREEN } }
	},
	{
		.code = MEDIA_BUS_FMT_SRGGB12_1X12,
		.order = { { VIMC_DEB_RED, VIMC_DEB_GREEN },
			   { VIMC_DEB_GREEN, VIMC_DEB_BLUE } }
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

	/* Check if it's the end of enumeration or if it is a valid pad */
	if (code->index || code->pad >= vdeb->vsd.sd.entity.num_pads)
		return -EINVAL;

	if (code->pad)
		code->code = vdeb->src_code;
	else
		code->code = vdeb->sink_fmt.code;

	return 0;
}

static int vimc_deb_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct vimc_deb_device *vdeb = v4l2_get_subdevdata(sd);

	/* Check if it is a valid pad */
	if (fse->pad >= vdeb->vsd.sd.entity.num_pads)
		return -EINVAL;

	fse->min_width = MIN_WIDTH;
	fse->max_width = MAX_WIDTH;
	fse->min_height = MIN_HEIGHT;
	fse->max_height = MAX_HEIGHT;

	return 0;
}

static int vimc_deb_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct vimc_deb_device *vdeb = v4l2_get_subdevdata(sd);

	/* Check if it is a valid pad */
	if (format->pad >= vdeb->vsd.sd.entity.num_pads)
		return -EINVAL;

	/* Get the current sink format */
	format->format = (format->which == V4L2_SUBDEV_FORMAT_TRY) ?
			 *v4l2_subdev_get_try_format(sd, cfg, 0) :
			 vdeb->sink_fmt;

	/* Set the right code for the source pad */
	if (format->pad)
		format->format.code = vdeb->src_code;

	return 0;
}

static void vimc_deb_adjust_sink_fmt(struct v4l2_mbus_framefmt *fmt,
				     const struct v4l2_mbus_framefmt *ref_fmt)
{
	const struct vimc_deb_pix_map *vpix;

	/* Don't accept a code that is not on the debayer table */
	vpix = vimc_deb_pix_map_by_code(fmt->code);
	if (!vpix)
		fmt->code = ref_fmt->code;

	fmt->width = clamp_t(u32, fmt->width, MIN_WIDTH, MAX_WIDTH);
	fmt->height = clamp_t(u32, fmt->height, MIN_HEIGHT, MAX_HEIGHT);
	/* We don't support changing the colorspace for now */
	/* TODO: add support for others
	 */
	fmt->colorspace = ref_fmt->colorspace;
	fmt->ycbcr_enc = ref_fmt->ycbcr_enc;
	fmt->quantization = ref_fmt->quantization;
	fmt->xfer_func = ref_fmt->xfer_func;
}

static int vimc_deb_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct vimc_deb_device *vdeb = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *ref_sink;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		/* Do not change the format while stream is on */
		if (vdeb->src_frame)
			return -EINVAL;

		ref_sink = &vdeb->sink_fmt;
	} else
		ref_sink = v4l2_subdev_get_try_format(sd, cfg, 0);

	/* Do not change the format of the source pad,
	 * it is propagated from the sink
	 */
	if (format->pad) {
		format->format = *ref_sink;
		/* TODO: Add support for other formats */
		format->format.code = vdeb->src_code;

		return 0;
	}

	/* Set the new format in the sink pad */
	vimc_deb_adjust_sink_fmt(&format->format, ref_sink);

	/* Apply format */
	*ref_sink = format->format;

	return 0;
}

static const struct v4l2_subdev_pad_ops vimc_deb_pad_ops = {
	.enum_mbus_code		= vimc_deb_enum_mbus_code,
	.enum_frame_size	= vimc_deb_enum_frame_size,
	.get_fmt		= vimc_deb_get_fmt,
	.set_fmt		= vimc_deb_set_fmt,
};

static void vimc_deb_set_rgb_mbus_fmt_rgb888_1x24(struct vimc_deb_device *vdeb,
						  unsigned int lin,
						  unsigned int col,
						  unsigned int rgb[3])
{
	unsigned int i, index;

	index = VIMC_FRAME_INDEX(lin, col, vdeb->sink_fmt.width, 3);
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
		vpix = vimc_pix_map_by_code(vdeb->src_code);
		vdeb->src_frame_size = vdeb->sink_fmt.width *
				       vpix->bpp * vdeb->sink_fmt.height;

		/* Save the bytes per pixel of the sink */
		vpix = vimc_pix_map_by_code(vdeb->sink_fmt.code);
		vdeb->sink_bpp = vpix->bpp;

		/* Get the corresponding pixel map from the table */
		vdeb->sink_pix_map = vimc_deb_pix_map_by_code(
						vdeb->sink_fmt.code);

		/* Allocate the frame buffer. Use vmalloc to be able to
		 * allocate a large amount of memory
		 */
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
	 * pixel as the center)
	 */
	seek = vdeb->mean_win_size / 2;

	/* Sum the values of the colors in the mean window */

	dev_dbg(vdeb->vsd.sd.v4l2_dev->mdev->dev,
		"deb: %s: --- Calc pixel %dx%d, window mean %d, seek %d ---\n",
		vdeb->vsd.sd.name, lin, col, vdeb->sink_fmt.height, seek);

	/* Iterate through all the lines in the mean window, start
	 * with zero if the pixel is outside the frame and don't pass
	 * the height when the pixel is in the bottom border of the
	 * frame
	 */
	for (wlin = seek > lin ? 0 : lin - seek;
	     wlin < lin + seek + 1 && wlin < vdeb->sink_fmt.height;
	     wlin++) {

		/* Iterate through all the columns in the mean window, start
		 * with zero if the pixel is outside the frame and don't pass
		 * the width when the pixel is in the right border of the
		 * frame
		 */
		for (wcol = seek > col ? 0 : col - seek;
		     wcol < col + seek + 1 && wcol < vdeb->sink_fmt.width;
		     wcol++) {
			enum vimc_deb_rgb_colors color;
			unsigned int index;

			/* Check which color this pixel is */
			color = vdeb->sink_pix_map->order[wlin % 2][wcol % 2];

			index = VIMC_FRAME_INDEX(wlin, wcol,
						 vdeb->sink_fmt.width,
						 vdeb->sink_bpp);

			dev_dbg(vdeb->vsd.sd.v4l2_dev->mdev->dev,
				"deb: %s: RGB CALC: frame index %d, win pos %dx%d, color %d\n",
				vdeb->vsd.sd.name, index, wlin, wcol, color);

			/* Get its value */
			rgb[color] = rgb[color] +
				vimc_deb_get_val(&frame[index], vdeb->sink_bpp);

			/* Save how many values we already added */
			n_rgb[color]++;

			dev_dbg(vdeb->vsd.sd.v4l2_dev->mdev->dev,
				"deb: %s: RGB CALC: val %d, n %d\n",
				vdeb->vsd.sd.name, rgb[color], n_rgb[color]);
		}
	}

	/* Calculate the mean */
	for (i = 0; i < 3; i++) {
		dev_dbg(vdeb->vsd.sd.v4l2_dev->mdev->dev,
			"deb: %s: PRE CALC: %dx%d Color %d, val %d, n %d\n",
			vdeb->vsd.sd.name, lin, col, i, rgb[i], n_rgb[i]);

		if (n_rgb[i])
			rgb[i] = rgb[i] / n_rgb[i];

		dev_dbg(vdeb->vsd.sd.v4l2_dev->mdev->dev,
			"deb: %s: FINAL CALC: %dx%d Color %d, val %d\n",
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

	for (i = 0; i < vdeb->sink_fmt.height; i++)
		for (j = 0; j < vdeb->sink_fmt.width; j++) {
			vimc_deb_calc_rgb_sink(vdeb, sink_frame, i, j, rgb);
			vdeb->set_rgb_src(vdeb, i, j, rgb);
		}

	/* Propagate the frame thought all source pads */
	for (i = 1; i < vdeb->vsd.sd.entity.num_pads; i++) {
		struct media_pad *pad = &vdeb->vsd.sd.entity.pads[i];

		vimc_propagate_frame(vdeb->vsd.sd.v4l2_dev->mdev->dev,
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
	unsigned int i;
	struct vimc_deb_device *vdeb;
	struct vimc_ent_subdevice *vsd;

	/* check pads types
	 * NOTE: we support a single sink pad and multiple source pads
	 * the sink pad must be the first
	 */
	if (num_pads < 2 || !(pads_flag[0] & MEDIA_PAD_FL_SINK))
		return ERR_PTR(-EINVAL);
	/* check if the rest of pads are sources */
	for (i = 1; i < num_pads; i++)
		if (!(pads_flag[i] & MEDIA_PAD_FL_SOURCE))
			return ERR_PTR(-EINVAL);

	vsd = vimc_ent_sd_init(sizeof(struct vimc_deb_device),
			       name, MEDIA_ENT_F_ATV_DECODER,
			       num_pads, pads_flag, &vimc_deb_ops,
			       vimc_deb_destroy);
	if (IS_ERR(vsd))
		return (struct vimc_ent_device *)vsd;

	vdeb = container_of(vsd, struct vimc_deb_device, vsd);

	vdeb->sink_fmt.width = 640;
	vdeb->sink_fmt.height = 480;
	vdeb->sink_fmt.field = V4L2_FIELD_NONE;
	vdeb->sink_fmt.colorspace = V4L2_COLORSPACE_SRGB;
	vdeb->sink_fmt.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	vdeb->sink_fmt.xfer_func = V4L2_XFER_FUNC_SRGB;
	vdeb->sink_fmt.code = MEDIA_BUS_FMT_SRGGB8_1X8;
	/* TODO: Add support for more output formats, we only support
	 * RGB8888 for now
	 * NOTE: the src format is always the same as the sink, except
	 * for the code
	 */
	vdeb->src_code = MEDIA_BUS_FMT_RGB888_1X24;
	vdeb->set_rgb_src = vimc_deb_set_rgb_mbus_fmt_rgb888_1x24;

	/* Set the window size to calculate the mean */
	vdeb->mean_win_size = VIMC_DEB_MEAN_WINDOW_SIZE;

	/* Set the process frame callback */
	vdeb->vsd.ved.process_frame = vimc_deb_process_frame;

	/* Register the subdev with the v4l2 and the media framework */
	ret = v4l2_device_register_subdev(v4l2_dev, &vdeb->vsd.sd);
	if (ret) {
		dev_err(vdeb->vsd.sd.v4l2_dev->mdev->dev,
			"subdev register failed (err=%d)\n", ret);

		vimc_ent_sd_cleanup(vsd);

		return ERR_PTR(ret);
	}

	return &vdeb->vsd.ved;
}
