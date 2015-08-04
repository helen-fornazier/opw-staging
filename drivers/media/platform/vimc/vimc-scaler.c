/*
 * vimc-scaler.c Virtual Media Controller Driver
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

#include "vimc-scaler.h"

/* TODO: add this as a parameter of this module */
#define VIMC_SCA_MULTIPLIER 3

struct vimc_sca_device {
	struct vimc_ent_subdevice vsd;
	unsigned int mult;
	/* The active format */
	struct v4l2_mbus_framefmt sink_mbus_fmt;
	unsigned int src_width;
	unsigned int src_height;
	/* Values calculated when the stream starts */
	u8 *src_frame;
	unsigned int src_frame_size;
	unsigned int src_line_size;
	unsigned int bpp;
};

static int vimc_sca_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	struct vimc_sca_device *vsca = v4l2_get_subdevdata(sd);

	/* Check if it is a valid pad */
	if (code->pad >= vsca->vsd.sd.entity.num_pads)
		return -EINVAL;

	code->code = vsca->sink_mbus_fmt.code;

	return 0;
}

static int vimc_sca_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct vimc_sca_device *vsca = v4l2_get_subdevdata(sd);
	struct media_pad *pad;

	/* Check if it is a valid pad */
	if (fse->pad >= vsca->vsd.sd.entity.num_pads)
		return -EINVAL;

	/* TODO: Add support to other formats sizes */

	pad = &vsca->vsd.sd.entity.pads[fse->pad];
	if ((pad->flags & MEDIA_PAD_FL_SOURCE)) {
		fse->min_width = vsca->src_width;
		fse->max_width = vsca->src_width;
		fse->min_height = vsca->src_height;
		fse->max_height = vsca->src_height;
	} else if ((pad->flags & MEDIA_PAD_FL_SINK)) {
		fse->min_width = vsca->sink_mbus_fmt.width;
		fse->max_width = vsca->sink_mbus_fmt.width;
		fse->min_height = vsca->sink_mbus_fmt.height;
		fse->max_height = vsca->sink_mbus_fmt.height;
	} else
		return -EINVAL;

	return 0;
}

static int vimc_sca_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct vimc_sca_device *vsca = v4l2_get_subdevdata(sd);
	struct media_pad *pad;

	/* Check if it is a valid pad */
	if (format->pad >= vsca->vsd.sd.entity.num_pads)
		return -EINVAL;

	pad = &vsca->vsd.sd.entity.pads[format->pad];
	if ((pad->flags & MEDIA_PAD_FL_SOURCE)) {
		format->format = vsca->sink_mbus_fmt;
		format->format.width = vsca->src_width;
		format->format.height = vsca->src_height;
	} else if ((pad->flags & MEDIA_PAD_FL_SINK))
		format->format = vsca->sink_mbus_fmt;
	else
		return -EINVAL;

	return 0;
}

static int vimc_sca_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct vimc_sca_device *vsca = v4l2_get_subdevdata(sd);
	const struct vimc_pix_map *vpix;

	/* TODO: Add support for try format */
	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		return -EINVAL;

	/* Do not change the format while stream is on */
	if (vsca->src_frame)
		return -EINVAL;

	/* Do not change the format of the source pad, it is propagated
	 * from the sink*/
	if (vsca->vsd.sd.entity.pads[format->pad].flags
	    & MEDIA_PAD_FL_SOURCE) {
		format->format = vsca->sink_mbus_fmt;
		format->format.width = vsca->src_width;
		format->format.height = vsca->src_height;
		return 0;
	}

	/* Don't accept a code that is not on the table
	 * or are in bayer format */
	vpix = vimc_pix_map_by_code(format->format.code);
	if (vpix && !vpix->bayer)
		vsca->sink_mbus_fmt.code = format->format.code;
	else
		format->format.code = vsca->sink_mbus_fmt.code;

	vimc_ent_sd_set_fsize(&vsca->sink_mbus_fmt, cfg, format);

	/* Update the source sizes */
	vsca->src_width = vsca->sink_mbus_fmt.width * vsca->mult;
	vsca->src_height = vsca->sink_mbus_fmt.height * vsca->mult;

	return 0;
}

