// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Video DMA
 *
 * Copyright (C) 2013-2015 Ideas on Board
 * Copyright (C) 2013-2015 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <linux/dma/xilinx_dma.h>
#include <linux/lcm.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

#include <media/v4l2-common.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include "xilinx-dma.h"
#include "xilinx-vipp.h"

#define XVIP_DMA_DEF_WIDTH		1920
#define XVIP_DMA_DEF_HEIGHT		1080

/* Minimum and maximum widths are expressed in bytes */
#define XVIP_DMA_MIN_WIDTH		1U
#define XVIP_DMA_MAX_WIDTH		65535U
#define XVIP_DMA_MIN_HEIGHT		1U
#define XVIP_DMA_MAX_HEIGHT		8191U

/* -----------------------------------------------------------------------------
 * Helper functions
 */

static inline bool xvip_dma_is_s2mm(const struct xvip_dma *dma)
{
	return dma->queue.type == V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

struct xvip_dma_format {
	unsigned int code;
	u32 fourcc;
};

static const struct xvip_dma_format xvip_dma_video_formats[] = {
	{ MEDIA_BUS_FMT_UYVY8_1X16, V4L2_PIX_FMT_YUYV },
	{ MEDIA_BUS_FMT_VUY8_1X24, V4L2_PIX_FMT_YUV24 },
	{ MEDIA_BUS_FMT_Y8_1X8, V4L2_PIX_FMT_GREY },
	{ MEDIA_BUS_FMT_SRGGB8_1X8, V4L2_PIX_FMT_SRGGB8 },
	{ MEDIA_BUS_FMT_SGRBG8_1X8, V4L2_PIX_FMT_SGRBG8 },
	{ MEDIA_BUS_FMT_SGBRG8_1X8, V4L2_PIX_FMT_SGBRG8 },
	{ MEDIA_BUS_FMT_SBGGR8_1X8, V4L2_PIX_FMT_SBGGR8 },
	{ MEDIA_BUS_FMT_Y12_1X12, V4L2_PIX_FMT_Y12 },
};

static const struct xvip_dma_format *xvip_dma_get_format_by_fourcc(u32 fourcc)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(xvip_dma_video_formats); ++i) {
		const struct xvip_dma_format *format = &xvip_dma_video_formats[i];

		if (format->fourcc == fourcc)
			return format;
	}

	return NULL;
}

static struct v4l2_subdev *
xvip_dma_remote_subdev(struct media_pad *local, u32 *pad)
{
	struct media_pad *remote;

	remote = media_pad_remote_pad_first(local);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return NULL;

	if (pad)
		*pad = remote->index;

	return media_entity_to_v4l2_subdev(remote->entity);
}

static int xvip_dma_verify_format(struct xvip_dma *dma,
				  struct v4l2_subdev *subdev, u32 pad)
{
	const struct v4l2_mbus_framefmt *fmt;
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(subdev);

	fmt = v4l2_subdev_state_get_format(state, pad);

	if (!fmt ||
	    dma->fmtinfo->code != fmt->code ||
	    dma->format.height != fmt->height ||
	    dma->format.width != fmt->width)
		ret = -EINVAL;

	v4l2_subdev_unlock_state(state);

	return ret;
}

/*
 * Validate the link between the DMA video node and the connected subdev. For
 * S2MM the video node is the link's sink and the framework calls this
 * operation directly. For MM2S the video node is the link's source, and the
 * remote subdev's v4l2_subdev_link_validate() delegates the validation to
 * this operation.
 */
static int xvip_dma_link_validate(struct media_link *link)
{
	struct media_pad *remote_pad;
	struct xvip_dma *dma;

	if (is_media_entity_v4l2_video_device(link->sink->entity)) {
		dma = to_xvip_dma(media_entity_to_video_device(link->sink->entity));
		remote_pad = link->source;
	} else {
		dma = to_xvip_dma(media_entity_to_video_device(link->source->entity));
		remote_pad = link->sink;
	}

	if (!is_media_entity_v4l2_subdev(remote_pad->entity))
		return -EPIPE;

	return xvip_dma_verify_format(dma,
				      media_entity_to_v4l2_subdev(remote_pad->entity),
				      remote_pad->index);
}

