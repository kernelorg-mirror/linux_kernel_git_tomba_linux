// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Texas Instruments Incorporated - https://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "tidss_crtc.h"
#include "tidss_dispc.h"
#include "tidss_drv.h"
#include "tidss_plane.h"

void tidss_plane_error_irq(struct drm_plane *plane, u64 irqstatus)
{
	struct tidss_plane *tplane = to_tidss_plane(plane);

	dev_err_ratelimited(plane->dev->dev, "Plane%u underflow (irq %llx)\n",
			    tplane->hw_plane_id, irqstatus);
}

/* drm_plane_helper_funcs */

static int tidss_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane *tplane = to_tidss_plane(plane);
	const struct drm_format_info *finfo;
	struct drm_crtc_state *crtc_state;
	u32 hw_plane = tplane->hw_plane_id;
	u32 hw_videoport;
	int ret;

	if (!new_plane_state->crtc) {
		/*
		 * The visible field is not reset by the DRM core but only
		 * updated by drm_atomic_helper_check_plane_state(), set it
		 * manually.
		 */
		new_plane_state->visible = false;
		return 0;
	}

	crtc_state = drm_atomic_get_crtc_state(state,
					       new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  0,
						  INT_MAX, true, true);
	if (ret < 0)
		return ret;

	/*
	 * The HW is only able to start drawing at subpixel boundary
	 * (the two first checks below). At the end of a row the HW
	 * can only jump integer number of subpixels forward to the
	 * beginning of the next row. So we can only show picture with
	 * integer subpixel width (the third check). However, after
	 * reaching the end of the drawn picture the drawing starts
	 * again at the absolute memory address where top left corner
	 * position of the drawn picture is (so there is no need to
	 * check for odd height).
	 */

	finfo = drm_format_info(new_plane_state->fb->format->format);

	if ((new_plane_state->src_x >> 16) % finfo->hsub != 0) {
		dev_dbg(ddev->dev,
			"%s: x-position %u not divisible subpixel size %u\n",
			__func__, (new_plane_state->src_x >> 16), finfo->hsub);
		return -EINVAL;
	}

	if ((new_plane_state->src_y >> 16) % finfo->vsub != 0) {
		dev_dbg(ddev->dev,
			"%s: y-position %u not divisible subpixel size %u\n",
			__func__, (new_plane_state->src_y >> 16), finfo->vsub);
		return -EINVAL;
	}

	if ((new_plane_state->src_w >> 16) % finfo->hsub != 0) {
		dev_dbg(ddev->dev,
			"%s: src width %u not divisible by subpixel size %u\n",
			 __func__, (new_plane_state->src_w >> 16),
			 finfo->hsub);
		return -EINVAL;
	}

	if (!new_plane_state->visible)
		return 0;

	hw_videoport = to_tidss_crtc(new_plane_state->crtc)->hw_videoport;

	ret = dispc_plane_check(tidss->dispc, hw_plane, new_plane_state,
				hw_videoport);
	if (ret)
		return ret;

	return 0;
}

static void tidss_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane *tplane = to_tidss_plane(plane);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	u32 hw_videoport;

	if (!new_state->visible) {
		dispc_plane_enable(tidss->dispc, tplane->hw_plane_id, false);
		return;
	}

	hw_videoport = to_tidss_crtc(new_state->crtc)->hw_videoport;

	dispc_plane_setup(tidss->dispc, tplane->hw_plane_id, new_state, hw_videoport);
}

static void tidss_plane_atomic_enable(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane *tplane = to_tidss_plane(plane);

	dispc_plane_enable(tidss->dispc, tplane->hw_plane_id, true);
}

static void tidss_plane_atomic_disable(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_plane *tplane = to_tidss_plane(plane);

	dispc_plane_enable(tidss->dispc, tplane->hw_plane_id, false);
}

static void drm_plane_destroy(struct drm_plane *plane)
{
	struct tidss_plane *tplane = to_tidss_plane(plane);

	drm_plane_cleanup(plane);
	kfree(tplane);
}

static const struct drm_plane_helper_funcs tidss_plane_helper_funcs = {
	.atomic_check = tidss_plane_atomic_check,
	.atomic_update = tidss_plane_atomic_update,
	.atomic_enable = tidss_plane_atomic_enable,
	.atomic_disable = tidss_plane_atomic_disable,
};

static const struct drm_plane_helper_funcs tidss_primary_plane_helper_funcs = {
	.atomic_check = tidss_plane_atomic_check,
	.atomic_update = tidss_plane_atomic_update,
	.atomic_enable = tidss_plane_atomic_enable,
	.atomic_disable = tidss_plane_atomic_disable,
	.get_scanout_buffer = drm_fb_dma_get_scanout_buffer,
};

static const struct drm_framebuffer_funcs tidss_plane_readout_fb_funcs = {
	.destroy	= drm_gem_fb_destroy,
};

