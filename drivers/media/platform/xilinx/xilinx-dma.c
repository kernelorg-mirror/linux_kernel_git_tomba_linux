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
#include <linux/dma/xilinx_frmbuf.h>
#include <linux/lcm.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/xilinx-v4l2-controls.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include "xilinx-dma.h"
#include "xilinx-vip.h"
#include "xilinx-vipp.h"

#define XVIP_DMA_DEF_FORMAT		V4L2_PIX_FMT_YUYV
#define XVIP_DMA_DEF_WIDTH		1920
#define XVIP_DMA_DEF_HEIGHT		1080
#define XVIP_DMA_DEF_WIDTH_ALIGN	2
/* Minimum and maximum widths are expressed in pixels */
#define XVIP_DMA_MIN_WIDTH		1U
#define XVIP_DMA_MAX_WIDTH		65535U
#define XVIP_DMA_MIN_HEIGHT		1U
#define XVIP_DMA_MAX_HEIGHT		8191U

/*
 * Select the mode of operation for pipeline that have multiple output DMA
 * engines.
 *
 * @XVIP_DMA_MULTI_OUT_MODE_SYNC: Wait for all outputs to be started before
 *	starting the pipeline
 * @XVIP_DMA_MULTI_OUT_MODE_ASYNC: Start pipeline branches independently when
 *	outputs are started
 */
enum {
	XVIP_DMA_MULTI_OUT_MODE_SYNC = 0,
	XVIP_DMA_MULTI_OUT_MODE_ASYNC = 1,
};

static int xvip_dma_multi_out_mode = 0;
module_param_named(multi_out_mode, xvip_dma_multi_out_mode, int, 0444);
MODULE_PARM_DESC(multi_out_mode, "Multi-output DMA mode (0: sync, 1: async)");

/* -----------------------------------------------------------------------------
 * Helper functions
 */

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