static const struct media_entity_operations xvip_dma_media_ops = {
	.link_validate = xvip_dma_link_validate,
};

/* -----------------------------------------------------------------------------
 * Pipeline Stream Management
 */

/**
 * xvip_pipeline_start_stop - Start or stop streaming on a pipeline
 * @pipe: The pipeline
 * @start: Start (when true) or stop (when false) the pipeline
 *
 * Start or stop streaming on the subdev directly connected to the video node.
 *
 * Return: 0 if successful, or the return value of the failed
 * v4l2_subdev_enable_streams() operation otherwise.
 */
static int xvip_pipeline_start_stop(struct media_pipeline *pipe, bool start)
{
	struct media_pipeline_pad_iter iter;
	struct media_pad *pad;

	media_pipeline_for_each_pad(pipe, &iter, pad) {
		struct v4l2_subdev *subdev;
		struct xvip_dma *dma;
		u32 rpad;
		int ret;

		if (pad->entity->function != MEDIA_ENT_F_IO_V4L)
			continue;

		dma = to_xvip_dma(media_entity_to_video_device(pad->entity));

		if (!xvip_dma_is_s2mm(dma))
			continue;

		subdev = xvip_dma_remote_subdev(&dma->pad, &rpad);
		if (!subdev)
			return -EPIPE;

		if (start)
			ret = v4l2_subdev_enable_streams(subdev, rpad,
							 BIT_ULL(0));
		else
			ret = v4l2_subdev_disable_streams(subdev, rpad,
							  BIT_ULL(0));

		if (start && ret < 0)
			return ret;
	}

	return 0;
}

/**
 * xvip_pipeline_set_stream - Enable/disable streaming on a pipeline
 * @dma: The DMA engine whose stream state changed
 * @on: Turn the stream on when true or off when false
 *
 * The pipeline is shared between all the MM2S and S2MM DMA engines connected
 * to it. While the stream state of DMA engines can be controlled
 * independently, pipelines have a shared stream state that enable or disable
 * all entities in the pipeline. The subdevs may only stream while every DMA
 * engine in the pipeline is streaming. For this reason the pipeline is only
 * started when the last DMA engine in the pipeline starts streaming, and
 * stopped when the first DMA engine stops streaming.
 *
 * Return: 0 if successful, or the return value of the failed
 * v4l2_subdev_enable_streams() operation otherwise. Stopping the pipeline
 * never fails. The pipeline state is not updated when the operation fails.
 */
static int xvip_pipeline_set_stream(struct xvip_dma *dma, bool on)
{
	struct media_pipeline *pipe = video_device_pipeline(&dma->video);
	struct media_pipeline_pad_iter iter;
	unsigned int num_streaming = 0;
	unsigned int num_dmas = 0;
	struct media_pad *pad;
	int ret = 0;

	mutex_lock(&dma->xdev->pipeline_lock);

	dma->streaming = on;

	/*
	 * Count the DMA engines in the pipeline and how many of them are
	 * streaming.
	 */
	media_pipeline_for_each_pad(pipe, &iter, pad) {
		struct xvip_dma *d;

		if (pad->entity->function != MEDIA_ENT_F_IO_V4L)
			continue;

		d = to_xvip_dma(media_entity_to_video_device(pad->entity));

		num_dmas++;
		if (d->streaming)
			num_streaming++;
	}

	if (on) {
		/* Start the pipeline when the last DMA engine starts. */
		if (num_streaming == num_dmas) {
			ret = xvip_pipeline_start_stop(pipe, true);
			if (ret < 0)
				dma->streaming = false;
		}
	} else {
		/*
		 * Stop the pipeline when the first DMA engine stops: this DMA
		 * engine leaving the fully streaming state means all the
		 * others are still streaming.
		 */
		if (num_streaming == num_dmas - 1)
			xvip_pipeline_start_stop(pipe, false);
	}

	mutex_unlock(&dma->xdev->pipeline_lock);
	return ret;
}