static struct drm_framebuffer *tidss_plane_readout_fb(struct drm_plane *plane)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct dispc_device *dispc = tidss->dispc;
	struct tidss_plane *tplane = to_tidss_plane(plane);
	struct drm_framebuffer *fb;
	int ret;

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return ERR_PTR(-ENOMEM);

	fb->dev = plane->dev;

	ret = dispc_fb_state_readout(dispc, tplane->hw_plane_id, fb);
	if (ret)
		goto err_free_fb;

	ret = drm_framebuffer_init(plane->dev, fb, &tidss_plane_readout_fb_funcs);
	if (ret) {
		kfree(fb);
		return ERR_PTR(ret);
	}

	return fb;

err_free_fb:
	kfree(fb);
	return ERR_PTR(ret);
}

static struct drm_plane_state *tidss_plane_atomic_readout_state(struct drm_plane *plane,
								struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct dispc_device *dispc = tidss->dispc;
	struct tidss_plane *tplane = to_tidss_plane(plane);
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct drm_framebuffer *fb;
	struct drm_crtc *crtc;
	int ret;

	if (plane->state)
		drm_atomic_helper_plane_destroy_state(plane, plane->state);

	plane_state = kzalloc(sizeof(*plane_state), GFP_KERNEL);
	if (!plane_state)
		return ERR_PTR(-ENOMEM);

	__drm_atomic_helper_plane_state_reset(plane_state, plane);

	bool vid_enable = dispc_plane_is_enabled(dispc, tplane->hw_plane_id);

	if (!vid_enable)
		goto out;

	bool ovr_enable;
	u32 hw_videoport;
	dispc_ovr_readout_plane(dispc, tplane->hw_plane_id, &ovr_enable,
				&hw_videoport);

	if (!ovr_enable)
		goto out;

	fb = tidss_plane_readout_fb(plane);
	if (IS_ERR(fb)) {
		ret = PTR_ERR(fb);
		goto err_free_state;
	}

	crtc = NULL;
	for (int crtc_idx = 0; crtc_idx < tidss->num_crtcs; ++crtc_idx) {
		if (to_tidss_crtc(tidss->crtcs[crtc_idx])->hw_videoport ==
		    hw_videoport) {
			crtc = tidss->crtcs[crtc_idx];
			break;
		}
	}

	if (!crtc) {
		ret = -ENODEV;
		goto err_free_state;
	}

	plane_state->fb = fb;
	plane_state->crtc = crtc;
	plane_state->visible = true;

	dispc_vid_state_readout(dispc, tplane->hw_plane_id, plane_state);

	crtc_state = drm_atomic_get_old_crtc_state(state, crtc);
	if (!crtc_state) {
		ret = -ENODEV;
		goto err_free_state;
	}

	crtc_state->plane_mask |= drm_plane_mask(plane);

out:
	return plane_state;

err_free_state:
	kfree(plane_state);
	return ERR_PTR(ret);
}

static const struct drm_plane_funcs tidss_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_destroy,
	.atomic_compare_state = drm_atomic_helper_plane_compare_state,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
	.atomic_readout_state = tidss_plane_atomic_readout_state,
};

struct tidss_plane *tidss_plane_create(struct tidss_device *tidss,
				       u32 hw_plane_id, u32 plane_type,
				       u32 crtc_mask, const u32 *formats,
				       u32 num_formats)
{
	struct tidss_plane *tplane;
	enum drm_plane_type type;
	u32 possible_crtcs;
	u32 num_planes = tidss->feat->num_vids;
	u32 color_encodings = (BIT(DRM_COLOR_YCBCR_BT601) |
			       BIT(DRM_COLOR_YCBCR_BT709));
	u32 color_ranges = (BIT(DRM_COLOR_YCBCR_FULL_RANGE) |
			    BIT(DRM_COLOR_YCBCR_LIMITED_RANGE));
	u32 default_encoding = DRM_COLOR_YCBCR_BT601;
	u32 default_range = DRM_COLOR_YCBCR_FULL_RANGE;
	u32 blend_modes = (BIT(DRM_MODE_BLEND_PREMULTI) |
			   BIT(DRM_MODE_BLEND_COVERAGE));
	int ret;

	tplane = kzalloc_obj(*tplane);
	if (!tplane)
		return ERR_PTR(-ENOMEM);

	tplane->hw_plane_id = hw_plane_id;

	possible_crtcs = crtc_mask;
	type = plane_type;

	ret = drm_universal_plane_init(&tidss->ddev, &tplane->plane,
				       possible_crtcs,
				       &tidss_plane_funcs,
				       formats, num_formats,
				       NULL, type, NULL);
	if (ret < 0)
		goto err;

	if (type == DRM_PLANE_TYPE_PRIMARY)
		drm_plane_helper_add(&tplane->plane, &tidss_primary_plane_helper_funcs);
	else
		drm_plane_helper_add(&tplane->plane, &tidss_plane_helper_funcs);

	drm_plane_create_zpos_property(&tplane->plane, tidss->num_planes, 0,
				       num_planes - 1);

	ret = drm_plane_create_color_properties(&tplane->plane,
						color_encodings,
						color_ranges,
						default_encoding,
						default_range);
	if (ret)
		goto err;

	ret = drm_plane_create_alpha_property(&tplane->plane);
	if (ret)
		goto err;

	ret = drm_plane_create_blend_mode_property(&tplane->plane, blend_modes);
	if (ret)
		goto err;

	return tplane;

err:
	kfree(tplane);
	return ERR_PTR(ret);
}