static int xvip_dma_verify_format(struct xvip_dma *dma)
{
	struct v4l2_subdev_format fmt;
	struct v4l2_subdev *subdev;
	int ret;

	subdev = xvip_dma_remote_subdev(&dma->pad, &fmt.pad);
	if (!subdev)
		return -EPIPE;

	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	ret = v4l2_subdev_call(subdev, pad, get_fmt, NULL, &fmt);
	if (ret < 0)
		return ret == -ENOIOCTLCMD ? -EINVAL : ret;

	if (dma->fmtinfo->code != fmt.format.code) {
		dev_dbg(dma->xdev->dev, "%s(): code mismatch 0x%04x != 0x%04x\n",
			__func__, fmt.format.code, dma->fmtinfo->code);
		return -EINVAL;
	}

	/*
	 * Crop rectangle contains format resolution by default, and crop
	 * rectangle if s_selection is executed.
	 */
	if (dma->r.width != fmt.format.width ||
	    dma->r.height != fmt.format.height) {
		dev_dbg(dma->xdev->dev, "%s(): size mismatch %ux%u != %ux%u\n",
			__func__, fmt.format.width, fmt.format.height,
			dma->r.width, dma->r.height);
		return -EINVAL;
	}

	if (fmt.format.field != dma->format.field) {
		dev_dbg(dma->xdev->dev, "%s(): field mismatch %u != %u\n",
			__func__, fmt.format.field, dma->format.field);
		return -EINVAL;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Buffer Handling
 */

static void xvip_dma_complete(void *param)
{
	struct xvip_dma_buffer *buf = param;
	struct xvip_dma *dma = buf->dma;
	unsigned int i;
	u32 fid;
	int status;

	spin_lock(&dma->queued_lock);
	list_del(&buf->queue);
	spin_unlock(&dma->queued_lock);

	buf->buf.field = V4L2_FIELD_NONE;
	buf->buf.sequence = dma->sequence++;
	buf->buf.vb2_buf.timestamp = ktime_get_ns();

	status = xilinx_xdma_get_fid(dma->dma, buf->desc, &fid);
	if (!status) {
		if (dma->format.field == V4L2_FIELD_ALTERNATE) {
			/*
			 * fid = 1 is odd field i.e. V4L2_FIELD_TOP.
			 * fid = 0 is even field i.e. V4L2_FIELD_BOTTOM.
			 */
			buf->buf.field = fid ?
					 V4L2_FIELD_TOP : V4L2_FIELD_BOTTOM;

			if (fid == dma->prev_fid)
				buf->buf.sequence = dma->sequence++;

			buf->buf.sequence >>= 1;
			dma->prev_fid = fid;
		}
	}

	for (i = 0; i < dma->fmtinfo->num_buffers; i++) {
		u32 sizeimage = dma->format.plane_fmt[i].sizeimage;

		vb2_set_plane_payload(&buf->buf.vb2_buf, i, sizeimage);
	}

	vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_DONE);
}

static int xvip_dma_submit_buffer(struct xvip_dma_buffer *buf,
				  enum dma_transfer_direction dir,
				  dma_addr_t dma_addrs[2],
				  u32 format, unsigned int num_planes,
				  unsigned int width, unsigned int height,
				  unsigned int bpl, u32 fid)
{
	struct xvip_dma *dma = buf->dma;
	struct dma_async_tx_descriptor *desc;
	u32 flags = 0;

	if (dir == DMA_DEV_TO_MEM) {
		flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
		dma->xt.dir = DMA_DEV_TO_MEM;
		dma->xt.src_sgl = false;
		dma->xt.dst_sgl = true;
		dma->xt.dst_start = dma_addrs[0];
	} else  {
		flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
		dma->xt.dir = DMA_MEM_TO_DEV;
		dma->xt.src_sgl = true;
		dma->xt.dst_sgl = false;
		dma->xt.src_start = dma_addrs[0];
	}

	/*
	 * DMA IP supports only 2 planes, so one datachunk is sufficient
	 * to get start address of 2nd plane
	 */

	xilinx_xdma_v4l2_config(dma->dma, format);
	dma->xt.frame_size = num_planes;

	dma->sgl[0].size = width;
	dma->sgl[0].icg = bpl - width;

	/*
	 * dst_icg is the number of bytes to jump after last luma addr
	 * and before first chroma addr
	 */
	if (num_planes == 2)
		dma->sgl[0].dst_icg = dma_addrs[1] - dma_addrs[0]
				    - bpl * height;

	dma->xt.numf = height;

	desc = dmaengine_prep_interleaved_dma(dma->dma, &dma->xt, flags);
	if (!desc) {
		dev_err(dma->xdev->dev, "Failed to prepare DMA transfer\n");
		return -EINVAL;
	}
	desc->callback = xvip_dma_complete;
	desc->callback_param = buf;
	buf->desc = desc;

	xilinx_xdma_set_fid(dma->dma, desc, fid);

	spin_lock_irq(&dma->queued_lock);
	list_add_tail(&buf->queue, &dma->queued_bufs);
	spin_unlock_irq(&dma->queued_lock);

	dmaengine_submit(desc);

	return 0;
}

static void xvip_dma_submit_vb2_buffer(struct xvip_dma *dma,
				       struct xvip_dma_buffer *buf)
{
	struct vb2_buffer *vb = &buf->buf.vb2_buf;
	enum dma_transfer_direction dir;
	dma_addr_t dma_addrs[2] = { };
	unsigned int width;
	unsigned int bpl;
	u32 fid;
	int ret;

	switch (dma->queue.type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
	default:
		dir = DMA_DEV_TO_MEM;
		break;

	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		dir = DMA_MEM_TO_DEV;
		break;
	}

	bpl = dma->format.plane_fmt[0].bytesperline;

	dma_addrs[0] = vb2_dma_contig_plane_dma_addr(vb, 0);
	if (dma->fmtinfo->num_buffers == 2)
		dma_addrs[1] = vb2_dma_contig_plane_dma_addr(vb, 1);
	else if (dma->fmtinfo->num_planes == 2)
		dma_addrs[1] = dma_addrs[0] + bpl * dma->format.height;

	switch (buf->buf.field) {
	case V4L2_FIELD_TOP:
		fid = 1;
		break;
	case V4L2_FIELD_BOTTOM:
	case V4L2_FIELD_NONE:
		fid = 0;
		break;
	default:
		fid = ~0;
		break;
	}

	width = (size_t)dma->r.width * dma->fmtinfo->bytes_per_pixel[0].numerator
	      / (size_t)dma->fmtinfo->bytes_per_pixel[0].denominator;

	ret = xvip_dma_submit_buffer(buf, dir, dma_addrs, dma->format.pixelformat,
				     dma->fmtinfo->num_planes, width, dma->r.height,
				     bpl, fid);
	if (ret < 0) {
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}
}

/* -----------------------------------------------------------------------------
 * Pipeline Stream Management
 */

/*
 * Pipelines carry one or more streams, with the sources and sinks being either
 * live (such as camera sensors or HDMI connectors) or DMA engines. DMA engines
 * at the outputs of the pipeline don't accept packets on their AXI stream slave
 * interface until they are started, which may prevent the pipeline from running
 * due to back-pressure building up along the pipeline all the way to the
 * source if no IP core along the pipeline is able to drop packets. This affects
 * pipelines that have multiple output DMA engines.
 *
 * The runtime behaviour is controlled through the xvip_dma_multi_out_mode
 * parameter:
 *
 * - When set to XVIP_DMA_MULTI_OUT_MODE_SYNC, the pipeline start is delayed
 *   until all DMA engines have been started. This mode of operation is the
 *   default, and needed when the pipeline contains elements that can't drop
 *   packets.
 *
 * - When set to XVIP_DMA_MULTI_OUT_MODE_ASYNC, individual branches of the
 *   pipeline are started and stopped as output DMA engines are started and
 *   stopped. This allows capturing multiple streams independently, but only
 *   works if streams that are stopped can be blocked before they reach the DMA
 *   engine.
 */

/*
 * Start the DMA engine when the pipeline starts. This function is called for
 * all DMA engines when the pipeline starts.
 */
static int xvip_dma_start(struct xvip_dma *dma)
{
	dma_async_issue_pending(dma->dma);

	return 0;
}

/*
 * Stop the DMA engine when the pipeline stops. This function is called for all
 * DMA engines when the pipeline stops.
 */
static void xvip_dma_stop(struct xvip_dma *dma)
{
	dmaengine_terminate_all(dma->dma);
}

/**
 * xvip_pipeline_enable_branch - Enable streaming on all subdevs in a pipeline
 *	branch
 * @pipe: The pipeline
 * @dma: The DMA engine at the end of the branch
 *
 * Return: 0 for success, otherwise error code
 */
static int xvip_pipeline_enable_branch(struct xvip_pipeline *pipe,
				       struct xvip_dma *dma)
{
	struct v4l2_subdev *sd;
	u32 pad;
	int ret;

	dev_dbg(dma->xdev->dev, "Enabling streams on %s\n",
		dma->video.entity.name);

	sd = xvip_dma_remote_subdev(&dma->pad, &pad);
	if (!sd)
		return -ENXIO;

	ret = v4l2_subdev_enable_streams(sd, pad, BIT(0));
	if (ret) {
		dev_err(dma->xdev->dev, "Failed to enable streams for %s\n",
			dma->video.entity.name);
		return ret;
	}

	return 0;
}

/**
 * xvip_pipeline_disable_branch - Disable streaming on all subdevs in a pipeline
 *	branch
 * @pipe: The pipeline
 * @dma: The DMA engine at the end of the branch
 *
 * Return: 0 for success, otherwise error code
 */
static int xvip_pipeline_disable_branch(struct xvip_pipeline *pipe,
					struct xvip_dma *dma)
{
	struct v4l2_subdev *sd;
	u32 pad;
	int ret;

	dev_dbg(dma->xdev->dev, "Disabling streams on %s\n",
		dma->video.entity.name);

	sd = xvip_dma_remote_subdev(&dma->pad, &pad);
	if (!sd)
		return -ENXIO;

	ret = v4l2_subdev_disable_streams(sd, pad, BIT(0));
	if (ret) {
		dev_err(dma->xdev->dev, "Failed to disable streams for %s\n",
			dma->video.entity.name);
		return ret;
	}

	return 0;
}

#define xvip_pipeline_for_each_dma(pipe, dma, type)			\
list_for_each_entry(dma, &pipe->dmas, pipe_list)			\
	if (dma->video.vfl_dir == type)

#define xvip_pipeline_for_each_dma_continue_reverse(pipe, dma, type)	\
list_for_each_entry_continue_reverse(dma, &pipe->dmas, pipe_list)	\
	if (dma->video.vfl_dir == type)

/**
 * xvip_pipeline_start - Start the full pipeline
 * @pipe: The pipeline
 *
 * This function is used in synchronous pipeline mode to start the full
 * pipeline when all DMA engines have been started.
 *
 * Return: 0 for success, otherwise error code
 */
static int xvip_pipeline_start(struct xvip_pipeline *pipe)
{
	struct xvip_dma *dma;
	int ret;

	/*
	 * First start all the output DMA engines, before starting the
	 * pipeline. This is required to avoid the slave AXI stream interface
	 * applying back pressure and stopping the pipeline right when it gets
	 * started.
	 */
	xvip_pipeline_for_each_dma(pipe, dma, VFL_DIR_RX) {
		ret = xvip_dma_start(dma);
		if (ret)
			goto err_output;
	}

	/* Start all pipeline branches starting from the output DMA engines. */
	xvip_pipeline_for_each_dma(pipe, dma, VFL_DIR_RX) {
		ret = xvip_pipeline_enable_branch(pipe, dma);
		if (ret)
			goto err_branch;
	}

	/* Finally start all input DMA engines. */
	xvip_pipeline_for_each_dma(pipe, dma, VFL_DIR_TX) {
		ret = xvip_dma_start(dma);
		if (ret)
			goto err_input;
	}

	return 0;

err_input:
	xvip_pipeline_for_each_dma_continue_reverse(pipe, dma, VFL_DIR_TX)
		xvip_dma_stop(dma);

err_branch:
	xvip_pipeline_for_each_dma_continue_reverse(pipe, dma, VFL_DIR_RX)
		xvip_pipeline_disable_branch(pipe, dma);

err_output:
	xvip_pipeline_for_each_dma_continue_reverse(pipe, dma, VFL_DIR_RX)
		xvip_dma_stop(dma);

	return ret;
}

/**
 * xvip_pipeline_stop - Stop the full pipeline
 * @pipe: The pipeline
 *
 * This function is used in synchronous pipeline mode to stop the full
 * pipeline when a DMA engine is stopped.
 */
static void xvip_pipeline_stop(struct xvip_pipeline *pipe)
{
	struct xvip_dma *dma;

	/* There's no meaningful way to handle errors when disabling. */

	xvip_pipeline_for_each_dma(pipe, dma, VFL_DIR_TX)
		xvip_dma_stop(dma);

	xvip_pipeline_for_each_dma(pipe, dma, VFL_DIR_RX)
		xvip_pipeline_disable_branch(pipe, dma);

	xvip_pipeline_for_each_dma(pipe, dma, VFL_DIR_RX)
		xvip_dma_stop(dma);
}

/**
 * xvip_pipeline_start_dma - Start a DMA engine on a pipeline
 * @pipe: The pipeline
 * @dma: The DMA engine being started
 *
 * The pipeline is shared between all DMA engines connect at its input and
 * output. While the stream state of DMA engines can be controlled
 * independently, pipelines have a shared stream state that enable or disable
 * all entities in the pipeline. For this reason the pipeline uses a streaming
 * counter that tracks the number of DMA engines that have requested the stream
 * to be enabled.
 *
 * This function increments the pipeline streaming count corresponding to the
 * @dma direction. When the streaming count reaches the number of DMA engines
 * in the pipeline, it enables all entities that belong to the pipeline.
 *
 * Return: 0 if successful, or the return value of the failed video::s_stream
 * operation otherwise. The pipeline state is not updated when the operation
 * fails.
 */
static int xvip_pipeline_start_dma(struct xvip_pipeline *pipe,
				   struct xvip_dma *dma)
{
	int ret = 0;

	mutex_lock(&pipe->lock);

	switch (xvip_dma_multi_out_mode) {
	case XVIP_DMA_MULTI_OUT_MODE_SYNC:
	default:
		if (pipe->input_stream_count + pipe->output_stream_count ==
		    pipe->num_inputs + pipe->num_outputs - 1) {
			ret = xvip_pipeline_start(pipe);
			if (ret < 0)
				goto done;
		}

		if (dma->video.vfl_dir == VFL_DIR_RX)
			pipe->output_stream_count++;
		else
			pipe->input_stream_count++;

		break;

	case XVIP_DMA_MULTI_OUT_MODE_ASYNC:
		ret = xvip_dma_start(dma);
		if (ret)
			goto done;

		ret = xvip_pipeline_enable_branch(pipe, dma);
		if (ret) {
			xvip_dma_stop(dma);
			goto done;
		}

		break;
	}

done:
	mutex_unlock(&pipe->lock);
	return ret;
}

/**
 * xvip_pipeline_stop_dma - Stop a DMA engine on a pipeline
 * @pipe: The pipeline
 * @dma: The DMA engine being stopped
 *
 * The pipeline is shared between all DMA engines connect at its input and
 * output. While the stream state of DMA engines can be controlled
 * independently, pipelines have a shared stream state that enable or disable
 * all entities in the pipeline. For this reason the pipeline uses a streaming
 * counter that tracks the number of DMA engines that have requested the stream
 * to be enabled.
 *
 * This function decrements the pipeline streaming count corresponding to the
 * @dma direction. As soon as the streaming count goes lower than the number of
 * DMA engines in the pipeline, it disables all entities in the pipeline.
 */
static void xvip_pipeline_stop_dma(struct xvip_pipeline *pipe,
				   struct xvip_dma *dma)
{
	mutex_lock(&pipe->lock);

	switch (xvip_dma_multi_out_mode) {
	case XVIP_DMA_MULTI_OUT_MODE_SYNC:
	default:
		if (dma->video.vfl_dir == VFL_DIR_RX)
			pipe->output_stream_count--;
		else
			pipe->input_stream_count--;

		if (pipe->input_stream_count + pipe->output_stream_count ==
		    pipe->num_inputs + pipe->num_outputs - 1)
			xvip_pipeline_stop(pipe);

		break;

	case XVIP_DMA_MULTI_OUT_MODE_ASYNC:
		xvip_pipeline_disable_branch(pipe, dma);
		xvip_dma_stop(dma);
		break;
	}

	mutex_unlock(&pipe->lock);
}

static int xvip_pipeline_init(struct xvip_pipeline *pipe,
			      struct xvip_dma *start)
{
	struct media_graph graph;
	struct media_entity *entity = &start->video.entity;
	struct media_device *mdev = entity->graph_obj.mdev;
	unsigned int num_inputs = 0;
	unsigned int num_outputs = 0;
	int ret;

	mutex_lock(&mdev->graph_mutex);

	/* Walk the graph to locate the video nodes. */
	ret = media_graph_walk_init(&graph, mdev);
	if (ret) {
		mutex_unlock(&mdev->graph_mutex);
		return ret;
	}

	media_graph_walk_start(&graph, entity);

	while ((entity = media_graph_walk_next(&graph))) {
		struct xvip_dma *dma;

		if (entity->function != MEDIA_ENT_F_IO_V4L)
			continue;

		dma = to_xvip_dma(media_entity_to_video_device(entity));

		if (dma->pad.flags & MEDIA_PAD_FL_SINK)
			num_outputs++;
		else
			num_inputs++;

		list_add_tail(&dma->pipe_list, &pipe->dmas);
	}

	mutex_unlock(&mdev->graph_mutex);

	media_graph_walk_cleanup(&graph);

	/* We need at least one DMA to proceed */
	if (num_outputs == 0 && num_inputs == 0)
		return -EPIPE;

	pipe->num_inputs = num_inputs;
	pipe->num_outputs = num_outputs;
	pipe->xdev = start->xdev;

	return 0;
}

static void __xvip_pipeline_cleanup(struct xvip_pipeline *pipe)
{
	while (!list_empty(&pipe->dmas))
		list_del(pipe->dmas.next);

	pipe->num_inputs = 0;
	pipe->num_outputs = 0;
}

/**
 * xvip_pipeline_cleanup - Cleanup the pipeline after streaming
 * @pipe: the pipeline
 *
 * Decrease the pipeline use count and clean it up if we were the last user.
 */
static void xvip_pipeline_cleanup(struct xvip_pipeline *pipe)
{
	mutex_lock(&pipe->lock);

	/* If we're the last user clean up the pipeline. */
	if (--pipe->use_count == 0)
		__xvip_pipeline_cleanup(pipe);

	mutex_unlock(&pipe->lock);
}

/**
 * xvip_pipeline_prepare - Prepare the pipeline for streaming
 * @pipe: the pipeline
 * @dma: DMA engine at one end of the pipeline
 *
 * Validate the pipeline if no user exists yet, otherwise just increase the use
 * count.
 *
 * Return: 0 if successful or -EPIPE if the pipeline is not valid.
 */
static int xvip_pipeline_prepare(struct xvip_pipeline *pipe,
				 struct xvip_dma *dma)
{
	int ret;

	mutex_lock(&pipe->lock);

	/* If we're the first user validate and initialize the pipeline. */
	if (pipe->use_count == 0) {
		ret = xvip_pipeline_init(pipe, dma);
		if (ret < 0) {
			__xvip_pipeline_cleanup(pipe);
			goto done;
		}
	}

	pipe->use_count++;
	ret = 0;

done:
	mutex_unlock(&pipe->lock);
	return ret;
}

/* -----------------------------------------------------------------------------
 * videobuf2 queue operations
 */

static void xvip_dma_return_buffers(struct xvip_dma *dma,
				    enum vb2_buffer_state state)
{
	struct xvip_dma_buffer *buf, *nbuf;

	spin_lock_irq(&dma->queued_lock);
	list_for_each_entry_safe(buf, nbuf, &dma->queued_bufs, queue) {
		vb2_buffer_done(&buf->buf.vb2_buf, state);
		list_del(&buf->queue);
	}
	spin_unlock_irq(&dma->queued_lock);
}

static int
xvip_dma_queue_setup(struct vb2_queue *vq,
		     unsigned int *nbuffers, unsigned int *nplanes,
		     unsigned int sizes[], struct device *alloc_devs[])
{
	struct xvip_dma *dma = vb2_get_drv_priv(vq);
	unsigned int i;
	int sizeimage;

	/* Make sure the image size is large enough. */
	if (*nplanes) {
		if (*nplanes != dma->format.num_planes)
			return -EINVAL;

		for (i = 0; i < *nplanes; i++) {
			sizeimage = dma->format.plane_fmt[i].sizeimage;
			if (sizes[i] < sizeimage)
				return -EINVAL;
		}
	} else {
		*nplanes = dma->fmtinfo->num_buffers;
		for (i = 0; i < dma->fmtinfo->num_buffers; i++) {
			sizeimage = dma->format.plane_fmt[i].sizeimage;
			sizes[i] = sizeimage;
		}
	}

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
	struct xvip_dma_buffer *buf = to_xvip_dma_buffer(vbuf);
	struct xvip_dma *dma = buf->dma;

	xvip_dma_submit_vb2_buffer(dma, buf);

	if (vb2_is_streaming(&dma->queue))
		dma_async_issue_pending(dma->dma);
}

static int xvip_dma_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct xvip_dma *dma = vb2_get_drv_priv(vq);
	struct xvip_pipeline *pipe;
	int ret;

	dma->sequence = 0;
	dma->prev_fid = ~0;

	/*
	 * Start streaming on the pipeline. No link touching an entity in the
	 * pipeline can be activated or deactivated once streaming is started.
	 *
	 * Use the pipeline object embedded in the first DMA object that starts
	 * streaming.
	 */
	mutex_lock(&dma->xdev->lock);
	pipe = to_xvip_pipeline(&dma->video) ? : &dma->pipe;

	ret = video_device_pipeline_start(&dma->video, &pipe->pipe);
	mutex_unlock(&dma->xdev->lock);
	if (ret < 0)
		goto err_return_buffers;

	/* Verify that the configured format matches the output of the
	 * connected subdev.
	 */
	ret = xvip_dma_verify_format(dma);
	if (ret < 0)
		goto err_pipe_stop;

	ret = xvip_pipeline_prepare(pipe, dma);
	if (ret < 0)
		goto err_pipe_stop;

	/* Start the DMA engine on the pipeline. */
	ret = xvip_pipeline_start_dma(pipe, dma);
	if (ret < 0)
		goto err_pipe_cleanup;

	return 0;

err_pipe_cleanup:
	xvip_pipeline_cleanup(pipe);
err_pipe_stop:
	video_device_pipeline_stop(&dma->video);
err_return_buffers:
	/* Give back all queued buffers to videobuf2. */
	xvip_dma_return_buffers(dma, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void xvip_dma_stop_streaming(struct vb2_queue *vq)
{
	struct xvip_dma *dma = vb2_get_drv_priv(vq);
	struct xvip_pipeline *pipe = to_xvip_pipeline(&dma->video);

	/* Stop the DMA engine on the pipeline. */
	xvip_pipeline_stop_dma(pipe, dma);

	/* Cleanup the pipeline and mark it as being stopped. */
	xvip_pipeline_cleanup(pipe);
	video_device_pipeline_stop(&dma->video);

	/* Give back all queued buffers to videobuf2. */
	xvip_dma_return_buffers(dma, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops xvip_dma_queue_qops = {
	.queue_setup = xvip_dma_queue_setup,
	.buf_prepare = xvip_dma_buffer_prepare,
	.buf_queue = xvip_dma_buffer_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.start_streaming = xvip_dma_start_streaming,
	.stop_streaming = xvip_dma_stop_streaming,
};

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int
xvip_dma_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct v4l2_fh *vfh = file->private_data;
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);

	cap->capabilities = dma->xdev->v4l2_caps | V4L2_CAP_STREAMING |
			    V4L2_CAP_DEVICE_CAPS;

	strscpy((char *)cap->driver, "xilinx-vipp", sizeof(cap->driver));
	strscpy((char *)cap->card, (char *)dma->video.name, sizeof(cap->card));
	snprintf((char *)cap->bus_info, sizeof(cap->bus_info),
		 "platform:%pOFn:%u", dma->xdev->dev->of_node, dma->port);

	return 0;
}

static int
xvip_dma_enum_input(struct file *file, void *priv, struct v4l2_input *i)
{
	struct v4l2_fh *vfh = file->private_data;
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);
	struct v4l2_subdev *subdev;

	if (i->index > 0)
		return -EINVAL;

	subdev = xvip_dma_remote_subdev(&dma->pad, NULL);
	if (!subdev)
		return -EPIPE;

	/*
	 * FIXME: right now only camera input type is handled.
	 * There should be mechanism to distinguish other types of
	 * input like V4L2_INPUT_TYPE_TUNER and V4L2_INPUT_TYPE_TOUCH.
	 */
	i->type = V4L2_INPUT_TYPE_CAMERA;
	strlcpy((char *)i->name, (char *)subdev->name, sizeof(i->name));

	return 0;
}

static int
xvip_dma_get_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int
xvip_dma_set_input(struct file *file, void *fh, unsigned int i)
{
	if (i > 0)
		return -EINVAL;

	return 0;
}

/* FIXME: without this callback function, some applications are not configured
 * with correct formats, and it results in frames in wrong format. Whether this
 * callback needs to be required is not clearly defined, so it should be
 * clarified through the mailing list.
 */
static int
xvip_dma_enum_format(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct v4l2_fh *vfh = file->private_data;
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);
	const struct xvip_video_format *fmt;
	unsigned int i;
	u32 fmt_cnt = 0;
	u32 *fmts;

	xilinx_xdma_get_v4l2_vid_fmts(dma->dma, &fmt_cnt, &fmts);

	if (f->mbus_code) {
		/* A single 4CC is supported per media bus code. */
		if (f->index > 0)
			return -EINVAL;

		/*
		 * If the DMA engine returned a list of formats, find the one
		 * that matches the media bus code. Otherwise, search all the
		 * formats supported by this driver.
		 */
		if (fmt_cnt) {
			for (i = 0; i < fmt_cnt; ++i) {
				fmt = xvip_get_format_by_fourcc(fmts[i]);
				if (!IS_ERR(fmt) && fmt->code == f->mbus_code)
					break;
			}

			if (i == fmt_cnt)
				return -EINVAL;
		} else {
			fmt = xvip_get_format_by_code(f->mbus_code);
		}
	} else {
		/*
		 * If the DMA engine returned a list of formats, enumerate them,
		 * otherwise enumerate all the formats supported by this driver.
		 */
		if (fmt_cnt) {
			if (f->index >= fmt_cnt)
				return -EINVAL;

			fmt = xvip_get_format_by_fourcc(fmts[f->index]);
		} else {
			fmt = xvip_get_format_by_index(f->index);
		}
	}

	if (IS_ERR(fmt))
		return -EINVAL;

	f->pixelformat = fmt->fourcc;

	return 0;
}

