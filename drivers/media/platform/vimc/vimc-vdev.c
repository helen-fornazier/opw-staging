/*
 * vimc-vdev.c Virtual Media Controller Driver
 *
 * Copyright (C) 2015-2017 Helen Koike <helen.fornazier@gmail.com>
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

#include <linux/component.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-tpg.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

#include "vimc-common.h"

#define VIMC_CAP_DRV_NAME "vimc-capture"
#define VIMC_OUT_DRV_NAME "vimc-output"

struct vimc_vdev_device {
	struct vimc_ent_device ved;
	struct video_device vdev;
	struct device *dev;
	struct v4l2_pix_format format;
	struct vb2_queue queue;
	struct list_head buf_list;
	/*
	 * NOTE: in a real driver, a spin lock must be used to access the
	 * queue because the frames are generated from a hardware interruption
	 * and the isr is not allowed to sleep.
	 * Even if it is not necessary a spinlock in the vimc driver, we
	 * use it here as a code reference
	 */
	spinlock_t qlock;
	struct mutex lock;
	u32 sequence;
	struct media_pipeline pipe;
	struct tpg_data tpg;
	struct task_struct *kthread;
	bool is_output;
};

static const struct v4l2_pix_format fmt_default = {
	.width = 640,
	.height = 480,
	.pixelformat = V4L2_PIX_FMT_RGB24,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_DEFAULT,
};

struct vimc_vdev_buffer {
	/*
	 * struct vb2_v4l2_buffer must be the first element
	 * the videobuf2 framework will allocate this struct based on
	 * buf_struct_size and use the first sizeof(struct vb2_buffer) bytes of
	 * memory as a vb2_buffer
	 */
	struct vb2_v4l2_buffer vb2;
	struct list_head list;
};

static int vimc_vdev_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	struct vimc_vdev_device *vv = video_drvdata(file);

	if (vv->is_output) {
		strlcpy(cap->driver, VIMC_OUT_DRV_NAME, sizeof(cap->driver));
		strlcpy(cap->card, VIMC_OUT_DRV_NAME, sizeof(cap->card));
	} else {
		strlcpy(cap->driver, VIMC_CAP_DRV_NAME, sizeof(cap->driver));
		strlcpy(cap->card, VIMC_CAP_DRV_NAME, sizeof(cap->card));
	}
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", vv->vdev.v4l2_dev->name);

	return 0;
}

static void vimc_vdev_get_format(struct vimc_ent_device *ved,
				 struct v4l2_pix_format *fmt)
{
	struct vimc_vdev_device *vv = container_of(ved, struct vimc_vdev_device,
						   ved);

	*fmt = vv->format;
}

static int vimc_vdev_g_fmt_vid(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct vimc_vdev_device *vv = video_drvdata(file);

	f->fmt.pix = vv->format;

	return 0;
}

static int vimc_vdev_try_fmt_vid(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct v4l2_pix_format *format = &f->fmt.pix;
	const struct vimc_pix_map *vpix;

	format->width = clamp_t(u32, format->width, VIMC_FRAME_MIN_WIDTH,
				VIMC_FRAME_MAX_WIDTH) & ~1;
	format->height = clamp_t(u32, format->height, VIMC_FRAME_MIN_HEIGHT,
				 VIMC_FRAME_MAX_HEIGHT) & ~1;

	/* Don't accept a pixelformat that is not on the table */
	vpix = vimc_pix_map_by_pixelformat(format->pixelformat);
	if (!vpix) {
		format->pixelformat = fmt_default.pixelformat;
		vpix = vimc_pix_map_by_pixelformat(format->pixelformat);
	}
	/* TODO: Add support for custom bytesperline values */
	format->bytesperline = format->width * vpix->bpp;
	format->sizeimage = format->bytesperline * format->height;

	if (format->field == V4L2_FIELD_ANY)
		format->field = fmt_default.field;

	vimc_colorimetry_clamp(format);

	return 0;
}

