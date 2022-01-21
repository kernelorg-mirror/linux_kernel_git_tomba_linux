// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Video IP Core
 *
 * Copyright (C) 2013-2015 Ideas on Board
 * Copyright (C) 2013-2015 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <linux/clk.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <dt-bindings/media/xilinx-vip.h>

#include "xilinx-vip.h"

/* -----------------------------------------------------------------------------
 * Helper functions
 */

static const struct xvip_video_format xvip_video_formats[] = {
	{ XVIP_VF_YUV_420, 8, NULL, MEDIA_BUS_FMT_VYYUYY8_1X24, 0,
	  1, 12, V4L2_PIX_FMT_NV12, 2, 1, 1, 2 },
	{ XVIP_VF_YUV_420, 8, NULL, MEDIA_BUS_FMT_VYYUYY8_1X24, 0,
	  1, 12, V4L2_PIX_FMT_NV12M, 2, 2, 1, 2 },
	{ XVIP_VF_YUV_420, 10, NULL, MEDIA_BUS_FMT_VYYUYY10_4X20, 0,
	  1, 12, V4L2_PIX_FMT_XV15, 2, 1, 2, 2 },
	{ XVIP_VF_YUV_420, 10, NULL, MEDIA_BUS_FMT_VYYUYY10_4X20, 0,
	  1, 12, V4L2_PIX_FMT_XV15M, 2, 2, 1, 2 },
	{ XVIP_VF_YUV_420, 12, NULL, MEDIA_BUS_FMT_UYYVYY12_4X24, 0,
	  1, 12, V4L2_PIX_FMT_X012, 2, 1, 2, 2 },
	{ XVIP_VF_YUV_420, 12, NULL, MEDIA_BUS_FMT_UYYVYY12_4X24, 0,
	  1, 12, V4L2_PIX_FMT_X012M, 2, 2, 1, 2 },
	{ XVIP_VF_YUV_420, 16, NULL, MEDIA_BUS_FMT_UYYVYY16_4X32, 0,
	  2, 12, V4L2_PIX_FMT_X016, 2, 1, 2, 2 },
	{ XVIP_VF_YUV_420, 16, NULL, MEDIA_BUS_FMT_UYYVYY16_4X32, 0,
	  2, 12, V4L2_PIX_FMT_X016M, 2, 2, 1, 2 },
	{ XVIP_VF_YUV_422, 8, NULL, MEDIA_BUS_FMT_UYVY8_1X16, 0,
	  1, 16, V4L2_PIX_FMT_NV16, 2, 1, 1, 1 },
	{ XVIP_VF_YUV_422, 8, NULL, MEDIA_BUS_FMT_UYVY8_1X16, 0,
	  1, 16, V4L2_PIX_FMT_NV16M, 2, 2, 1, 1 },
	{ XVIP_VF_YUV_422, 8, NULL, MEDIA_BUS_FMT_UYVY8_1X16, 0,
	  2, 16, V4L2_PIX_FMT_YUYV, 1, 1, 2, 1 },
	{ XVIP_VF_VUY_422, 8, NULL, MEDIA_BUS_FMT_UYVY8_1X16, 0,
	  2, 16, V4L2_PIX_FMT_UYVY, 1, 1, 2, 1 },
	{ XVIP_VF_YUV_422, 10, NULL, MEDIA_BUS_FMT_UYVY10_1X20, 0,
	  1, 16, V4L2_PIX_FMT_XV20, 2, 1, 2, 1 },
	{ XVIP_VF_YUV_422, 10, NULL, MEDIA_BUS_FMT_UYVY10_1X20, 0,
	  1, 16, V4L2_PIX_FMT_XV20M, 2, 2, 1, 1 },
	{ XVIP_VF_YUV_422, 12, NULL, MEDIA_BUS_FMT_UYVY12_1X24, 0,
	  1, 16, V4L2_PIX_FMT_X212, 2, 1, 2, 1 },
	{ XVIP_VF_YUV_422, 12, NULL, MEDIA_BUS_FMT_UYVY12_1X24, 0,
	  1, 16, V4L2_PIX_FMT_X212M, 2, 2, 1, 1 },
	{ XVIP_VF_YUV_422, 16, NULL, MEDIA_BUS_FMT_UYVY16_2X32, 0,
	  2, 16, V4L2_PIX_FMT_X216, 2, 1, 2, 1 },
	{ XVIP_VF_YUV_422, 16, NULL, MEDIA_BUS_FMT_UYVY16_2X32, 0,
	  2, 16, V4L2_PIX_FMT_X216M, 2, 2, 1, 1 },
	{ XVIP_VF_YUV_444, 8, NULL, MEDIA_BUS_FMT_VUY8_1X24, 0,
	  3, 24, V4L2_PIX_FMT_VUY24, 1, 1, 1, 1 },
	{ XVIP_VF_YUV_444, 10, NULL, MEDIA_BUS_FMT_VUY10_1X30, 0,
	  1, 24, V4L2_PIX_FMT_X403, 3, 1, 1, 1 },
	{ XVIP_VF_YUV_444, 8, NULL, MEDIA_BUS_FMT_VUY8_1X24, 0,
	  1, 24, V4L2_PIX_FMT_YUV444P, 3, 1, 1, 1 },
	{ XVIP_VF_YUV_444, 8, NULL, MEDIA_BUS_FMT_VUY8_1X24, 0,
	  1, 8, V4L2_PIX_FMT_YUV444M, 3, 3, 1, 1 },
	{ XVIP_VF_YUVX, 8, NULL, MEDIA_BUS_FMT_VUY8_1X24, 0,
	  4, 32, V4L2_PIX_FMT_XVUY32, 1, 1, 1, 1 },
	{ XVIP_VF_YUVX, 10, NULL, MEDIA_BUS_FMT_VUY10_1X30, 0,
	  3, 32, V4L2_PIX_FMT_XVUY10, 1, 1, 1, 1 },
	{ XVIP_VF_YUV_444, 12, NULL, MEDIA_BUS_FMT_VUY12_1X36, 0,
	  1, 24, V4L2_PIX_FMT_X412, 1, 1, 1, 1 },
	{ XVIP_VF_YUV_444, 12, NULL, MEDIA_BUS_FMT_VUY12_1X36, 0,
	  1, 24, V4L2_PIX_FMT_X412M, 1, 1, 1, 1 },
	{ XVIP_VF_YUV_444, 16, NULL, MEDIA_BUS_FMT_VUY16_1X48, 0,
	  2, 24, V4L2_PIX_FMT_X416, 1, 1, 1, 1 },
	{ XVIP_VF_YUV_444, 16, NULL, MEDIA_BUS_FMT_VUY16_1X48, 0,
	  2, 24, V4L2_PIX_FMT_X416M, 1, 1, 1, 1 },
	{ XVIP_VF_RBG, 8, NULL, MEDIA_BUS_FMT_RBG888_1X24, 0,
	  3, 24, V4L2_PIX_FMT_BGR24, 1, 1, 1, 1 },
	{ XVIP_VF_RBG, 8, NULL, MEDIA_BUS_FMT_RBG888_1X24, 0,
	  3, 24, V4L2_PIX_FMT_RGB24, 1, 1, 1, 1 },
	{ XVIP_VF_BGRX, 8, NULL, MEDIA_BUS_FMT_RBG888_1X24, 0,
	  4, 32, V4L2_PIX_FMT_BGRX32, 1, 1, 1, 1 },
	{ XVIP_VF_XRGB, 8, NULL, MEDIA_BUS_FMT_RBG888_1X24, 0,
	  4, 32, V4L2_PIX_FMT_XBGR32, 1, 1, 1, 1 },
	{ XVIP_VF_XBGR, 10, NULL, MEDIA_BUS_FMT_RBG101010_1X30, 0,
	  3, 32, V4L2_PIX_FMT_XBGR30, 1, 1, 1, 1 },
	{ XVIP_VF_XBGR, 12, NULL, MEDIA_BUS_FMT_RBG121212_1X36, 0,
	  3, 40, V4L2_PIX_FMT_XBGR40, 1, 1, 1, 1 },
	{ XVIP_VF_RBG, 16, NULL, MEDIA_BUS_FMT_RBG161616_1X48, 0,
	  6, 48, V4L2_PIX_FMT_BGR48, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 8, "mono", MEDIA_BUS_FMT_Y8_1X8,
	  MEDIA_BUS_FMT_Y8_1X8,
	  1, 8, V4L2_PIX_FMT_GREY, 1, 1, 1, 1 },
	{ XVIP_VF_Y_GREY, 10, NULL, MEDIA_BUS_FMT_Y10_1X10,
	  MEDIA_BUS_FMT_Y8_1X8,
	  1, 32, V4L2_PIX_FMT_XY10, 1, 1, 1, 1 },
	{ XVIP_VF_Y_GREY, 12, NULL, MEDIA_BUS_FMT_Y12_1X12,
	  MEDIA_BUS_FMT_Y8_1X8,
	  1, 12, V4L2_PIX_FMT_XY12, 1, 1, 1, 1 },
	{ XVIP_VF_Y_GREY, 16, NULL, MEDIA_BUS_FMT_Y16_1X16,
	  MEDIA_BUS_FMT_Y8_1X8,
	  2, 16, V4L2_PIX_FMT_Y16, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 8, "rggb", MEDIA_BUS_FMT_SRGGB8_1X8,
	  MEDIA_BUS_FMT_SRGGB8_1X8,
	  1, 8, V4L2_PIX_FMT_SGRBG8, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 8, "grbg", MEDIA_BUS_FMT_SGRBG8_1X8,
	  MEDIA_BUS_FMT_SGRBG8_1X8,
	  1, 8, V4L2_PIX_FMT_SGRBG8, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 8, "gbrg", MEDIA_BUS_FMT_SGBRG8_1X8,
	  MEDIA_BUS_FMT_SGBRG8_1X8,
	  1, 8, V4L2_PIX_FMT_SGBRG8, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 8, "bggr", MEDIA_BUS_FMT_SBGGR8_1X8,
	  MEDIA_BUS_FMT_SBGGR8_1X8,
	  1, 8, V4L2_PIX_FMT_SBGGR8, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 12, "mono", MEDIA_BUS_FMT_Y12_1X12,
	  2, 12, V4L2_PIX_FMT_Y12, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 10, "rggb", MEDIA_BUS_FMT_SRGGB10_1X10,
	  MEDIA_BUS_FMT_SRGGB8_1X8,
	  2, 10, V4L2_PIX_FMT_SRGGB10, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 10, "grbg", MEDIA_BUS_FMT_SGRBG10_1X10,
	  MEDIA_BUS_FMT_SGRBG8_1X8,
	  2, 10, V4L2_PIX_FMT_SGRBG10, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 10, "gbrg", MEDIA_BUS_FMT_SGBRG10_1X10,
	  MEDIA_BUS_FMT_SGBRG8_1X8,
	  2, 10, V4L2_PIX_FMT_SGBRG10, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 10, "bggr", MEDIA_BUS_FMT_SBGGR10_1X10,
	  MEDIA_BUS_FMT_SBGGR8_1X8,
	  2, 10, V4L2_PIX_FMT_SBGGR10, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 12, "rggb", MEDIA_BUS_FMT_SRGGB12_1X12,
	  MEDIA_BUS_FMT_SRGGB8_1X8,
	  2, 12, V4L2_PIX_FMT_SRGGB12, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 12, "grbg", MEDIA_BUS_FMT_SGRBG12_1X12,
	  MEDIA_BUS_FMT_SGRBG8_1X8,
	  2, 12, V4L2_PIX_FMT_SGRBG12, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 12, "gbrg", MEDIA_BUS_FMT_SGBRG12_1X12,
	  MEDIA_BUS_FMT_SGBRG8_1X8,
	  2, 12, V4L2_PIX_FMT_SGBRG12, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 12, "bggr", MEDIA_BUS_FMT_SBGGR12_1X12,
	  MEDIA_BUS_FMT_SBGGR8_1X8,
	  2, 12, V4L2_PIX_FMT_SBGGR12, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 16, "rggb", MEDIA_BUS_FMT_SRGGB16_1X16,
	  MEDIA_BUS_FMT_SRGGB8_1X8,
	  2, 16, V4L2_PIX_FMT_SRGGB16, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 16, "grbg", MEDIA_BUS_FMT_SGRBG16_1X16,
	  MEDIA_BUS_FMT_SGRBG8_1X8,
	  2, 16, V4L2_PIX_FMT_SGRBG16, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 16, "gbrg", MEDIA_BUS_FMT_SGBRG16_1X16,
	  MEDIA_BUS_FMT_SGBRG8_1X8,
	  2, 16, V4L2_PIX_FMT_SGBRG16, 1, 1, 1, 1 },
	{ XVIP_VF_MONO_SENSOR, 16, "bggr", MEDIA_BUS_FMT_SBGGR16_1X16,
	  MEDIA_BUS_FMT_SBGGR8_1X8,
	  2, 16, V4L2_PIX_FMT_SBGGR16, 1, 1, 1, 1 },
};