static int
xvip_dma_get_format_mplane(struct file *file, void *fh,
			   struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);

	format->fmt.pix_mp = dma->format;

	return 0;
}

static void
__xvip_dma_try_format(const struct xvip_dma *dma,
		      struct v4l2_pix_format_mplane *pix_mp,
		      const struct xvip_video_format **fmtinfo)
{
	const struct xvip_video_format *info;
	unsigned int min_width, max_width;
	unsigned int min_bpl, max_bpl;
	unsigned int width;
	unsigned int i;


	if (pix_mp->field != V4L2_FIELD_ALTERNATE)
		pix_mp->field = V4L2_FIELD_NONE;

	/*
	 * Retrieve format information and select the default format if the
	 * requested format isn't supported.
	 */
	info = xvip_get_format_by_fourcc(pix_mp->pixelformat);

	if (IS_ERR(info))
		info = xvip_get_format_by_fourcc(XVIP_DMA_DEF_FORMAT);

	/*
	 * The width alignment requirements (width_align) are expressed in
	 * pixels, while the stride alignment (align) requirements are
	 * expressed in bytes.
	 */
	min_width = roundup(XVIP_DMA_MIN_WIDTH, dma->width_align);
	max_width = rounddown(XVIP_DMA_MAX_WIDTH, dma->width_align);

	width = rounddown(pix_mp->width, dma->width_align);
	pix_mp->width = clamp(width, min_width, max_width);
	pix_mp->height = clamp(pix_mp->height, XVIP_DMA_MIN_HEIGHT,
			       XVIP_DMA_MAX_HEIGHT);

	/*
	 * Clamp the requested bytes per line value. If the maximum
	 * bytes per line value is zero, the module doesn't support
	 * user configurable line sizes. Override the requested value
	 * with the minimum in that case.
	 */
	max_bpl = rounddown(XVIP_DMA_MAX_WIDTH, dma->align);

	/* Calculate the bytesperline and sizeimage values for each plane. */
	for (i = 0; i < info->num_planes; i++) {
		struct v4l2_plane_pix_format *plane = &pix_mp->plane_fmt[i];
		unsigned int bpl;

		min_bpl = pix_mp->width * info->bytes_per_pixel[i].numerator
			/ info->bytes_per_pixel[i].denominator;
		min_bpl = roundup(min_bpl, dma->align);

		bpl = rounddown(plane->bytesperline, dma->align);
		plane->bytesperline = clamp(bpl, min_bpl, max_bpl);

		plane->sizeimage = plane->bytesperline * pix_mp->height
				 / (i ? info->vsub : 1);
	}

	/*
	 * When using single-planar formats with multiple planes, add up all
	 * sizeimage values in the first plane.
	 */
	if (info->num_buffers == 1) {
		for (i = 1; i < info->num_planes; ++i) {
			struct v4l2_plane_pix_format *plane =
				&pix_mp->plane_fmt[i];

			pix_mp->plane_fmt[0].sizeimage += plane->sizeimage;
		}
	}

	if (fmtinfo)
		*fmtinfo = info;
}