static const struct v4l2_subdev_pad_ops vimc_sca_pad_ops = {
	.enum_mbus_code		= vimc_sca_enum_mbus_code,
	.enum_frame_size	= vimc_sca_enum_frame_size,
	.get_fmt		= vimc_sca_get_fmt,
	.set_fmt		= vimc_sca_set_fmt,
};

static int vimc_sca_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vimc_sca_device *vsca = v4l2_get_subdevdata(sd);

	if (enable) {
		const struct vimc_pix_map *vpix;

		if (vsca->src_frame)
			return -EINVAL;

		/* Save the bytes per pixel of the sink */
		vpix = vimc_pix_map_by_code(vsca->sink_mbus_fmt.code);
		/* This should never be NULL, as we won't allow any format
		 * other then the ones in the vimc_pix_map_list table */
		BUG_ON(!vpix);
		vsca->bpp = vpix->bpp;

		/* Calculate the width in bytes of the src frame */
		vsca->src_line_size = vsca->src_width * vsca->bpp;

		/* Calculate the frame size of the source pad */
		vsca->src_frame_size = vsca->src_line_size * vsca->src_height;

		/* Allocate the frame buffer. Use vmalloc to be able to
		 * allocate a large amount of memory*/
		vsca->src_frame = vmalloc(vsca->src_frame_size);
		if (!vsca->src_frame)
			return -ENOMEM;

		/* Turn the stream on in the subdevices directly connected */
		if (vimc_pipeline_s_stream(&vsca->vsd.sd.entity, 1)) {
			vfree(vsca->src_frame);
			vsca->src_frame = NULL;
			return -EINVAL;
		}
	} else {
		if (!vsca->src_frame)
			return -EINVAL;
		vfree(vsca->src_frame);
		vsca->src_frame = NULL;
		vimc_pipeline_s_stream(&vsca->vsd.sd.entity, 0);
	}

	return 0;
}

struct v4l2_subdev_video_ops vimc_sca_video_ops = {
	.s_stream = vimc_sca_s_stream,
};

static const struct v4l2_subdev_ops vimc_sca_ops = {
	.pad = &vimc_sca_pad_ops,
	.video = &vimc_sca_video_ops,
};

static void vimc_sca_fill_pix(u8 *const ptr,
			      const u8 *const pixel,
			      const unsigned int bpp)
{
	unsigned int i;

	/* copy the pixel to the pointer */
	for (i = 0; i < bpp; i++)
		ptr[i] = pixel[i];
}

static void vimc_sca_scale_pix(const struct vimc_sca_device *const vsca,
			       const unsigned int lin, const unsigned int col,
			       const u8 *const sink_frame)
{
	unsigned int i, j, index;
	const u8 *pixel;

	/* Point to the pixel value in position (lin, col) in the sink frame */
	index = VIMC_FRAME_INDEX(lin, col,
				 vsca->sink_mbus_fmt.width,
				 vsca->bpp);
	pixel = &sink_frame[index];

	dev_dbg(vsca->vsd.dev,
		"sca: %s: --- scale_pix sink pos %dx%d, index %d ---\n",
		vsca->vsd.sd.name, lin, col, index);

	/* point to the place we are going to put the first pixel
	 * in the scaled src frame */
	index = VIMC_FRAME_INDEX(lin * vsca->mult, col * vsca->mult,
				 vsca->src_width, vsca->bpp);

	dev_dbg(vsca->vsd.dev, "sca: %s: scale_pix src pos %dx%d, index %d\n",
		vsca->vsd.sd.name, lin * vsca->mult, col * vsca->mult, index);

	/* Repeat this pixel mult times */
	for (i = 0; i < vsca->mult; i++) {
		/* Iterate though each beginning of a
		 * pixel repetition in a line */
		for (j = 0; j < vsca->mult * vsca->bpp; j += vsca->bpp) {
			dev_dbg(vsca->vsd.dev,
				"sca: %s: sca: scale_pix src pos %d\n",
				vsca->vsd.sd.name, index + j);

			/* copy the pixel to the position index + j */
			vimc_sca_fill_pix(&vsca->src_frame[index + j],
					  pixel, vsca->bpp);
		}

		/* move the index to the next line */
		index += vsca->src_line_size;
	}
}