/**
 * xvip_get_format_by_code - Retrieve format information for a media bus code
 * @code: the format media bus code
 *
 * Return: a pointer to the format information structure corresponding to the
 * given V4L2 media bus format @code, or ERR_PTR if no corresponding format can
 * be found.
 */
const struct xvip_video_format *xvip_get_format_by_code(unsigned int code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xvip_video_formats); ++i) {
		const struct xvip_video_format *format = &xvip_video_formats[i];

		if (format->code == code)
			return format;
	}

	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(xvip_get_format_by_code);

/**
 * xvip_get_format_by_fourcc - Retrieve format information for a 4CC
 * @fourcc: the format 4CC
 *
 * Return: a pointer to the format information structure corresponding to the
 * given V4L2 format @fourcc. If not found, return a pointer to the first
 * available format (V4L2_PIX_FMT_YUYV).
 */
const struct xvip_video_format *xvip_get_format_by_fourcc(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xvip_video_formats); ++i) {
		const struct xvip_video_format *format = &xvip_video_formats[i];

		if (format->fourcc == fourcc)
			return format;
	}

	return &xvip_video_formats[0];
}
EXPORT_SYMBOL_GPL(xvip_get_format_by_fourcc);

/**
 * xvip_bpl_scaling_factor - Retrieve bpl scaling factor for a 4CC
 * @fourcc: the format 4CC
 * @numerator: returning numerator of scaling factor
 * @denominator: returning denominator of scaling factor
 *
 * Return: Return numerator and denominator values by address
 */