static int vimc_vdev_s_fmt_vid(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct vimc_vdev_device *vv = video_drvdata(file);

	/* Do not change the format while stream is on */
	if (vb2_is_busy(&vv->queue))
		return -EBUSY;

	vimc_vdev_try_fmt_vid(file, priv, f);

	dev_dbg(vv->dev, "%s: format update: "
		"old:%dx%d (0x%x, %d, %d, %d, %d) "
		"new:%dx%d (0x%x, %d, %d, %d, %d)\n", vv->vdev.name,
		/* old */
		vv->format.width, vv->format.height,
		vv->format.pixelformat, vv->format.colorspace,
		vv->format.quantization, vv->format.xfer_func,
		vv->format.ycbcr_enc,
		/* new */
		f->fmt.pix.width, f->fmt.pix.height,
		f->fmt.pix.pixelformat,	f->fmt.pix.colorspace,
		f->fmt.pix.quantization, f->fmt.pix.xfer_func,
		f->fmt.pix.ycbcr_enc);

	vv->format = f->fmt.pix;

	return 0;
}

static int vimc_vdev_enum_fmt_vid(struct file *file, void *priv,
				  struct v4l2_fmtdesc *f)
{
	const struct vimc_pix_map *vpix = vimc_pix_map_by_index(f->index);

	if (!vpix)
		return -EINVAL;

	f->pixelformat = vpix->pixelformat;

	return 0;
}

static int vimc_vdev_enum_framesizes(struct file *file, void *fh,
				     struct v4l2_frmsizeenum *fsize)
{
	const struct vimc_pix_map *vpix;

	if (fsize->index)
		return -EINVAL;

	/* Only accept code in the pix map table */
	vpix = vimc_pix_map_by_code(fsize->pixel_format);
	if (!vpix)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise.min_width = VIMC_FRAME_MIN_WIDTH;
	fsize->stepwise.max_width = VIMC_FRAME_MAX_WIDTH;
	fsize->stepwise.min_height = VIMC_FRAME_MIN_HEIGHT;
	fsize->stepwise.max_height = VIMC_FRAME_MAX_HEIGHT;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.step_height = 2;

	return 0;
}

static const struct v4l2_file_operations vimc_vdev_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read           = vb2_fop_read,
	.write		= vb2_fop_write,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap,
};

static int vimc_cap_enum_input(struct file *file, void *priv,
                              struct v4l2_input *i)
{
       /* We only have one input */
       if (i->index > 0)
               return -EINVAL;

       i->type = V4L2_INPUT_TYPE_CAMERA;
       strlcpy(i->name, "VIMC capture", sizeof(i->name));

       return 0;
}

static int vimc_cap_g_input(struct file *file, void *priv, unsigned int *i)
{
       /* We only have one input */
       *i = 0;
       return 0;
}