static int xvip_pipeline_validate(struct media_pipeline *pipe)
{
	struct media_pipeline_pad_iter iter;
	unsigned int num_mm2s = 0;
	unsigned int num_s2mm = 0;
	struct media_pad *pad;

	/* Locate the video nodes in the pipeline. */
	media_pipeline_for_each_pad(pipe, &iter, pad) {
		struct xvip_dma *dma;

		if (pad->entity->function != MEDIA_ENT_F_IO_V4L)
			continue;

		dma = to_xvip_dma(media_entity_to_video_device(pad->entity));

		if (xvip_dma_is_s2mm(dma))
			num_s2mm++;
		else
			num_mm2s++;
	}

	/* We need exactly one S2MM and zero or one MM2S DMA. */
	if (num_s2mm != 1 || num_mm2s > 1)
		return -EPIPE;

	return 0;
}

/* -----------------------------------------------------------------------------
 * videobuf2 queue operations
 */

/**
 * struct xvip_dma_buffer - Video DMA buffer
 * @buf: vb2 buffer base object
 * @queue: buffer list entry in the DMA engine queued buffers list
 * @dma: DMA channel that uses the buffer
 */
struct xvip_dma_buffer {
	struct vb2_v4l2_buffer buf;
	struct list_head queue;
	struct xvip_dma *dma;
};

#define to_xvip_dma_buffer(vb)	container_of(vb, struct xvip_dma_buffer, buf)

static void xvip_dma_complete(void *param)
{
	struct xvip_dma_buffer *buf = param;
	struct xvip_dma *dma = buf->dma;

	spin_lock(&dma->queued_lock);
	list_del(&buf->queue);
	spin_unlock(&dma->queued_lock);

	buf->buf.field = V4L2_FIELD_NONE;
	buf->buf.sequence = dma->sequence++;
	buf->buf.vb2_buf.timestamp = ktime_get_ns();
	vb2_set_plane_payload(&buf->buf.vb2_buf, 0, dma->format.sizeimage);
	vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_DONE);
}

static int
xvip_dma_queue_setup(struct vb2_queue *vq,
		     unsigned int *nbuffers, unsigned int *nplanes,
		     unsigned int sizes[], struct device *alloc_devs[])
{
	struct xvip_dma *dma = vb2_get_drv_priv(vq);

	/* Make sure the image size is large enough. */
	if (*nplanes)
		return sizes[0] < dma->format.sizeimage ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = dma->format.sizeimage;

	return 0;
}

static int xvip_dma_buffer_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct xvip_dma *dma = vb2_get_drv_priv(vb->vb2_queue);
	struct xvip_dma_buffer *buf = to_xvip_dma_buffer(vbuf);

	buf->dma = dma;

	return 0;
}

static void xvip_dma_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct xvip_dma *dma = vb2_get_drv_priv(vb->vb2_queue);
	const struct v4l2_format_info *finfo =
		v4l2_format_info(dma->format.pixelformat);
	struct xvip_dma_buffer *buf = to_xvip_dma_buffer(vbuf);
	struct dma_async_tx_descriptor *desc;
	dma_addr_t addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	u32 flags;

	DEFINE_RAW_FLEX(struct dma_interleaved_template, xt, sgl, 1);

	if (xvip_dma_is_s2mm(dma)) {
		flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
		xt->dir = DMA_DEV_TO_MEM;
		xt->src_sgl = false;
		xt->dst_sgl = true;
		xt->dst_start = addr;
	} else {
		flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
		xt->dir = DMA_MEM_TO_DEV;
		xt->src_sgl = true;
		xt->dst_sgl = false;
		xt->src_start = addr;
	}

	xt->frame_size = 1;
	xt->sgl[0].size = dma->format.width * finfo->bpp[0] / finfo->bpp_div[0];
	xt->sgl[0].icg = dma->format.bytesperline - xt->sgl[0].size;
	xt->numf = dma->format.height;

	desc = dmaengine_prep_interleaved_dma(dma->dma, xt, flags);
	if (!desc) {
		dev_err(dma->xdev->dev, "Failed to prepare DMA transfer\n");
		vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}
	desc->callback = xvip_dma_complete;
	desc->callback_param = buf;

	spin_lock_irq(&dma->queued_lock);
	list_add_tail(&buf->queue, &dma->queued_bufs);
	spin_unlock_irq(&dma->queued_lock);

	dmaengine_submit(desc);

	if (vb2_is_streaming(&dma->queue))
		dma_async_issue_pending(dma->dma);
}

