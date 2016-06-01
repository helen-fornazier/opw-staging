/*
 * vimc-sensor.c Virtual Media Controller Driver
 *
 * Copyright (C) 2016 Helen Koike F. <helen.fornazier@gmail.com>
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
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-tpg.h>

#include "vimc-sensor.h"

#define VIMC_SEN_FRAME_MAX_WIDTH 4096

struct vimc_sen_device {
	struct vimc_ent_subdevice vsd;
	struct tpg_data tpg;
	struct task_struct *kthread_sen;
	u8 *frame;
	/* The active format */
	struct v4l2_mbus_framefmt mbus_format;
	int frame_size;
};

static int vimc_sen_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	struct vimc_sen_device *vsen = v4l2_get_subdevdata(sd);

	/* Check if it's the end of enumeration or if it is a valid pad
	 * The last element of the pix map has all values equal to zero
	 * and the bytes per pixel is never zero in a valid entry
	 */
	if (!vimc_pix_map_list[code->index].bpp ||
	    code->pad >= vsen->vsd.sd.entity.num_pads)
		return -EINVAL;

	code->code = vimc_pix_map_list[code->index].code;

	return 0;
}

static int vimc_sen_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct vimc_sen_device *vsen = v4l2_get_subdevdata(sd);
	const struct vimc_pix_map *vpix;

	/* Check if it is a valid pad */
	if (fse->pad >= vsen->vsd.sd.entity.num_pads)
		return -EINVAL;

	/* Only accept code in the pix map table */
	vpix = vimc_pix_map_by_code(fse->code);
	if (!vpix)
		return -EINVAL;

	fse->min_width = MIN_WIDTH;
	fse->max_width = MAX_WIDTH;
	fse->min_height = MIN_HEIGHT;
	fse->max_height = MAX_HEIGHT;

	return 0;
}

static int vimc_sen_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *format)
{
	struct vimc_sen_device *vsen = v4l2_get_subdevdata(sd);

	format->format = vsen->mbus_format;

	return 0;
}

static void vimc_sen_tpg_s_format(struct vimc_sen_device *vsen)
{
	const struct vimc_pix_map *vpix =
				vimc_pix_map_by_code(vsen->mbus_format.code);

	tpg_reset_source(&vsen->tpg, vsen->mbus_format.width,
			 vsen->mbus_format.height, vsen->mbus_format.field);
	tpg_s_bytesperline(&vsen->tpg, 0,
			   vsen->mbus_format.width * vpix->bpp);
	tpg_s_buf_height(&vsen->tpg, vsen->mbus_format.height);
	tpg_s_fourcc(&vsen->tpg, vpix->pixelformat);
	/* TODO: check why the tpg_s_field need this third argument if
	 * it is already receiving the field
	 */
	tpg_s_field(&vsen->tpg, vsen->mbus_format.field,
		    vsen->mbus_format.field == V4L2_FIELD_ALTERNATE);
	tpg_s_colorspace(&vsen->tpg, vsen->mbus_format.colorspace);
	tpg_s_ycbcr_enc(&vsen->tpg, vsen->mbus_format.ycbcr_enc);
	tpg_s_quantization(&vsen->tpg, vsen->mbus_format.quantization);
	tpg_s_xfer_func(&vsen->tpg, vsen->mbus_format.xfer_func);
}

static const struct v4l2_subdev_pad_ops vimc_sen_pad_ops = {
	.enum_mbus_code		= vimc_sen_enum_mbus_code,
	.enum_frame_size	= vimc_sen_enum_frame_size,
	.get_fmt		= vimc_sen_get_fmt,
	/* TODO: Add support to other formats */
	.set_fmt		= vimc_sen_get_fmt,
};

static int vimc_thread_sen(void *data)
{
	unsigned int i;
	struct vimc_sen_device *vsen = data;

	set_freezable();

	for (;;) {
		try_to_freeze();
		if (kthread_should_stop())
			break;

		tpg_fill_plane_buffer(&vsen->tpg, V4L2_STD_PAL, 0, vsen->frame);

		/* Send the frame to all source pads */
		for (i = 0; i < vsen->vsd.sd.entity.num_pads; i++) {
			struct media_pad *pad = &vsen->vsd.sd.entity.pads[i];

			vimc_propagate_frame(vsen->vsd.sd.v4l2_dev->mdev->dev,
					     pad, vsen->frame);
		}

		/* 60 frames per second */
		schedule_timeout_interruptible(HZ/60);
	}

	return 0;
}