void xvip_bpl_scaling_factor(u32 fourcc, u32 *numerator, u32 *denominator)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_XY10:
	case V4L2_PIX_FMT_XV15:
	case V4L2_PIX_FMT_XV20:
	case V4L2_PIX_FMT_X403:
	case V4L2_PIX_FMT_XV15M:
	case V4L2_PIX_FMT_XV20M:
	case V4L2_PIX_FMT_XBGR30:
	case V4L2_PIX_FMT_XVUY10:
		*numerator = 10;
		*denominator = 8;
		break;
	case V4L2_PIX_FMT_XBGR40:
	case V4L2_PIX_FMT_XY12:
	case V4L2_PIX_FMT_X012:
	case V4L2_PIX_FMT_X012M:
	case V4L2_PIX_FMT_X212:
	case V4L2_PIX_FMT_X212M:
	case V4L2_PIX_FMT_X412:
	case V4L2_PIX_FMT_X412M:
		*numerator = 12;
		*denominator = 8;
		break;
	default:
		*numerator = 1;
		*denominator = 1;
		break;
	}
}
EXPORT_SYMBOL_GPL(xvip_bpl_scaling_factor);

/**
 * xvip_width_padding_factor - Retrieve width's padding factor for a 4CC
 * @fourcc: the format 4CC
 * @numerator: returning numerator of padding factor
 * @denominator: returning denominator of padding factor
 *
 * Return: Return numerator and denominator values by address
 */