static int xvip_dma_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct xvip_dma *dma = vb2_get_drv_priv(vq);
	struct xvip_composite_device *xdev = dma->xdev;
	struct xvip_dma_buffer *buf, *nbuf;
	int ret;

	dma->sequence = 0;

	/*
	 * Start streaming on the pipeline. No link touching an entity in the
	 * pipeline can be activated or deactivated once streaming is started.
	 *
	 * The pipeline lock makes the pipeline start and the first start check
	 * atomic with respect to the other video nodes of the pipeline.
	 */
	mutex_lock(&xdev->pipeline_lock);

	ret = video_device_pipeline_alloc_start(&dma->video);

	/*
	 * Validate the pipeline topology when the pipeline is started for the
	 * first time.
	 */
	if (!ret) {
		struct media_pipeline *pipe = video_device_pipeline(&dma->video);

		if (pipe->start_count == 1) {
			ret = xvip_pipeline_validate(pipe);
			if (ret < 0)
				video_device_pipeline_stop(&dma->video);
		}
	}

	mutex_unlock(&xdev->pipeline_lock);

	if (ret < 0)
		goto error;

	/* Start the DMA engine. This must be done before starting the blocks
	 * in the pipeline to avoid DMA synchronization issues.
	 */
	dma_async_issue_pending(dma->dma);

	/* Start the pipeline. */
	ret = xvip_pipeline_set_stream(dma, true);
	if (ret < 0)
		goto error_stop;

	return 0;

error_stop:
	video_device_pipeline_stop(&dma->video);

error:
	dmaengine_terminate_all(dma->dma);

	/* Give back all queued buffers to videobuf2. */
	spin_lock_irq(&dma->queued_lock);
	list_for_each_entry_safe(buf, nbuf, &dma->queued_bufs, queue) {
		vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_QUEUED);
		list_del(&buf->queue);
	}
	spin_unlock_irq(&dma->queued_lock);

	return ret;
}

static void xvip_dma_stop_streaming(struct vb2_queue *vq)
{
	struct xvip_dma *dma = vb2_get_drv_priv(vq);
	struct xvip_dma_buffer *buf, *nbuf;

	/* Stop the pipeline. */
	xvip_pipeline_set_stream(dma, false);

	/* Stop and reset the DMA engine. */
	dmaengine_terminate_all(dma->dma);

	/* Mark the pipeline as being stopped. */
	video_device_pipeline_stop(&dma->video);

	/* Give back all queued buffers to videobuf2. */
	spin_lock_irq(&dma->queued_lock);
	list_for_each_entry_safe(buf, nbuf, &dma->queued_bufs, queue) {
		vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_ERROR);
		list_del(&buf->queue);
	}
	spin_unlock_irq(&dma->queued_lock);
}

static const struct vb2_ops xvip_dma_queue_qops = {
	.queue_setup = xvip_dma_queue_setup,
	.buf_prepare = xvip_dma_buffer_prepare,
	.buf_queue = xvip_dma_buffer_queue,
	.start_streaming = xvip_dma_start_streaming,
	.stop_streaming = xvip_dma_stop_streaming,
};

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int
xvip_dma_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct v4l2_fh *vfh = file_to_v4l2_fh(file);
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);

	cap->capabilities |= dma->xdev->v4l2_caps;

	strscpy(cap->driver, "xilinx-vipp", sizeof(cap->driver));
	strscpy(cap->card, dma->video.name, sizeof(cap->card));

	return 0;
}

static int
xvip_dma_enum_format(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	unsigned int index = f->index;

	for (unsigned int i = 0; i < ARRAY_SIZE(xvip_dma_video_formats); ++i) {
		const struct xvip_dma_format *format = &xvip_dma_video_formats[i];

		if (f->mbus_code && f->mbus_code != format->code)
			continue;

		if (index-- == 0) {
			f->pixelformat = format->fourcc;
			return 0;
		}
	}

	return -EINVAL;
}