static int
xvip_dma_try_format_mplane(struct file *file, void *fh,
			   struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);

	__xvip_dma_try_format(dma, &format->fmt.pix_mp, NULL);
	return 0;
}

static int
xvip_dma_set_format_mplane(struct file *file, void *fh,
			   struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);
	const struct xvip_video_format *info = NULL;

	__xvip_dma_try_format(dma, &format->fmt.pix_mp, &info);

	if (vb2_is_busy(&dma->queue))
		return -EBUSY;

	dma->format = format->fmt.pix_mp;

	/*
	 * Save format resolution in crop rectangle. This will be
	 * updated when s_slection is called.
	 */
	dma->r.width = format->fmt.pix_mp.width;
	dma->r.height = format->fmt.pix_mp.height;

	dma->fmtinfo = info;

	return 0;
}

/* Emulate the legacy single-planar API using the multi-planar operations. */
static void
xvip_dma_single_to_multi_planar(const struct v4l2_format *fmt,
				struct v4l2_format *fmt_mp)
{
	const struct v4l2_pix_format *pix = &fmt->fmt.pix;
	struct v4l2_pix_format_mplane *pix_mp = &fmt_mp->fmt.pix_mp;

	memset(fmt_mp, 0, sizeof(*fmt_mp));

	switch (fmt->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		fmt_mp->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		fmt_mp->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		break;
	}