static int vimc_sen_s_stream(struct v4l2_subdev *sd, int enable)
{
	int ret;
	struct vimc_sen_device *vsen = v4l2_get_subdevdata(sd);

	if (enable) {
		const struct vimc_pix_map *vpix;

		if (vsen->kthread_sen)
			return -EINVAL;

		/* Calculate the frame size */
		vpix = vimc_pix_map_by_code(vsen->mbus_format.code);
		vsen->frame_size = vsen->mbus_format.width * vpix->bpp *
				   vsen->mbus_format.height;

		/* Allocate the frame buffer. Use vmalloc to be able to
		 * allocate a large amount of memory
		 */
		vsen->frame = vmalloc(vsen->frame_size);
		if (!vsen->frame)
			return -ENOMEM;

		/* Initialize the image generator thread */
		vsen->kthread_sen = kthread_run(vimc_thread_sen, vsen,
					"%s-sen", vsen->vsd.sd.v4l2_dev->name);
		if (IS_ERR(vsen->kthread_sen)) {
			v4l2_err(vsen->vsd.sd.v4l2_dev,
				 "kernel_thread() failed\n");
			vfree(vsen->frame);
			vsen->frame = NULL;
			return PTR_ERR(vsen->kthread_sen);
		}

		/* configure the test pattern generator */
		vimc_sen_tpg_s_format(vsen);
	} else {
		if (!vsen->kthread_sen)
			return -EINVAL;

		/* Stop image generator */
		ret = kthread_stop(vsen->kthread_sen);
		vsen->kthread_sen = NULL;

		vfree(vsen->frame);
		vsen->frame = NULL;
		return ret;
	}

	return 0;
}

struct v4l2_subdev_video_ops vimc_sen_video_ops = {
	.s_stream = vimc_sen_s_stream,
};

static const struct v4l2_subdev_ops vimc_sen_ops = {
	.pad = &vimc_sen_pad_ops,
	.video = &vimc_sen_video_ops,
};

static void vimc_sen_destroy(struct vimc_ent_device *ved)
{
	struct vimc_sen_device *vsen = container_of(ved, struct vimc_sen_device,
						    vsd.ved);

	tpg_free(&vsen->tpg);
	vimc_ent_sd_cleanup(&vsen->vsd);
}

struct vimc_ent_device *vimc_sen_create(struct v4l2_device *v4l2_dev,
					const char *const name,
					u16 num_pads,
					const unsigned long *pads_flag)
{
	int ret;
	unsigned int i;
	struct vimc_sen_device *vsen;
	struct vimc_ent_subdevice *vsd;

	/* check if all pads are sources */
	for (i = 0; i < num_pads; i++)
		if (!(pads_flag[i] & MEDIA_PAD_FL_SOURCE))
			return ERR_PTR(-EINVAL);

	vsd = vimc_ent_sd_init(sizeof(struct vimc_sen_device),
			       name, MEDIA_ENT_F_CAM_SENSOR,
			       num_pads, pads_flag, &vimc_sen_ops,
			       vimc_sen_destroy);
	if (IS_ERR(vsd))
		return (struct vimc_ent_device *)vsd;

	vsen = container_of(vsd, struct vimc_sen_device, vsd);

	/* Set the active frame format (this is hardcoded for now) */
	vsen->mbus_format.width = 640;
	vsen->mbus_format.height = 480;
	vsen->mbus_format.code = MEDIA_BUS_FMT_RGB888_1X24;
	vsen->mbus_format.field = V4L2_FIELD_NONE;
	vsen->mbus_format.colorspace = V4L2_COLORSPACE_SRGB;
	vsen->mbus_format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	vsen->mbus_format.xfer_func = V4L2_XFER_FUNC_SRGB;

	/* Initialize the test pattern generator */
	tpg_init(&vsen->tpg, vsen->mbus_format.width,
		 vsen->mbus_format.height);
	ret = tpg_alloc(&vsen->tpg, VIMC_SEN_FRAME_MAX_WIDTH);
	if (ret)
		goto err_clean_vsd;

	/* Register the subdev with the v4l2 and the media framework */
	ret = v4l2_device_register_subdev(v4l2_dev, &vsen->vsd.sd);
	if (ret) {
		dev_err(vsen->vsd.sd.v4l2_dev->mdev->dev,
			"subdev register failed (err=%d)\n", ret);
		goto err_free_tpg;
	}

	return &vsen->vsd.ved;

err_free_tpg:
	tpg_free(&vsen->tpg);
err_clean_vsd:
	vimc_ent_sd_cleanup(vsd);

	return ERR_PTR(ret);
}