static int
xvip_dma_get_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file_to_v4l2_fh(file);
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);

	format->fmt.pix = dma->format;

	return 0;
}

static void
__xvip_dma_try_format(struct xvip_dma *dma, struct v4l2_pix_format *pix,
		      const struct xvip_dma_format **fmtinfo)
{
	const struct v4l2_format_info *finfo;
	const struct xvip_dma_format *info;
	unsigned int min_width_bytes;
	unsigned int max_width_bytes;
	unsigned int min_bytesperline;
	unsigned int max_bytesperline;
	unsigned int width_bytes;
	unsigned int align_bytes;
	unsigned int bytesperline;

	/* Retrieve format information and select the default format if the
	 * requested format isn't supported.
	 */
	info = xvip_dma_get_format_by_fourcc(pix->pixelformat);
	if (!info)
		info = xvip_dma_get_format_by_fourcc(V4L2_PIX_FMT_YUYV);
	finfo = v4l2_format_info(info->fourcc);

	pix->pixelformat = info->fourcc;
	pix->field = V4L2_FIELD_NONE;

	/* The transfer alignment requirements are expressed in bytes. Compute
	 * the minimum and maximum values, clamp the requested width and convert
	 * it back to pixels.
	 */
	align_bytes = lcm(dma->align, finfo->bpp[0]);
	min_width_bytes = roundup(XVIP_DMA_MIN_WIDTH, align_bytes);
	max_width_bytes = rounddown(XVIP_DMA_MAX_WIDTH, align_bytes);
	width_bytes = pix->width * finfo->bpp[0] / finfo->bpp_div[0];
	width_bytes = rounddown(width_bytes, align_bytes);

	pix->width = clamp(width_bytes, min_width_bytes, max_width_bytes) *
		     finfo->bpp_div[0] / finfo->bpp[0];
	pix->height = clamp(pix->height, XVIP_DMA_MIN_HEIGHT,
			    XVIP_DMA_MAX_HEIGHT);

	/* Clamp the requested bytes per line value. */
	min_bytesperline = pix->width * finfo->bpp[0] / finfo->bpp_div[0];
	max_bytesperline = rounddown(XVIP_DMA_MAX_WIDTH, dma->align);
	bytesperline = rounddown(pix->bytesperline, dma->align);

	pix->bytesperline = clamp(bytesperline, min_bytesperline,
				  max_bytesperline);
	pix->sizeimage = pix->bytesperline * pix->height;

	if (fmtinfo)
		*fmtinfo = info;
}

static int
xvip_dma_try_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file_to_v4l2_fh(file);
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);

	__xvip_dma_try_format(dma, &format->fmt.pix, NULL);
	return 0;
}

static int
xvip_dma_set_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file_to_v4l2_fh(file);
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);
	const struct xvip_dma_format *info;

	__xvip_dma_try_format(dma, &format->fmt.pix, &info);

	if (vb2_is_busy(&dma->queue))
		return -EBUSY;

	dma->format = format->fmt.pix;
	dma->fmtinfo = info;

	return 0;
}

static const struct v4l2_ioctl_ops xvip_dma_ioctl_ops = {
	.vidioc_querycap		= xvip_dma_querycap,
	.vidioc_enum_fmt_vid_cap	= xvip_dma_enum_format,
	.vidioc_enum_fmt_vid_out	= xvip_dma_enum_format,
	.vidioc_g_fmt_vid_cap		= xvip_dma_get_format,
	.vidioc_g_fmt_vid_out		= xvip_dma_get_format,
	.vidioc_s_fmt_vid_cap		= xvip_dma_set_format,
	.vidioc_s_fmt_vid_out		= xvip_dma_set_format,
	.vidioc_try_fmt_vid_cap		= xvip_dma_try_format,
	.vidioc_try_fmt_vid_out		= xvip_dma_try_format,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
};

/* -----------------------------------------------------------------------------
 * V4L2 file operations
 */

static const struct v4l2_file_operations xvip_dma_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

/* -----------------------------------------------------------------------------
 * Xilinx Video DMA Core
 */

int xvip_dma_init(struct xvip_composite_device *xdev, struct xvip_dma *dma,
		  enum v4l2_buf_type type, unsigned int port)
{
	char name[16];
	int ret;

