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

#include <linux/vmalloc.h>
#include <linux/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "vimc-scaler.h"

/* TODO: add this as a parameter in a V4L2 subdev control */
#define VIMC_SCA_MULTIPLIER 3

struct vimc_sca_device {
	struct vimc_ent_subdevice vsd;
	unsigned int mult;
	/* NOTE: the source fmt is the same as the sink
	 * with the width and hight multiplied by mult
	 */
	struct v4l2_mbus_framefmt sink_fmt;
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

	/* Check if it's the end of enumeration or if it is a valid pad */
	if (code->index || code->pad >= vsca->vsd.sd.entity.num_pads)
		return -EINVAL;

	code->code = vsca->sink_fmt.code;

	return 0;
}

static int vimc_sca_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct vimc_sca_device *vsca = v4l2_get_subdevdata(sd);

	/* Check if it is a valid pad */
	if (fse->pad >= vsca->vsd.sd.entity.num_pads)
		return -EINVAL;

	fse->min_width = MIN_WIDTH;
	fse->min_height = MIN_HEIGHT;

	if (fse->pad) {
		fse->max_width = MAX_WIDTH;
		fse->max_height = MAX_HEIGHT;
	} else {
		fse->max_width = MAX_WIDTH * MAX_ZOOM;
		fse->max_height = MAX_HEIGHT * MAX_ZOOM;
	}

	return 0;
}

static int vimc_sca_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct vimc_sca_device *vsca = v4l2_get_subdevdata(sd);

	/* Check if it is a valid pad */
	if (format->pad >= vsca->vsd.sd.entity.num_pads)
		return -EINVAL;

	/* Get the current sink format */
	format->format = (format->which == V4L2_SUBDEV_FORMAT_TRY) ?
			 *v4l2_subdev_get_try_format(sd, cfg, 0) :
			 vsca->sink_fmt;

	/* Scale the frame size for the source pad */
	if (format->pad) {
		format->format.width = vsca->sink_fmt.width * vsca->mult;
		format->format.height = vsca->sink_fmt.height * vsca->mult;
	}

	return 0;
}

static void vimc_sca_adjust_sink_fmt(struct v4l2_mbus_framefmt *fmt,
				     const struct v4l2_mbus_framefmt *ref_fmt)
{
	const struct vimc_pix_map *vpix;

	/* Don't accept a code that is not on the table
	 * or are in bayer format
	 */
	vpix = vimc_pix_map_by_code(fmt->code);
	if (!vpix || vpix->bayer)
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

static int vimc_sca_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct vimc_sca_device *vsca = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *ref_sink;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		/* Do not change the format while stream is on */
		if (vsca->src_frame)
			return -EINVAL;

		ref_sink = &vsca->sink_fmt;
	} else
		ref_sink = v4l2_subdev_get_try_format(sd, cfg, 0);


	/* Do not change the format of the source pad,
	 * it is propagated from the sink
	 */
	if (format->pad) {
		format->format = *ref_sink;
		format->format.width = vsca->sink_fmt.width * vsca->mult;
		format->format.height = vsca->sink_fmt.height * vsca->mult;
		return 0;
	}

	/* Set the new format in the sink pad */
	vimc_sca_adjust_sink_fmt(&format->format, ref_sink);

	/* Apply format */
	*ref_sink = format->format;

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
		vpix = vimc_pix_map_by_code(vsca->sink_fmt.code);
		vsca->bpp = vpix->bpp;

		/* Calculate the width in bytes of the src frame */
		vsca->src_line_size = vsca->sink_fmt.width *
				      vsca->mult * vsca->bpp;

		/* Calculate the frame size of the source pad */
		vsca->src_frame_size = vsca->src_line_size *
				       vsca->sink_fmt.height * vsca->mult;

		/* Allocate the frame buffer. Use vmalloc to be able to
		 * allocate a large amount of memory
		 */
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
				 vsca->sink_fmt.width,
				 vsca->bpp);
	pixel = &sink_frame[index];

	dev_dbg(vsca->vsd.sd.v4l2_dev->mdev->dev,
		"sca: %s: --- scale_pix sink pos %dx%d, index %d ---\n",
		vsca->vsd.sd.name, lin, col, index);

	/* point to the place we are going to put the first pixel
	 * in the scaled src frame
	 */
	index = VIMC_FRAME_INDEX(lin * vsca->mult, col * vsca->mult,
				 vsca->sink_fmt.width * vsca->mult, vsca->bpp);

	dev_dbg(vsca->vsd.sd.v4l2_dev->mdev->dev,
		"sca: %s: scale_pix src pos %dx%d, index %d\n",
		vsca->vsd.sd.name, lin * vsca->mult, col * vsca->mult, index);

	/* Repeat this pixel mult times */
	for (i = 0; i < vsca->mult; i++) {
		/* Iterate though each beginning of a
		 * pixel repetition in a line
		 */
		for (j = 0; j < vsca->mult * vsca->bpp; j += vsca->bpp) {
			dev_dbg(vsca->vsd.sd.v4l2_dev->mdev->dev,
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
	for (i = 0; i < vsca->sink_fmt.height; i++)
		for (j = 0; j < vsca->sink_fmt.width; j++)
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
	for (i = 1; i < vsca->vsd.sd.entity.num_pads; i++) {
		struct media_pad *pad = &vsca->vsd.sd.entity.pads[i];

		vimc_propagate_frame(vsca->vsd.sd.v4l2_dev->mdev->dev,
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
	unsigned int i;
	struct vimc_sca_device *vsca;
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

	vsd = vimc_ent_sd_init(sizeof(struct vimc_sca_device),
			       name, MEDIA_ENT_F_ATV_DECODER,
			       num_pads, pads_flag, &vimc_sca_ops,
			       vimc_sca_destroy);
	if (IS_ERR(vsd))
		return (struct vimc_ent_device *)vsd;

	vsca = container_of(vsd, struct vimc_sca_device, vsd);

	/* Set the default active frame format (this is hardcoded for now) */
	vsca->sink_fmt.width = 640;
	vsca->sink_fmt.height = 480;
	vsca->sink_fmt.code = MEDIA_BUS_FMT_RGB888_1X24;
	vsca->sink_fmt.field = V4L2_FIELD_NONE;
	vsca->sink_fmt.colorspace = V4L2_COLORSPACE_SRGB;
	vsca->sink_fmt.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	vsca->sink_fmt.xfer_func = V4L2_XFER_FUNC_SRGB;

	/* Set the scaler multiplier factor */
	vsca->mult = VIMC_SCA_MULTIPLIER;

	/* Set the process frame callback */
	vsca->vsd.ved.process_frame = vimc_sca_process_frame;

	/* Register the subdev with the v4l2 and the media framework */
	ret = v4l2_device_register_subdev(v4l2_dev, &vsca->vsd.sd);
	if (ret) {
		dev_err(vsca->vsd.sd.v4l2_dev->mdev->dev,
			"subdev register failed (err=%d)\n", ret);

		vimc_ent_sd_cleanup(vsd);

		return ERR_PTR(ret);
	}

	return &vsca->vsd.ved;
}