	pix_mp->width = pix->width;
	pix_mp->height = pix->height;
	pix_mp->pixelformat = pix->pixelformat;
	pix_mp->field = pix->field;
	pix_mp->colorspace = pix->colorspace;
	pix_mp->plane_fmt[0].sizeimage = pix->sizeimage;
	pix_mp->plane_fmt[0].bytesperline = pix->bytesperline;
	pix_mp->num_planes = 1;
	pix_mp->flags = pix->flags;
	pix_mp->ycbcr_enc = pix->ycbcr_enc;
	pix_mp->quantization = pix->quantization;
	pix_mp->xfer_func = pix->xfer_func;
}

static void
xvip_dma_multi_to_single_planar(const struct v4l2_format *fmt_mp,
				struct v4l2_format *fmt)
{
	const struct v4l2_pix_format_mplane *pix_mp = &fmt_mp->fmt.pix_mp;
	struct v4l2_pix_format *pix = &fmt->fmt.pix;

	memset(fmt, 0, sizeof(*fmt));

	switch (fmt->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		break;
	}

	pix->width = pix_mp->width;
	pix->height = pix_mp->height;
	pix->pixelformat = pix_mp->pixelformat;
	pix->field = pix_mp->field;
	pix->colorspace = pix_mp->colorspace;
	pix->sizeimage = pix_mp->plane_fmt[0].sizeimage;
	pix->bytesperline = pix_mp->plane_fmt[0].bytesperline;
	pix->flags = pix_mp->flags;
	pix->ycbcr_enc = pix_mp->ycbcr_enc;
	pix->quantization = pix_mp->quantization;
	pix->xfer_func = pix_mp->xfer_func;
}