	dma->xdev = xdev;
	dma->port = port;
	mutex_init(&dma->lock);
	INIT_LIST_HEAD(&dma->queued_bufs);
	spin_lock_init(&dma->queued_lock);

	dma->fmtinfo = xvip_dma_get_format_by_fourcc(V4L2_PIX_FMT_YUYV);
	v4l2_fill_pixfmt(&dma->format, dma->fmtinfo->fourcc,
			 XVIP_DMA_DEF_WIDTH, XVIP_DMA_DEF_HEIGHT);
	dma->format.colorspace = V4L2_COLORSPACE_SRGB;
	dma->format.field = V4L2_FIELD_NONE;

	/* Initialize the media entity... */
	dma->pad.flags = type == V4L2_BUF_TYPE_VIDEO_CAPTURE
		       ? MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&dma->video.entity, 1, &dma->pad);
	if (ret < 0)
		goto error;

	/* ... and the video node... */
	dma->video.fops = &xvip_dma_fops;
	dma->video.entity.ops = &xvip_dma_media_ops;
	dma->video.v4l2_dev = &xdev->v4l2_dev;
	dma->video.queue = &dma->queue;
	snprintf(dma->video.name, sizeof(dma->video.name), "%pOFn %s %u",
		 xdev->dev->of_node,
		 type == V4L2_BUF_TYPE_VIDEO_CAPTURE ? "output" : "input",
		 port);
	dma->video.vfl_type = VFL_TYPE_VIDEO;
	dma->video.vfl_dir = type == V4L2_BUF_TYPE_VIDEO_CAPTURE
			   ? VFL_DIR_RX : VFL_DIR_TX;
	dma->video.release = video_device_release_empty;
	dma->video.ioctl_ops = &xvip_dma_ioctl_ops;
	dma->video.lock = &dma->lock;
	dma->video.device_caps = V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
		dma->video.device_caps |= V4L2_CAP_VIDEO_CAPTURE;
	else
		dma->video.device_caps |= V4L2_CAP_VIDEO_OUTPUT;

	video_set_drvdata(&dma->video, dma);

	/* ... and the buffers queue... */
	/* Don't enable VB2_READ and VB2_WRITE, as using the read() and write()
	 * V4L2 APIs would be inefficient. Testing on the command line with a
	 * 'cat /dev/video?' thus won't be possible, but given that the driver
	 * anyway requires a test tool to setup the pipeline before any video
	 * stream can be started, requiring a specific V4L2 test tool as well
	 * instead of 'cat' isn't really a drawback.
	 */
	dma->queue.type = type;
	dma->queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dma->queue.lock = &dma->lock;
	dma->queue.drv_priv = dma;
	dma->queue.buf_struct_size = sizeof(struct xvip_dma_buffer);
	dma->queue.ops = &xvip_dma_queue_qops;
	dma->queue.mem_ops = &vb2_dma_contig_memops;
	dma->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
				   | V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	dma->queue.dev = dma->xdev->dev;
	ret = vb2_queue_init(&dma->queue);
	if (ret < 0) {
		dev_err(dma->xdev->dev, "failed to initialize VB2 queue\n");
		goto error;
	}

	/* ... and the DMA channel. */
	snprintf(name, sizeof(name), "port%u", port);
	dma->dma = dma_request_chan(dma->xdev->dev, name);
	if (IS_ERR(dma->dma)) {
		ret = dev_err_probe(dma->xdev->dev, PTR_ERR(dma->dma),
				    "no VDMA channel found\n");
		goto error;
	}

	dma->align = 1 << dma->dma->device->copy_align;

	ret = video_register_device(&dma->video, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		dev_err(dma->xdev->dev, "failed to register video device\n");
		goto error;
	}

	return 0;

error:
	xvip_dma_cleanup(dma);
	return ret;
}

void xvip_dma_cleanup(struct xvip_dma *dma)
{
	if (video_is_registered(&dma->video))
		video_unregister_device(&dma->video);

	if (!IS_ERR_OR_NULL(dma->dma))
		dma_release_channel(dma->dma);

	media_entity_cleanup(&dma->video.entity);

	mutex_destroy(&dma->lock);
}
