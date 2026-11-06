// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/export.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_helper.h>
#include <drm/drm_modeset_lock.h>

/**
 * drm_bridge_helper_reset_crtc - Reset the pipeline feeding a bridge
 * @bridge: DRM bridge to reset
 * @ctx: lock acquisition context
 *
 * Reset a @bridge pipeline. It will power-cycle all active components
 * between the CRTC and connector that bridge is connected to.
 *
 * As it relies on drm_atomic_helper_reset_crtc(), the same limitations
 * apply.
 *
 * All the locks taken by this function use @ctx: the caller is
 * responsible for releasing them with drm_modeset_drop_locks(), and
 * for handling an -EDEADLK error with drm_modeset_backoff().
 *
 * Returns:
 *
 * 0 on success or a negative error code on failure. If the error
 * returned is EDEADLK, the whole atomic sequence must be restarted.
 */
int drm_bridge_helper_reset_crtc(struct drm_bridge *bridge,
				 struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_connector *connector;
	struct drm_encoder *encoder = bridge->encoder;
	struct drm_device *dev = encoder->dev;
	struct drm_crtc *crtc;
	int ret;

	/*
	 * Every lock taken with @ctx is released either by a backoff or
	 * by the caller through drm_modeset_drop_locks(), so we must not
	 * unlock the connection mutex ourselves.
	 */
	ret = drm_modeset_lock(&dev->mode_config.connection_mutex, ctx);
	if (ret)
		return ret;

	connector = drm_atomic_get_connector_for_encoder(encoder, ctx);
	if (IS_ERR(connector))
		return PTR_ERR(connector);

	/*
	 * drm_atomic_get_connector_for_encoder() drops the connection
	 * mutex on its way out, whether or not it was held through @ctx,
	 * so we need to take it back before looking at the connector
	 * state.
	 */
	ret = drm_modeset_lock(&dev->mode_config.connection_mutex, ctx);
	if (ret)
		return ret;

	if (!connector->state)
		return -EINVAL;

	/*
	 * A concurrent atomic commit may have run while the connection
	 * mutex was released above, and re-routed the connector to
	 * another encoder or CRTC. Make sure the connector is still
	 * tied to our encoder before resetting anything.
	 */
	if (connector->state->best_encoder != encoder)
		return -EINVAL;

	crtc = connector->state->crtc;
	if (!crtc)
		return -EINVAL;

	return drm_atomic_helper_reset_crtc(crtc, ctx);
}
EXPORT_SYMBOL(drm_bridge_helper_reset_crtc);