static int
xvip_dma_get_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_format fmt_mp;
	int ret;

	xvip_dma_single_to_multi_planar(format, &fmt_mp);

	ret = xvip_dma_get_format_mplane(file, fh, &fmt_mp);
	if (ret)
		return ret;

	xvip_dma_multi_to_single_planar(&fmt_mp, format);

	return 0;
}

static int
xvip_dma_try_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_format fmt_mp;
	int ret;

	xvip_dma_single_to_multi_planar(format, &fmt_mp);

	ret = xvip_dma_try_format_mplane(file, fh, &fmt_mp);
	if (ret)
		return ret;

	xvip_dma_multi_to_single_planar(&fmt_mp, format);

	return 0;
}

static int
xvip_dma_set_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_format fmt_mp;
	int ret;

	xvip_dma_single_to_multi_planar(format, &fmt_mp);

	ret = xvip_dma_set_format_mplane(file, fh, &fmt_mp);
	if (ret)
		return ret;

	xvip_dma_multi_to_single_planar(&fmt_mp, format);

	return 0;
}

static int
xvip_dma_g_selection(struct file *file, void *fh, struct v4l2_selection *sel)
{
	struct v4l2_fh *vfh = file->private_data;
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);
	bool crop_frame = false;

	switch (sel->target) {
	case V4L2_SEL_TGT_COMPOSE:
		if (sel->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;

		crop_frame = true;
		break;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		if (sel->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_CROP:
		if (sel->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;

		crop_frame = true;
		break;
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		if (sel->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	sel->r.left = 0;
	sel->r.top = 0;

	if (crop_frame) {
		sel->r.width = dma->r.width;
		sel->r.height = dma->r.height;
	} else {
		sel->r.width = dma->format.width;
		sel->r.height = dma->format.height;
	}

	return 0;
}

static int
xvip_dma_s_selection(struct file *file, void *fh, struct v4l2_selection *sel)
{
	struct v4l2_fh *vfh = file->private_data;
	struct xvip_dma *dma = to_xvip_dma(vfh->vdev);
	u32 width, height;

	switch (sel->target) {
	case V4L2_SEL_TGT_COMPOSE:
		/* COMPOSE target is only valid for capture buftype */
		if (sel->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_CROP:
		/* CROP target is only valid for output buftype */
		if (sel->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	width = dma->format.width;
	height = dma->format.height;

	if (sel->r.width > width || sel->r.height > height ||
	    sel->r.top != 0 || sel->r.left != 0)
		return -EINVAL;

	sel->r.width = rounddown(max(XVIP_DMA_MIN_WIDTH, sel->r.width),
				 dma->width_align);
	sel->r.height = max(XVIP_DMA_MIN_HEIGHT, sel->r.height);
	dma->r.width = sel->r.width;
	dma->r.height = sel->r.height;

	return 0;
}

static const struct v4l2_ioctl_ops xvip_dma_ioctl_ops = {
	.vidioc_querycap		= xvip_dma_querycap,
	.vidioc_enum_fmt_vid_cap	= xvip_dma_enum_format,
	.vidioc_enum_fmt_vid_out	= xvip_dma_enum_format,
	.vidioc_g_fmt_vid_cap		= xvip_dma_get_format,
	.vidioc_g_fmt_vid_cap_mplane	= xvip_dma_get_format_mplane,
	.vidioc_g_fmt_vid_out		= xvip_dma_get_format,
	.vidioc_g_fmt_vid_out_mplane	= xvip_dma_get_format_mplane,
	.vidioc_s_fmt_vid_cap		= xvip_dma_set_format,
	.vidioc_s_fmt_vid_cap_mplane	= xvip_dma_set_format_mplane,
	.vidioc_s_fmt_vid_out		= xvip_dma_set_format,
	.vidioc_s_fmt_vid_out_mplane	= xvip_dma_set_format_mplane,
	.vidioc_try_fmt_vid_cap		= xvip_dma_try_format,
	.vidioc_try_fmt_vid_cap_mplane	= xvip_dma_try_format_mplane,
	.vidioc_try_fmt_vid_out		= xvip_dma_try_format,
	.vidioc_try_fmt_vid_out_mplane	= xvip_dma_try_format_mplane,
	.vidioc_s_selection		= xvip_dma_s_selection,
	.vidioc_g_selection		= xvip_dma_g_selection,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
	.vidioc_enum_input	= &xvip_dma_enum_input,
	.vidioc_g_input		= &xvip_dma_get_input,
	.vidioc_s_input		= &xvip_dma_set_input,
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
	struct v4l2_pix_format_mplane *pix_mp;
	char name[16];
	int ret;

	dma->xdev = xdev;
	dma->port = port;
	mutex_init(&dma->lock);
	mutex_init(&dma->pipe.lock);
	INIT_LIST_HEAD(&dma->queued_bufs);
	INIT_LIST_HEAD(&dma->pipe.dmas);
	spin_lock_init(&dma->queued_lock);

	/* Request the DMA channel. */
	snprintf(name, sizeof(name), "port%u", port);
	dma->dma = dma_request_chan(dma->xdev->dev, name);
	if (IS_ERR(dma->dma)) {
		ret = PTR_ERR(dma->dma);
		if (ret != -EPROBE_DEFER)
			dev_err(dma->xdev->dev, "no VDMA channel found\n");
		goto error;
	}

	xilinx_xdma_get_width_align(dma->dma, &dma->width_align);
	if (!dma->width_align) {
		dev_dbg(dma->xdev->dev,
			"Using width align %d\n", XVIP_DMA_DEF_WIDTH_ALIGN);
		dma->width_align = XVIP_DMA_DEF_WIDTH_ALIGN;
	}

	dma->align = 1 << dma->dma->device->copy_align;

	/* Initialize the default format. */
	dma->fmtinfo = xvip_get_format_by_fourcc(XVIP_DMA_DEF_FORMAT);

	pix_mp = &dma->format;
	pix_mp->pixelformat = dma->fmtinfo->fourcc;
	pix_mp->colorspace = V4L2_COLORSPACE_SRGB;
	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->width = XVIP_DMA_DEF_WIDTH;
	pix_mp->height = XVIP_DMA_DEF_HEIGHT;

	__xvip_dma_try_format(dma, &dma->format, NULL);

	/* Initialize the media entity... */
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		dma->pad.flags = MEDIA_PAD_FL_SINK;
	else
		dma->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&dma->video.entity, 1, &dma->pad);
	if (ret < 0)
		goto error;

	/* ... and the video node... */
	dma->video.fops = &xvip_dma_fops;
	dma->video.v4l2_dev = &xdev->v4l2_dev;
	dma->video.queue = &dma->queue;
	snprintf(dma->video.name, sizeof(dma->video.name), "%pOFn %s %u",
		 xdev->dev->of_node,
		 (type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
		  type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
					? "output" : "input",
		 port);

	dma->video.vfl_type = VFL_TYPE_VIDEO;
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		dma->video.vfl_dir = VFL_DIR_RX;
	else
		dma->video.vfl_dir = VFL_DIR_TX;

	dma->video.release = video_device_release_empty;
	dma->video.ioctl_ops = &xvip_dma_ioctl_ops;
	dma->video.lock = &dma->lock;
	dma->video.device_caps = V4L2_CAP_STREAMING;
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		dma->video.device_caps |= V4L2_CAP_VIDEO_CAPTURE_MPLANE;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		dma->video.device_caps |= V4L2_CAP_VIDEO_CAPTURE;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		dma->video.device_caps |= V4L2_CAP_VIDEO_OUTPUT_MPLANE;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		dma->video.device_caps |= V4L2_CAP_VIDEO_OUTPUT;
		break;
	default:
		ret = -EINVAL;
		goto error;
	}

	video_set_drvdata(&dma->video, dma);

	/* ... and the buffers queue. */

	/*
	 * Don't enable VB2_READ and VB2_WRITE, as using the read() and write()
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
	mutex_destroy(&dma->pipe.lock);
}