void xvip_width_padding_factor(u32 fourcc, u32 *numerator, u32 *denominator)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_XY10:
	case V4L2_PIX_FMT_XV15:
	case V4L2_PIX_FMT_XV20:
	case V4L2_PIX_FMT_X403:
	case V4L2_PIX_FMT_XV15M:
	case V4L2_PIX_FMT_XV20M:
	case V4L2_PIX_FMT_XBGR30:
	case V4L2_PIX_FMT_XVUY10:
		/* 32 bits are required per 30 bits of data */
		*numerator = 32;
		*denominator = 30;
		break;
	case V4L2_PIX_FMT_XBGR40:
	case V4L2_PIX_FMT_XY12:
	case V4L2_PIX_FMT_X012:
	case V4L2_PIX_FMT_X012M:
	case V4L2_PIX_FMT_X212:
	case V4L2_PIX_FMT_X212M:
	case V4L2_PIX_FMT_X412:
	case V4L2_PIX_FMT_X412M:
		*numerator = 40;
		*denominator = 36;
		break;
	default:
		*numerator = 1;
		*denominator = 1;
		break;
	}
}
EXPORT_SYMBOL_GPL(xvip_width_padding_factor);

/**
 * xvip_of_get_format - Parse a device tree node and return format information
 * @node: the device tree node
 *
 * Read the xlnx,video-format, xlnx,video-width and xlnx,cfa-pattern properties
 * from the device tree @node passed as an argument and return the corresponding
 * format information.
 *
 * Return: a pointer to the format information structure corresponding to the
 * format name and width, or ERR_PTR if no corresponding format can be found.
 */