static int vimc_cap_s_input(struct file *file, void *priv, unsigned int i)
{
       /* We only have one input */
       return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops vimc_vdev_ioctl_ops = {
	.vidioc_enum_input = vimc_cap_enum_input,
	.vidioc_g_input = vimc_cap_g_input,
	.vidioc_s_input = vimc_cap_s_input,

	.vidioc_querycap = vimc_vdev_querycap,

	.vidioc_g_fmt_vid_cap = vimc_vdev_g_fmt_vid,
	.vidioc_s_fmt_vid_cap = vimc_vdev_s_fmt_vid,
	.vidioc_try_fmt_vid_cap = vimc_vdev_try_fmt_vid,
	.vidioc_enum_fmt_vid_cap = vimc_vdev_enum_fmt_vid,
	.vidioc_enum_framesizes = vimc_vdev_enum_framesizes,

	.vidioc_g_fmt_vid_out = vimc_vdev_g_fmt_vid,
	.vidioc_s_fmt_vid_out = vimc_vdev_s_fmt_vid,
	.vidioc_try_fmt_vid_out = vimc_vdev_try_fmt_vid,
	.vidioc_enum_fmt_vid_out = vimc_vdev_enum_fmt_vid,
	.vidioc_enum_framesizes = vimc_vdev_enum_framesizes,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static void vimc_vdev_return_all_buffers(struct vimc_vdev_device *vv,
					 enum vb2_buffer_state state)
{
	struct vimc_vdev_buffer *vbuf, *node;

	spin_lock(&vv->qlock);

	list_for_each_entry_safe(vbuf, node, &vv->buf_list, list) {
		list_del(&vbuf->list);
		vb2_buffer_done(&vbuf->vb2.vb2_buf, state);
	}

	spin_unlock(&vv->qlock);
}

static void vimc_cap_process_frame(struct vimc_ent_device *ved,
				   struct media_pad *sink, const void *frame)
{
	struct vimc_vdev_device *vv = container_of(ved, struct vimc_vdev_device,
						   ved);
	struct vimc_vdev_buffer *vimc_buf;
	void *vbuf;

	spin_lock(&vv->qlock);

	/* Get the first entry of the list */
	vimc_buf = list_first_entry_or_null(&vv->buf_list,
					    typeof(*vimc_buf), list);
	if (!vimc_buf) {
		spin_unlock(&vv->qlock);
		return;
	}

	/* Remove this entry from the list */
	list_del(&vimc_buf->list);

	spin_unlock(&vv->qlock);

	/* Fill the buffer */
	vimc_buf->vb2.vb2_buf.timestamp = ktime_get_ns();
	vimc_buf->vb2.sequence = vv->sequence++;
	vimc_buf->vb2.field = vv->format.field;

	vbuf = vb2_plane_vaddr(&vimc_buf->vb2.vb2_buf, 0);

	if (sink)
		memcpy(vbuf, frame, vv->format.sizeimage);
	else
		tpg_fill_plane_buffer(&vv->tpg, V4L2_STD_PAL, 0, vbuf);

	/* Set it as ready */
	vb2_set_plane_payload(&vimc_buf->vb2.vb2_buf, 0,
			      vv->format.sizeimage);
	vb2_buffer_done(&vimc_buf->vb2.vb2_buf, VB2_BUF_STATE_DONE);
}

static int vimc_cap_tpg_thread(void *data)
{
	struct vimc_vdev_device *vv = data;

	set_freezable();
	set_current_state(TASK_UNINTERRUPTIBLE);

	for (;;) {
		try_to_freeze();
		if (kthread_should_stop())
			break;

		vimc_cap_process_frame(&vv->ved, NULL, NULL);

		/* 60 frames per second */
		schedule_timeout(HZ/60);
	}

	return 0;
}

static int vimc_out_thread(void *data)
{
	struct vimc_vdev_device *vv = data;
	struct vimc_vdev_buffer *vimc_buf;
	void *vbuf;

	set_freezable();
	set_current_state(TASK_UNINTERRUPTIBLE);

	for (;;) {
		try_to_freeze();
		if (kthread_should_stop())
			break;

		spin_lock(&vv->qlock);

		/* Get the first entry of the list */
		vimc_buf = list_first_entry_or_null(&vv->buf_list,
						    typeof(*vimc_buf), list);
		if (!vimc_buf) {
			spin_unlock(&vv->qlock);
			continue;
		}

		/* Remove this entry from the list */
		list_del(&vimc_buf->list);

		spin_unlock(&vv->qlock);

		vbuf = vb2_plane_vaddr(&vimc_buf->vb2.vb2_buf, 0);

		vimc_propagate_frame(&vv->vdev.entity.pads[0], vbuf);

		/* Set it as ready */
		vb2_buffer_done(&vimc_buf->vb2.vb2_buf, VB2_BUF_STATE_DONE);
	}

	return 0;
}

static void vimc_cap_tpg_s_format(struct vimc_vdev_device *vv)
{
	const struct vimc_pix_map *vpix =
			vimc_pix_map_by_pixelformat(vv->format.pixelformat);

	tpg_reset_source(&vv->tpg, vv->format.width, vv->format.height,
			 vv->format.field);
	tpg_s_bytesperline(&vv->tpg, 0, vv->format.width * vpix->bpp);
	tpg_s_buf_height(&vv->tpg, vv->format.height);
	tpg_s_fourcc(&vv->tpg, vpix->pixelformat);
	/*
	 * TODO: check why the tpg_s_field need this third argument if
	 * it is already receiving the field
	 */
	tpg_s_field(&vv->tpg, vv->format.field,
		    vv->format.field == V4L2_FIELD_ALTERNATE);
	tpg_s_colorspace(&vv->tpg, vv->format.colorspace);
	tpg_s_ycbcr_enc(&vv->tpg, vv->format.ycbcr_enc);
	tpg_s_quantization(&vv->tpg, vv->format.quantization);
	tpg_s_xfer_func(&vv->tpg, vv->format.xfer_func);
}

static int vimc_cap_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vq);
	struct media_entity *entity = &vv->vdev.entity;
	int ret;

	vv->sequence = 0;

	/* Start the media pipeline */
	ret = media_pipeline_start(entity, &vv->pipe);
	if (ret)
		goto err_ret_all_buffs;

	/* Enable streaming from the pipe */
	ret = vimc_pipeline_s_stream(&vv->vdev.entity, 1);
	if (ret < 0)
		goto err_mpipe_stop;

	if (ret == VIMC_PIPE_OPT) {
		tpg_init(&vv->tpg, vv->format.width, vv->format.height);
		ret = tpg_alloc(&vv->tpg, VIMC_FRAME_MAX_WIDTH);
		if (ret)
			/* We don't need to call vimc_pipeline_s_stream(e, 0) */
			goto err_mpipe_stop;

		vimc_cap_tpg_s_format(vv);
		vv->kthread = kthread_run(vimc_cap_tpg_thread, vv,
					  "%s-cap", vv->vdev.v4l2_dev->name);
		if (IS_ERR(vv->kthread)) {
			dev_err(vv->dev, "%s: kernel_thread() failed\n",
				vv->vdev.name);
			ret = PTR_ERR(vv->kthread);
			goto err_tpg_free;
		}
	}

	return 0;

err_tpg_free:
	tpg_free(&vv->tpg);
err_mpipe_stop:
	media_pipeline_stop(entity);
err_ret_all_buffs:
	vimc_vdev_return_all_buffers(vv, VB2_BUF_STATE_QUEUED);

	return ret;
}