static void vimc_sca_fill_src_frame(const struct vimc_sca_device *const vsca,
				    const u8 *const sink_frame)
{
	unsigned int i, j;

	/* Scale each pixel from the original sink frame */
	/* TODO: implement scale down, only scale up is supported for now */
	for (i = 0; i < vsca->sink_mbus_fmt.height; i++)
		for (j = 0; j < vsca->sink_mbus_fmt.width; j++)
			vimc_sca_scale_pix(vsca, i, j, sink_frame);
}

static void vimc_sca_process_frame(struct vimc_ent_device *ved,
				   struct media_pad *sink,
				   const void *sink_frame)
{
	unsigned int i;
	struct vimc_sca_device *vsca = container_of(ved, struct vimc_sca_device,
						    vsd.ved);

	/* If the stream in this node is not active, just return */
	if (!vsca->src_frame)
		return;

	vimc_sca_fill_src_frame(vsca, sink_frame);

	/* Propagate the frame thought all source pads */
	for (i = 0; i < vsca->vsd.sd.entity.num_pads; i++) {
		struct media_pad *pad = &vsca->vsd.sd.entity.pads[i];

		if (pad->flags & MEDIA_PAD_FL_SOURCE)
			vimc_propagate_frame(vsca->vsd.dev,
					     pad, vsca->src_frame);
	}
};

static void vimc_sca_destroy(struct vimc_ent_device *ved)
{
	struct vimc_sca_device *vsca = container_of(ved, struct vimc_sca_device,
						    vsd.ved);

	vimc_ent_sd_cleanup(&vsca->vsd);
}

struct vimc_ent_device *vimc_sca_create(struct v4l2_device *v4l2_dev,
					const char *const name,
					u16 num_pads,
					const unsigned long *pads_flag)
{
	int ret;
	struct vimc_sca_device *vsca;
	struct vimc_ent_subdevice *vsd;

	vsd = vimc_ent_sd_init(sizeof(struct vimc_sca_device),
			       v4l2_dev, name, num_pads, pads_flag,
			       &vimc_sca_ops, vimc_sca_destroy);
	if (IS_ERR(vsd))
		return (struct vimc_ent_device *)vsd;

	vsca = container_of(vsd, struct vimc_sca_device, vsd);

	/* Set the default active frame format (this is hardcoded for now) */
	vsca->sink_mbus_fmt.width = 640;
	vsca->sink_mbus_fmt.height = 480;
	vsca->sink_mbus_fmt.code = MEDIA_BUS_FMT_RGB888_1X24;
	vsca->sink_mbus_fmt.field = V4L2_FIELD_NONE;
	vsca->sink_mbus_fmt.colorspace = V4L2_COLORSPACE_SRGB;
	vsca->sink_mbus_fmt.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	vsca->sink_mbus_fmt.xfer_func = V4L2_XFER_FUNC_SRGB;

	/* Set the scaler multiplier factor */
	vsca->mult = VIMC_SCA_MULTIPLIER;

	/* Calculate the width and the height of the src frame */
	vsca->src_width = vsca->sink_mbus_fmt.width * vsca->mult;
	vsca->src_height = vsca->sink_mbus_fmt.height * vsca->mult;

	/* Set the process frame callback */
	vsca->vsd.ved.process_frame = vimc_sca_process_frame;

	/* Register the subdev with the v4l2 and the media framework */
	ret = v4l2_device_register_subdev(vsca->vsd.v4l2_dev, &vsca->vsd.sd);
	if (ret) {
		dev_err(vsca->vsd.dev,
			"subdev register failed (err=%d)\n", ret);

		vimc_ent_sd_cleanup(vsd);

		return ERR_PTR(ret);
	}

	return &vsca->vsd.ved;
}