const struct xvip_video_format *xvip_of_get_format(struct device_node *node)
{
	const char *pattern = "mono";
	unsigned int vf_code = 0;
	unsigned int i;
	u32 width = 0;
	int ret;

	ret = of_property_read_u32(node, "xlnx,video-format", &vf_code);
	if (ret < 0)
		return ERR_PTR(ret);

	ret = of_property_read_u32(node, "xlnx,video-width", &width);
	if (ret < 0)
		return ERR_PTR(ret);

	if (vf_code == XVIP_VF_MONO_SENSOR) {
		ret = of_property_read_string(node,
					      "xlnx,cfa-pattern",
					      &pattern);
		if (ret < 0)
			return ERR_PTR(ret);
	}

	for (i = 0; i < ARRAY_SIZE(xvip_video_formats); ++i) {
		const struct xvip_video_format *format = &xvip_video_formats[i];

		if (format->vf_code != vf_code || format->width != width)
			continue;

		if (vf_code == XVIP_VF_MONO_SENSOR &&
		    strcmp(pattern, format->pattern))
			continue;

		return format;
	}

	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(xvip_of_get_format);

/**
 * xvip_set_format_size - Set the media bus frame format size
 * @format: V4L2 frame format on media bus
 * @fmt: media bus format
 *
 * Set the media bus frame format size. The width / height from the subdevice
 * format are set to the given media bus format. The new format size is stored
 * in @format. The width and height are clamped using default min / max values.
 */
void xvip_set_format_size(struct v4l2_mbus_framefmt *format,
			  const struct v4l2_subdev_format *fmt)
{
	format->width = clamp_t(unsigned int, fmt->format.width,
				XVIP_MIN_WIDTH, XVIP_MAX_WIDTH);
	format->height = clamp_t(unsigned int, fmt->format.height,
				 XVIP_MIN_HEIGHT, XVIP_MAX_HEIGHT);
}
EXPORT_SYMBOL_GPL(xvip_set_format_size);

/* -----------------------------------------------------------------------------
 * Video IP device operations
 */

static int xvip_device_parse_dt(struct xvip_device *xvip,
				const struct xvip_device_info *info)
{
	const unsigned int num_pads = xvip->num_sinks + xvip->num_sources;
	struct device_node *node = xvip->dev->of_node;
	struct device_node *ports;
	struct device_node *port;
	unsigned int num_ports = 0;
	u32 found_ports = 0;
	int ret = 0;

	ports = of_get_child_by_name(node, "ports");
	if (!ports)
		ports = of_node_get(node);

	for_each_child_of_node(ports, port) {
		const struct xvip_video_format *format;
		u32 index;

		if (!of_node_name_eq(port, "port"))
			continue;

		ret = of_property_read_u32(port, "reg", &index);
		if (ret) {
			dev_err(xvip->dev, "port %pOF has no reg property\n",
				port);
			of_node_put(port);
			break;
		}

		if (index >= num_pads) {
			dev_err(xvip->dev, "Invalid port number %u\n", index);
			of_node_put(port);
			ret = -EINVAL;
			break;
		}

		if (found_ports & BIT(index)) {
			dev_err(xvip->dev, "Duplicated port number %u in %pOF\n",
				index, port);
			of_node_put(port);
			ret = -EINVAL;
			break;
		}

		if (info->has_port_formats) {
			format = xvip_of_get_format(port);
			if (IS_ERR(format)) {
				dev_err(xvip->dev,
					"Failed to retrieve format for port %pOF\n",
					port);
				of_node_put(port);
				ret = PTR_ERR(format);
				break;
			}

			xvip->ports[index].format = format;
		}

		of_property_read_u32(port, "data-shift",
				     &xvip->ports[index].data_shift);

		found_ports |= BIT(index);
		num_ports++;
	}

	of_node_put(ports);

	if (ret)
		return ret;

	/* Validate the number of ports. */
	if (num_ports != xvip->num_sinks + xvip->num_sources) {
		dev_err(xvip->dev, "invalid number of ports: %u, expected %u\n",
			num_ports, xvip->num_sinks + xvip->num_sources);
		return -EINVAL;
	}

	return 0;
}

/**
 * xvip_device_init - Initialize a Xilinx video IP device
 * @xvip: The video IP device
 * @info: Device information
 *
 * Before being used, xvip_device instances must be initialized by a call to
 * this function.
 *
 * The @info structure describes the resources needed by the device. Those
 * resources are acquired by this function. No reference to the @info pointer is
 * stored, the caller isn't required to keep it valid after the function
 * returns.
 *
 * Every device successfully initialized by this function must be cleaned up by
 * a call to xvip_device_cleanup().
 *
 * Return: 0 on success or a negative error code on failure
 */
int xvip_device_init(struct xvip_device *xvip,
		     const struct xvip_device_info *info)
{
	struct platform_device *pdev = to_platform_device(xvip->dev);
	unsigned int num_pads;
	unsigned int i;
	int ret;

	xvip->num_sinks = info->num_sinks;
	xvip->num_sources = info->num_sources;

	num_pads = info->num_sinks + info->num_sources;

	if (num_pads) {
		xvip->ports = devm_kcalloc(xvip->dev, num_pads,
					   sizeof(*xvip->ports), GFP_KERNEL);
		if (!xvip->ports)
			return -ENOMEM;
	}

	ret = xvip_device_parse_dt(xvip, info);
	if (ret < 0)
		return ret;

	if (num_pads) {
		xvip->pads = devm_kcalloc(xvip->dev, num_pads,
					  sizeof(*xvip->pads), GFP_KERNEL);
		if (!xvip->pads)
			return -ENOMEM;

		for (i = 0; i < xvip->num_sinks; ++i)
			xvip->pads[i].flags = MEDIA_PAD_FL_SINK;
		for (; i < num_pads; ++i)
			xvip->pads[i].flags = MEDIA_PAD_FL_SOURCE;
	}

	if (info->has_axi_lite) {
		xvip->iomem = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(xvip->iomem))
			return PTR_ERR(xvip->iomem);
	}

	xvip->clk = devm_clk_get(xvip->dev, NULL);
	if (IS_ERR(xvip->clk))
		return PTR_ERR(xvip->clk);

	return clk_prepare_enable(xvip->clk);
}
EXPORT_SYMBOL_GPL(xvip_device_init);

/**
 * xvip_device_cleanup - Cleanup a Xilinx video IP device
 * @xvip: The video IP device
 *
 * This function is the counterpart of xvip_device_init() and must be called to
 * release all allocated resources before destroying the device.
 */
void xvip_device_cleanup(struct xvip_device *xvip)
{
	clk_disable_unprepare(xvip->clk);
}
EXPORT_SYMBOL_GPL(xvip_device_cleanup);

/**
 * xvip_clr_or_set - Clear or set the register with a bitmask
 * @xvip: Xilinx Video IP device
 * @addr: address of register
 * @mask: bitmask to be set or cleared
 * @set: boolean flag indicating whether to set or clear
 *
 * Clear or set the register at address @addr with a bitmask @mask depending on
 * the boolean flag @set. When the flag @set is true, the bitmask is set in
 * the register, otherwise the bitmask is cleared from the register
 * when the flag @set is false.
 *
 * Fox example, this function can be used to set a control with a boolean value
 * requested by users. If the caller knows whether to set or clear in the first
 * place, the caller should call xvip_clr() or xvip_set() directly instead of
 * using this function.
 */
void xvip_clr_or_set(struct xvip_device *xvip, u32 addr, u32 mask, bool set)
{
	u32 reg;

	reg = xvip_read(xvip, addr);
	reg = set ? reg | mask : reg & ~mask;
	xvip_write(xvip, addr, reg);
}
EXPORT_SYMBOL_GPL(xvip_clr_or_set);