/*
 * Stop the stream engine. Any remaining buffers in the stream queue are
 * dequeued and passed on to the vb2 framework marked as STATE_ERROR.
 */
static void vimc_cap_stop_streaming(struct vb2_queue *vq)
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vq);

	if (vv->kthread) {
		/* Stop image generator */
		kthread_stop(vv->kthread);
		vv->kthread = NULL;
		tpg_free(&vv->tpg);
	} else {
		/* Disable streaming from the pipe */
		vimc_pipeline_s_stream(&vv->vdev.entity, 0);
	}

	/* Stop the media pipeline */
	media_pipeline_stop(&vv->vdev.entity);

	/* Release all active buffers */
	vimc_vdev_return_all_buffers(vv, VB2_BUF_STATE_ERROR);
}

static int vimc_out_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vq);
	int ret;

	vv->kthread = kthread_run(vimc_out_thread, vv,
				  "%s-out", vv->vdev.v4l2_dev->name);
	if (IS_ERR(vv->kthread)) {
		dev_err(vv->dev, "%s: kernel_thread() failed\n",
			vv->vdev.name);
		ret = PTR_ERR(vv->kthread);
		vimc_vdev_return_all_buffers(vv, VB2_BUF_STATE_QUEUED);

		return ret;
	}

	return 0;
}

static void vimc_out_stop_streaming(struct vb2_queue *vq)
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vq);

	if (vv->kthread) {
		kthread_stop(vv->kthread);
		vv->kthread = NULL;
	}

	/* Release all active buffers */
	vimc_vdev_return_all_buffers(vv, VB2_BUF_STATE_ERROR);
}

static int vimc_vdev_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vq);

	return vv->is_output ? vimc_out_start_streaming(vq, count) :
			       vimc_cap_start_streaming(vq, count);
}

static void vimc_vdev_stop_streaming(struct vb2_queue *vq)
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vq);

	return vv->is_output ? vimc_out_stop_streaming(vq) :
			       vimc_cap_stop_streaming(vq);
}

static void vimc_vdev_buf_queue(struct vb2_buffer *vb2_buf)
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vb2_buf->vb2_queue);
	struct vimc_vdev_buffer *buf = container_of(vb2_buf,
						    struct vimc_vdev_buffer,
						    vb2.vb2_buf);

	spin_lock(&vv->qlock);
	list_add_tail(&buf->list, &vv->buf_list);
	spin_unlock(&vv->qlock);
}

static int vimc_vdev_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				 unsigned int *nplanes, unsigned int sizes[],
				 struct device *alloc_devs[])
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vq);

	if (*nplanes)
		return sizes[0] < vv->format.sizeimage ? -EINVAL : 0;
	/* We don't support multiplanes for now */
	*nplanes = 1;
	sizes[0] = vv->format.sizeimage;

	return 0;
}

static int vimc_vdev_buffer_prepare(struct vb2_buffer *vb)
{
	struct vimc_vdev_device *vv = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long size = vv->format.sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(vv->dev, "%s: buffer too small (%lu < %lu)\n",
			vv->vdev.name, vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}
	return 0;
}

static const struct vb2_ops vimc_vdev_qops = {
	.start_streaming	= vimc_vdev_start_streaming,
	.stop_streaming		= vimc_vdev_stop_streaming,
	.buf_queue		= vimc_vdev_buf_queue,
	.queue_setup		= vimc_vdev_queue_setup,
	.buf_prepare		= vimc_vdev_buffer_prepare,
	/*
	 * Since q->lock is set we can use the standard
	 * vb2_ops_wait_prepare/finish helper functions.
	 */
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static const struct media_entity_operations vimc_cap_mops = {
	.link_validate		= vimc_link_validate,
};

static void vimc_vdev_comp_unbind(struct device *comp, struct device *master,
				  void *master_data)
{
	struct vimc_ent_device *ved = dev_get_drvdata(comp);
	struct vimc_vdev_device *vv = container_of(ved, struct vimc_vdev_device,
						   ved);

	vb2_queue_release(&vv->queue);
	media_entity_cleanup(ved->ent);
	video_unregister_device(&vv->vdev);
	vimc_pads_cleanup(vv->ved.pads);
	kfree(vv);
}

static int vimc_vdev_comp_bind(struct device *comp, struct device *master,
			       void *master_data)
{
	struct v4l2_device *v4l2_dev = master_data;
	struct vimc_platform_data *pdata = comp->platform_data;
	const struct vimc_pix_map *vpix;
	struct vimc_vdev_device *vv;
	struct video_device *vdev;
	struct vb2_queue *q;
	int ret;

	/* Allocate the vimc_vdev_device struct */
	vv = kzalloc(sizeof(*vv), GFP_KERNEL);
	if (!vv)
		return -ENOMEM;

	if (!strncmp(dev_name(comp), VIMC_OUT_DRV_NAME,
					sizeof(VIMC_OUT_DRV_NAME) - 1)) {
	    vv->is_output = true;
	}

	/* Allocate the pads */
	vv->ved.pads = vimc_pads_init(1, vv->is_output ?
				(const unsigned long[1]) {MEDIA_PAD_FL_SOURCE} :
				(const unsigned long[1]) {MEDIA_PAD_FL_SINK});
	if (IS_ERR(vv->ved.pads)) {
		ret = PTR_ERR(vv->ved.pads);
		goto err_free_vv;
	}

	/* Initialize the media entity */
	vv->vdev.entity.name = pdata->entity_name;
	vv->vdev.entity.function = MEDIA_ENT_F_IO_V4L;
	ret = media_entity_pads_init(&vv->vdev.entity,
				     1, vv->ved.pads);
	if (ret)
		goto err_clean_pads;

	/* Initialize the lock */
	mutex_init(&vv->lock);

	/* Initialize the vb2 queue */
	q = &vv->queue;
	q->type = vv->is_output ? V4L2_BUF_TYPE_VIDEO_OUTPUT :
				  V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_READ | VB2_WRITE | VB2_MMAP | VB2_DMABUF;
	q->drv_priv = vv;
	q->buf_struct_size = sizeof(struct vimc_vdev_buffer);
	q->ops = &vimc_vdev_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_buffers_needed = 2;
	q->lock = &vv->lock;

	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(comp, "%s: vb2 queue init failed (err=%d)\n",
			pdata->entity_name, ret);
		goto err_clean_m_ent;
	}

	/* Initialize buffer list and its lock */
	INIT_LIST_HEAD(&vv->buf_list);
	spin_lock_init(&vv->qlock);

	/* Set default frame format */
	vv->format = fmt_default;
	vpix = vimc_pix_map_by_pixelformat(vv->format.pixelformat);
	vv->format.bytesperline = vv->format.width * vpix->bpp;
	vv->format.sizeimage = vv->format.bytesperline * vv->format.height;

	/* Fill the vimc_ent_device struct */
	vv->ved.ent = &vv->vdev.entity;
	vv->ved.process_frame = vv->is_output ? NULL : vimc_cap_process_frame;
	vv->ved.vdev_get_format = vimc_vdev_get_format;
	dev_set_drvdata(comp, &vv->ved);
	vv->dev = comp;

	/* Initialize the video_device struct */
	vdev = &vv->vdev;
	vdev->device_caps = vv->is_output ?
				V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING :
				V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	vdev->entity.ops = &vimc_cap_mops;
	vdev->release = video_device_release_empty;
	vdev->fops = &vimc_vdev_fops;
	vdev->ioctl_ops = &vimc_vdev_ioctl_ops;
	vdev->lock = &vv->lock;
	vdev->queue = q;
	vdev->v4l2_dev = v4l2_dev;
	vdev->vfl_dir = vv->is_output ? VFL_DIR_TX : VFL_DIR_RX;
	strlcpy(vdev->name, pdata->entity_name, sizeof(vdev->name));
	video_set_drvdata(vdev, &vv->ved);

	/* Register the video_device with the v4l2 and the media framework */
	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		dev_err(comp, "%s: video register failed (err=%d)\n",
			vv->vdev.name, ret);
		goto err_release_queue;
	}

	return 0;