/**
 * xvip_clr_and_set - Clear and set the register with a bitmask
 * @xvip: Xilinx Video IP device
 * @addr: address of register
 * @clr: bitmask to be cleared
 * @set: bitmask to be set
 *
 * Clear a bit(s) of mask @clr in the register at address @addr, then set
 * a bit(s) of mask @set in the register after.
 */
void xvip_clr_and_set(struct xvip_device *xvip, u32 addr, u32 clr, u32 set)
{
	u32 reg;

	reg = xvip_read(xvip, addr);
	reg &= ~clr;
	reg |= set;
	xvip_write(xvip, addr, reg);
}
EXPORT_SYMBOL_GPL(xvip_clr_and_set);

/* -----------------------------------------------------------------------------
 * Subdev operations handlers
 */

/**
 * xvip_enum_mbus_code - Enumerate the media format code
 * @subdev: V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @code: returning media bus code
 *
 * Enumerate the media bus code of the subdevice. Return the corresponding
 * pad format code. This function only works for subdevices with fixed format
 * on all pads. Subdevices with multiple format should have their own
 * function to enumerate mbus codes.
 *
 * Return: 0 if the media bus code is found, or -EINVAL if the format index
 * is not valid.
 */
int xvip_enum_mbus_code(struct v4l2_subdev *subdev,
			struct v4l2_subdev_state *sd_state,
			struct v4l2_subdev_mbus_code_enum *code)
{
	struct v4l2_mbus_framefmt *format;

	/* Enumerating frame sizes based on the active configuration isn't
	 * supported yet.
	 */
	if (code->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	if (code->index)
		return -EINVAL;

	format = v4l2_subdev_get_try_format(subdev, sd_state, code->pad);

	code->code = format->code;

	return 0;
}
EXPORT_SYMBOL_GPL(xvip_enum_mbus_code);

/**
 * xvip_enum_frame_size - Enumerate the media bus frame size
 * @subdev: V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fse: returning media bus frame size
 *
 * This function is a drop-in implementation of the subdev enum_frame_size pad
 * operation. It assumes that the subdevice has one sink pad and one source
 * pad, and that the format on the source pad is always identical to the
 * format on the sink pad. Entities with different requirements need to
 * implement their own enum_frame_size handlers.
 *
 * Return: 0 if the media bus frame size is found, or -EINVAL
 * if the index or the code is not valid.
 */
int xvip_enum_frame_size(struct v4l2_subdev *subdev,
			 struct v4l2_subdev_state *sd_state,
			 struct v4l2_subdev_frame_size_enum *fse)
{
	struct v4l2_mbus_framefmt *format;

	/* Enumerating frame sizes based on the active configuration isn't
	 * supported yet.
	 */
	if (fse->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	format = v4l2_subdev_get_try_format(subdev, sd_state, fse->pad);

	if (fse->index || fse->code != format->code)
		return -EINVAL;

	if (fse->pad == XVIP_PAD_SINK) {
		fse->min_width = XVIP_MIN_WIDTH;
		fse->max_width = XVIP_MAX_WIDTH;
		fse->min_height = XVIP_MIN_HEIGHT;
		fse->max_height = XVIP_MAX_HEIGHT;
	} else {
		/* The size on the source pad is fixed and always identical to
		 * the size on the sink pad.
		 */
		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xvip_enum_frame_size);

/**
 * xvip_link_validate - Validate a link between subdevs
 * @sd: The V4L2 subdevice on the sink side
 * @link: The link
 * @source_fmt: The format on the source pad
 * @sink_fmt: The format on the sink pad
 *
 * This function is a drop-in implementation of the subdev link_validate pad
 * operation. It is similar to v4l2_subdev_link_validate_default(), but takes
 * into account any data shift caused by an AXI stream subset converter.
 *
 * Return: 0 if the link configuratiion is valid, or a negative error code
 * otherwise.
 */
int xvip_link_validate(struct v4l2_subdev *sd, struct media_link *link,
		       struct v4l2_subdev_format *source_fmt,
		       struct v4l2_subdev_format *sink_fmt)
{
	struct v4l2_mbus_config mbus_config = { 0 };
	const struct xvip_video_format *source_info;
	const struct xvip_video_format *sink_info;
	unsigned int shift;
	int ret;

	/* The width and height must match. */
	if (source_fmt->format.width != sink_fmt->format.width
	    || source_fmt->format.height != sink_fmt->format.height)
		return -EPIPE;

	/*
	 * The field order must match, or the sink field order must be NONE
	 * to support interlaced hardware connected to bridges that support
	 * progressive formats only.
	 */
	if (source_fmt->format.field != sink_fmt->format.field &&
	    sink_fmt->format.field != V4L2_FIELD_NONE)
		return -EPIPE;

	/*
	 * Validate the media bus code. An AXI stream subset converter may be
	 * present on the link. It will be modelled, by convention, on the sink
	 * subdev.
	 */
	source_info = xvip_get_format_by_code(source_fmt->format.code);
	sink_info = xvip_get_format_by_code(sink_fmt->format.code);
	if (IS_ERR(source_info) || IS_ERR(sink_info))
		return -EPIPE;

	ret = v4l2_subdev_call(sd, pad, get_mbus_config,
			       link->sink->index, &mbus_config);
	switch (ret) {
	case 0:
		shift = mbus_config.bus.parallel.data_shift;
		break;
	case -ENOIOCTLCMD:
		shift = 0;
		break;
	default:
		return ret;
	}

	if ((source_info->flavor == 0 || sink_info->flavor == 0 ||
	     source_info->flavor != sink_info->flavor) && shift != 0)
		return -EPIPE;

	if (source_info->width - sink_info->width != shift)
		return -EPIPE;

	return 0;
}
EXPORT_SYMBOL_GPL(xvip_link_validate);

/**
 * xvip_get_mbus_config - Retrieve the bus configuration for a pad
 * @sd: The V4L2 subdevice on the sink side
 * @pad: The pad
 * @config: The bus configuration
 *
 * This function is a drop-in implementation of the subdev get_mbus_config pad
 * operation.
 *
 * Return: 0 if the pad is valid, or a negative error code otherwise.
 */
int xvip_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
			 struct v4l2_mbus_config *config)
{
	struct xvip_device *xvip = to_xvip_device(sd);