err_release_queue:
	vb2_queue_release(q);
err_clean_m_ent:
	media_entity_cleanup(&vv->vdev.entity);
err_clean_pads:
	vimc_pads_cleanup(vv->ved.pads);
err_free_vv:
	kfree(vv);

	return ret;
}

static const struct component_ops vimc_vdev_comp_ops = {
	.bind = vimc_vdev_comp_bind,
	.unbind = vimc_vdev_comp_unbind,
};

static int vimc_vdev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vimc_vdev_comp_ops);
}

static int vimc_vdev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vimc_vdev_comp_ops);

	return 0;
}

static struct platform_driver vimc_cap_pdrv = {
	.probe		= vimc_vdev_probe,
	.remove		= vimc_vdev_remove,
	.driver		= {
		.name	= VIMC_CAP_DRV_NAME,
	},
};

static struct platform_driver vimc_out_pdrv = {
	.probe		= vimc_vdev_probe,
	.remove		= vimc_vdev_remove,
	.driver		= {
		.name	= VIMC_OUT_DRV_NAME,
	},
};

static const struct platform_device_id vimc_vdev_driver_ids[] = {
	{
		.name           = VIMC_CAP_DRV_NAME,
	},
	{
		.name           = VIMC_OUT_DRV_NAME,
	},
	{ }
};

static int __init vimc_vdev_init(void)
{
	int ret;

	ret = platform_driver_register(&vimc_cap_pdrv);
	if (ret)
		return ret;

	ret = platform_driver_register(&vimc_out_pdrv);
	if (ret) {
		platform_driver_unregister(&vimc_cap_pdrv);
		return ret;
	}

	return 0;
}

static void __init vimc_vdev_exit(void)
{
	platform_driver_unregister(&vimc_cap_pdrv);
	platform_driver_unregister(&vimc_out_pdrv);
}

module_init(vimc_vdev_init);
module_exit(vimc_vdev_exit);

MODULE_DEVICE_TABLE(platform, vimc_vdev_driver_ids);

MODULE_DESCRIPTION("Virtual Media Controller Driver (VIMC) Capture/Output");
MODULE_AUTHOR("Helen Mae Koike Fornazier <helen.fornazier@gmail.com>");
MODULE_LICENSE("GPL");