	if (pad >= xvip->num_sinks + xvip->num_sources)
		return -EINVAL;

	config->type = V4L2_MBUS_PARALLEL;
	config->bus.parallel.data_shift = xvip->ports[pad].data_shift;

	return 0;
}
EXPORT_SYMBOL_GPL(xvip_get_mbus_config);

static int __xvip_enable_connected_streams(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *state,
					   u32 pad, u64 streams_mask,
					   bool enable)
{
	struct xvip_device *xvip = to_xvip_device(sd);
	struct media_link *link;
	u64 *streams;
	int ret = 0;

	streams = kcalloc(sd->entity.num_pads, sizeof(*streams), GFP_KERNEL);
	if (!streams)
		return -ENOMEM;

	if (state) {
		struct v4l2_subdev_route *route;

		/* Collect the routed pads and their streams. */
		for_each_active_route(&state->routing, route) {
			if (route->sink_pad == pad &&
			    (streams_mask & BIT(route->sink_stream))) {
				streams[route->source_pad] |=
					BIT(route->source_stream);

				dev_dbg(xvip->dev,
					"Collected stream %u on pad %s/%u\n",
					route->source_stream, sd->entity.name,
					route->source_pad);
			}

			if (route->source_pad == pad &&
			    (streams_mask & BIT(route->source_stream))) {
				streams[route->sink_pad] |=
					BIT(route->sink_stream);

				dev_dbg(xvip->dev,
					"Collected stream %u on pad %s/%u\n",
					route->sink_stream, sd->entity.name,
					route->sink_pad);
			}
		}
	} else {
		struct media_pad *local_pad = &sd->entity.pads[pad];
		struct media_pad *other_pad;

		/*
		 * Not all Xilinx subdev have transitioned to active state
		 * management. Handle the legacy case by collecting all pads on
		 * the other side of the subdev.
		 */
		media_entity_for_each_pad(&sd->entity, other_pad) {
			if ((local_pad->flags ^ other_pad->flags) !=
			    (MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_SOURCE))
				continue;

			streams[other_pad->index] = streams_mask;

			dev_dbg(xvip->dev,
				"Collected pad %s/%u with streams 0x%llx\n",
				sd->entity.name, other_pad->index,
				streams_mask);
		}
	}

	/*
	 * Enable/disable streams on all remote pads connected to the collected
	 * local pads.
	 */
	list_for_each_entry(link, &sd->entity.links, list) {
		struct media_pad *local_pad;
		struct media_pad *remote_pad;
		struct v4l2_subdev *remote_sd;
		u64 link_streams;

		dev_dbg(xvip->dev, "Processing link %s/%u -> %s/%u\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index);

		/* Skip disabled links and non-data links. */
		if (!(link->flags & MEDIA_LNK_FL_ENABLED) ||
		    (link->flags & MEDIA_LNK_FL_LINK_TYPE) !=
		    MEDIA_LNK_FL_DATA_LINK)
			continue;

		if (link->source->entity == &sd->entity) {
			local_pad = link->source;
			remote_pad = link->sink;
		} else {
			local_pad = link->sink;
			remote_pad = link->source;
		}

		/* Skip pads that we haven't collected. */
		if (!streams[local_pad->index])
			continue;

		link_streams = streams[local_pad->index];

		/* Skip remote entities that are not subdevs. */
		if (!is_media_entity_v4l2_subdev(remote_pad->entity))
			continue;

		remote_sd = media_entity_to_v4l2_subdev(remote_pad->entity);

		dev_dbg(xvip->dev, "%s streams 0x%llx on %s/%u\n",
			enable ? "Enabling" : "Disabling", link_streams,
			remote_sd->entity.name, remote_pad->index);

		/* Enable/disable streams on the remote subdev. */
		if (enable)
			ret = v4l2_subdev_enable_streams(remote_sd,
							 remote_pad->index,
							 link_streams);
		else
			ret = v4l2_subdev_disable_streams(remote_sd,
							  remote_pad->index,
							  link_streams);

		if (ret) {
			/*
			 * TODO: Handle errors correctly by stopping all
			 * subdevs that have been successfully started.
			 */
			break;
		}
	}

	kfree(streams);

	return ret;
}

static int xvip_enable_connected_streams(struct v4l2_subdev *sd,
					 struct v4l2_subdev_state *state,
					 u32 pad, u64 streams_mask)
{
	return __xvip_enable_connected_streams(sd, state, pad, streams_mask,
					       true);
}

static int xvip_disable_connected_streams(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *state,
					  u32 pad, u64 streams_mask)
{
	return __xvip_enable_connected_streams(sd, state, pad, streams_mask,
					       false);
}

/**
 * xvip_enable_streams - Enable streams on a subdevice
 * @sd: The V4L2 subdevice
 * @state: The subdevice state
 * @pad: The pad
 * @streams_mask: The streams mask
 *
 * This function is a drop-in implementation of the subdev enable_streams pad
 * operation. It delegates enabling of the streams to the
 * &xvip_device_ops.enable_streams operation, and then forwards the call to
 * connected subdevs. If a device needs finer grained control on how the
 * connected subdevs are enabled, it should implement the
 * &v4l2_subdev_pad_ops.enable_streams operation directly instead of using this
 * helper.
 *
 * Device that don't need to perform any operation when enabling streams may
 * leave the &xvip_device_ops.enable_streams operation unimplemented.
 *
 * Return: 0 on success, or a negative error code otherwise.
 */
int xvip_enable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			u32 pad, u64 streams_mask)
{
	struct xvip_device *xvip = to_xvip_device(sd);
	int ret;

	if (xvip->ops && xvip->ops->enable_streams) {
		dev_dbg(xvip->dev, "Enabling streams 0x%llx on xvip %s/%u\n",
			streams_mask, sd->entity.name, pad);

		ret = xvip->ops->enable_streams(sd, state, pad, streams_mask);
		if (ret)
			return ret;
	}

	ret = xvip_enable_connected_streams(sd, state, pad, streams_mask);
	if (ret) {
		if (xvip->ops && xvip->ops->disable_streams)
			xvip->ops->disable_streams(sd, state, pad, streams_mask);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xvip_enable_streams);

/**
 * xvip_disable_streams - Disables streams on a subdevice
 * @sd: The V4L2 subdevice
 * @state: The subdevice state
 * @pad: The pad
 * @streams_mask: The streams mask
 *
 * This function is a drop-in implementation of the subdev disable_streams pad
 * operation. It forwards the call to connected subdevs, and then delegates
 * disabling of the streams to the &xvip_device_ops.disable_streams operation.
 * If a device needs finer grained control on how the connected subdevs are
 * disabled, it should implement the &v4l2_subdev_pad_ops.disable_streams
 * operation directly instead of using this helper.
 *
 * Device that don't need to perform any operation when disabling streams may
 * leave the &xvip_device_ops.disable_streams operation unimplemented.
 *
 * Return: 0 on success, or a negative error code otherwise.
 */
int xvip_disable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			u32 pad, u64 streams_mask)
{
	struct xvip_device *xvip = to_xvip_device(sd);
	int ret;

	ret = xvip_disable_connected_streams(sd, state, pad, streams_mask);
	if (ret)
		return ret;

	if (xvip->ops && xvip->ops->disable_streams) {
		dev_dbg(xvip->dev, "Disabling streams 0x%llx on xvip %s/%u\n",
			streams_mask, sd->entity.name, pad);

		ret = xvip->ops->disable_streams(sd, state, pad, streams_mask);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xvip_disable_streams);

/**
 * xvip_s_streams - Start or stop streaming on a subdevice
 * @sd: The V4L2 subdevice
 * @enable: True to start streaming, false to stop it
 *
 * This function is a drop-in implementation of the subdev s_stream video
 * operation. It is meant to handle the transition to per-stream control and
 * should be removed once the transition completes.
 *
 * Return: 0 on success, or a negative error code otherwise.
 */
int xvip_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct v4l2_subdev_state *state;
	struct media_pad *pad;
	int pad_index = -1;
	u64 streams = BIT(1);
	int ret;

	/*
	 * Find a source pad and collect all source streams to call the
	 * stream enable/disable operations. This isn't entirely
	 * correct, but should work well enough to handle the transition
	 * away from .s_stream().
	 */
	media_entity_for_each_pad(&sd->entity, pad) {
		if (pad->flags & MEDIA_PAD_FL_SOURCE) {
			pad_index = pad->index;
			break;
		}
	}

	if (WARN_ON(pad_index == -1))
		return -EINVAL;

	state = v4l2_subdev_get_unlocked_active_state(sd);
	if (state) {
		struct v4l2_subdev_route *route;

		v4l2_subdev_lock_state(state);

		streams = 0;

		for_each_active_route(&state->routing, route)
			streams |= BIT(route->source_stream);
	}

	if (enable)
		ret = v4l2_subdev_call(sd, pad, enable_streams, state,
				       pad_index, streams);
	else
		ret = v4l2_subdev_call(sd, pad, disable_streams, state,
				       pad_index, streams);

	if (state)
		v4l2_subdev_unlock_state(state);

	return ret;
}
EXPORT_SYMBOL_GPL(xvip_s_stream);
