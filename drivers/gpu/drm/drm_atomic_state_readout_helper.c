// SPDX-License-Identifier: GPL-2.0-or-later

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_readout.h>
#include <drm/drm_atomic_state_readout_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_colorop.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>

#include <linux/gfp.h>
#include <linux/sched/mm.h>

static int
drm_atomic_helper_sro_readout_plane_state(struct drm_device *dev,
					  struct drm_atomic_sro_state *state,
					  struct drm_plane *plane)
{
	const struct drm_plane_funcs *plane_funcs = plane->funcs;
	struct drm_plane_state *plane_state;
	int ret;

	if (!plane_funcs->atomic_sro_readout_state) {
		/*
		 * Missing readout implementations are only allowed
		 * when drm.atomic_readout skips them, but the object
		 * still needs a valid state: fall back to a pristine
		 * state, like drm_mode_config_create_state() would.
		 */
		drm_warn(dev, "No plane readout implementation.");

		plane_state = plane_funcs->atomic_create_state(plane);
		if (drm_WARN_ON(dev, IS_ERR(plane_state)))
			return PTR_ERR(plane_state);

		drm_atomic_sro_set_plane_state(state, plane, plane_state);
		return 0;
	}

	drm_dbg_kms(dev, "Initializing Plane %s state.\n", plane->name);

	plane_state = plane_funcs->atomic_create_state(plane);
	if (drm_WARN_ON(dev, IS_ERR(plane_state)))
		return PTR_ERR(plane_state);

	ret = plane_funcs->atomic_sro_readout_state(plane, state,
						    plane_state);
	if (ret) {
		drm_dbg_kms(dev, "Plane %s readout failed: %pe\n",
			    plane->name, ERR_PTR(ret));
		plane_funcs->atomic_destroy_state(plane, plane_state);
		return ret;
	}

	drm_atomic_sro_set_plane_state(state, plane, plane_state);

	return 0;
}

static int
drm_atomic_helper_sro_readout_commit_planes_state(struct drm_device *dev,
						  struct drm_atomic_sro_state *state,
						  struct drm_atomic_commit *commit)
{
	struct drm_plane_state *new_plane_state;
	struct drm_plane *plane;
	unsigned int i;
	int ret;

	if (!commit)
		return -EINVAL;

	for_each_new_plane_in_state(commit, plane, new_plane_state, i) {
		ret = drm_atomic_helper_sro_readout_plane_state(dev, state, plane);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * drm_atomic_helper_sro_readout_planes_state - read out all plane states from hardware
 * @dev: DRM device
 * @state: SRO state container to fill
 * @commit: optional atomic commit limiting the objects to read out
 *
 * Iterates over the planes that have a state in @commit when it isn't
 * NULL, or over all the planes of @dev otherwise. For each, allocates
 * a state via atomic_create_state, and calls the plane's
 * atomic_sro_readout_state hook to fill it from hardware.
 *
 * RETURNS:
 *
 * 0 on success, a negative error code otherwise.
 */
int
drm_atomic_helper_sro_readout_planes_state(struct drm_device *dev,
					   struct drm_atomic_sro_state *state,
					   struct drm_atomic_commit *commit)
{
	struct drm_plane *plane;
	int ret;

	might_alloc(GFP_KERNEL);

	if (commit)
		return drm_atomic_helper_sro_readout_commit_planes_state(dev, state, commit);

	drm_for_each_plane(plane, dev) {
		ret = drm_atomic_helper_sro_readout_plane_state(dev, state, plane);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_sro_readout_planes_state);

static int
drm_atomic_helper_sro_readout_crtc_state(struct drm_device *dev,
					 struct drm_atomic_sro_state *state,
					 struct drm_crtc *crtc)
{
	const struct drm_crtc_funcs *crtc_funcs = crtc->funcs;
	struct drm_crtc_state *crtc_state;
	int ret;

	if (!crtc_funcs->atomic_sro_readout_state) {
		/*
		 * Missing readout implementations are only allowed
		 * when drm.atomic_readout skips them, but the object
		 * still needs a valid state: fall back to a pristine
		 * state, like drm_mode_config_create_state() would.
		 */
		drm_warn(dev, "No CRTC readout implementation.");

		crtc_state = crtc_funcs->atomic_create_state(crtc);
		if (drm_WARN_ON(dev, IS_ERR(crtc_state)))
			return PTR_ERR(crtc_state);

		drm_atomic_sro_set_crtc_state(state, crtc, crtc_state);
		return 0;
	}

	drm_dbg_kms(dev, "Initializing CRTC %s state.\n", crtc->name);

	crtc_state = crtc_funcs->atomic_create_state(crtc);
	if (drm_WARN_ON(dev, IS_ERR(crtc_state)))
		return PTR_ERR(crtc_state);

	ret = crtc_funcs->atomic_sro_readout_state(crtc, state, crtc_state);
	if (ret) {
		drm_dbg_kms(dev, "CRTC %s readout failed: %pe\n",
			    crtc->name, ERR_PTR(ret));
		crtc_funcs->atomic_destroy_state(crtc, crtc_state);
		return ret;
	}

	drm_atomic_sro_set_crtc_state(state, crtc, crtc_state);

	return 0;
}

static int
drm_atomic_helper_sro_readout_commit_crtcs_state(struct drm_device *dev,
						 struct drm_atomic_sro_state *state,
						 struct drm_atomic_commit *commit)
{
	struct drm_crtc_state *new_crtc_state, *readout_crtc_state;
	struct drm_crtc *crtc;
	unsigned int i;
	int ret;

	if (!commit)
		return -EINVAL;

	for_each_new_crtc_in_state(commit, crtc, new_crtc_state, i) {
		ret = drm_atomic_helper_sro_readout_crtc_state(dev, state, crtc);
		if (ret)
			return ret;

		/*
		 * A commit can include a CRTC without including all the
		 * planes and connectors enabled on it, a plane property
		 * update for instance, but the readout CRTC state needs a
		 * complete plane_mask, connector_mask and encoder_mask to
		 * compare against the committed one.
		 *
		 * Only the objects of the commit are read out, as their
		 * locks are the only ones held, so report the objects the
		 * commit doesn't touch as enabled: the commit doesn't
		 * affect them, and the objects it does affect are
		 * verified by their own comparison hooks.
		 */
		readout_crtc_state = drm_atomic_sro_get_crtc_state(state, crtc);
		if (readout_crtc_state) {
			readout_crtc_state->plane_mask |= new_crtc_state->plane_mask;
			readout_crtc_state->connector_mask |= new_crtc_state->connector_mask;
			readout_crtc_state->encoder_mask |= new_crtc_state->encoder_mask;
		}
	}

	return 0;
}

/**
 * drm_atomic_helper_sro_readout_crtcs_state - read out all CRTC states from hardware
 * @dev: DRM device
 * @state: SRO state container to fill
 * @commit: optional atomic commit limiting the objects to read out
 *
 * Iterates over the CRTCs that have a state in @commit when it isn't
 * NULL, or over all the CRTCs of @dev otherwise. For each, allocates a
 * state via atomic_create_state, and calls the CRTC's
 * atomic_sro_readout_state hook to fill it from hardware.
 *
 * RETURNS:
 *
 * 0 on success, a negative error code otherwise.
 */
int
drm_atomic_helper_sro_readout_crtcs_state(struct drm_device *dev,
					  struct drm_atomic_sro_state *state,
					  struct drm_atomic_commit *commit)
{
	struct drm_crtc *crtc;
	int ret;

	might_alloc(GFP_KERNEL);

	if (commit)
		return drm_atomic_helper_sro_readout_commit_crtcs_state(dev, state, commit);

	drm_for_each_crtc(crtc, dev) {
		ret = drm_atomic_helper_sro_readout_crtc_state(dev, state, crtc);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_sro_readout_crtcs_state);

static int
drm_atomic_helper_sro_readout_connector_state(struct drm_device *dev,
					      struct drm_atomic_sro_state *state,
					      struct drm_connector *connector)
{
	const struct drm_connector_funcs *conn_funcs = connector->funcs;
	struct drm_connector_state *conn_state;
	int ret;

	if (!conn_funcs->atomic_sro_readout_state) {
		/*
		 * Missing readout implementations are only allowed
		 * when drm.atomic_readout skips them, but the object
		 * still needs a valid state: fall back to a pristine
		 * state, like drm_mode_config_create_state() would.
		 */
		drm_warn(dev, "No connector readout implementation.");

		conn_state = conn_funcs->atomic_create_state(connector);
		if (drm_WARN_ON(dev, IS_ERR(conn_state)))
			return PTR_ERR(conn_state);

		drm_atomic_sro_set_connector_state(state, connector, conn_state);
		return 0;
	}

	drm_dbg_kms(dev, "Initializing connector %s state.\n",
		    connector->name);

	conn_state = conn_funcs->atomic_create_state(connector);
	if (drm_WARN_ON(dev, IS_ERR(conn_state)))
		return PTR_ERR(conn_state);

	ret = conn_funcs->atomic_sro_readout_state(connector, state,
						   conn_state);
	if (ret) {
		drm_dbg_kms(dev, "Connector %s readout failed: %pe\n",
			    connector->name, ERR_PTR(ret));
		conn_funcs->atomic_destroy_state(connector, conn_state);
		return ret;
	}

	drm_atomic_sro_set_connector_state(state, connector, conn_state);

	return 0;
}

static int
drm_atomic_helper_sro_readout_commit_connectors_state(struct drm_device *dev,
						      struct drm_atomic_sro_state *state,
						      struct drm_atomic_commit *commit)
{
	struct drm_connector_state *new_connector_state;
	struct drm_connector *connector;
	unsigned int i;
	int ret;

	if (!commit)
		return -EINVAL;

	for_each_new_connector_in_state(commit, connector, new_connector_state, i) {
		ret = drm_atomic_helper_sro_readout_connector_state(dev, state, connector);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * drm_atomic_helper_sro_readout_connectors_state - read out all connector states from hardware
 * @dev: DRM device
 * @state: SRO state container to fill
 * @commit: optional atomic commit limiting the objects to read out
 *
 * Iterates over the connectors that have a state in @commit when it
 * isn't NULL, or over all the connectors of @dev otherwise. For each,
 * allocates a state via atomic_create_state, and calls the connector's
 * atomic_sro_readout_state hook to fill it from hardware.
 *
 * RETURNS:
 *
 * 0 on success, a negative error code otherwise.
 */
int
drm_atomic_helper_sro_readout_connectors_state(struct drm_device *dev,
					       struct drm_atomic_sro_state *state,
					       struct drm_atomic_commit *commit)
{
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector;
	int ret;

	might_alloc(GFP_KERNEL);

	if (commit)
		return drm_atomic_helper_sro_readout_commit_connectors_state(dev, state, commit);

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		ret = drm_atomic_helper_sro_readout_connector_state(dev, state, connector);
		if (ret) {
			drm_connector_list_iter_end(&conn_iter);
			return ret;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_sro_readout_connectors_state);

static int
drm_atomic_helper_sro_readout_bridge_state(struct drm_device *dev,
					   struct drm_atomic_sro_state *state,
					   struct drm_bridge *bridge)
{
	struct drm_private_obj *bridge_obj = &bridge->base;
	const struct drm_private_state_funcs *bridge_obj_funcs =
		bridge_obj->funcs;
	struct drm_private_state *bridge_obj_state;
	int ret;

	/*
	 * The same bridge chain can be reached through several
	 * connectors of the commit. This happens for instance when a
	 * commit disables a connector and routes another one to its
	 * encoder: the chain is then reached through the old state of
	 * the connector being disabled and through the new state of
	 * the connector taking over. Don't read a bridge out twice.
	 */
	if (drm_atomic_sro_get_private_obj_state(state, bridge_obj))
		return 0;

	if (!bridge_obj_funcs->atomic_sro_readout_state) {
		/*
		 * Missing readout implementations are only allowed
		 * when drm.atomic_readout skips them, but the object
		 * still needs a valid state: fall back to a pristine
		 * state, like drm_mode_config_create_state() would.
		 */
		drm_warn(dev, "No bridge %s readout implementation.",
			 bridge->name);

		bridge_obj_state = bridge_obj_funcs->atomic_create_state(bridge_obj);
		if (drm_WARN_ON(dev, IS_ERR(bridge_obj_state)))
			return PTR_ERR(bridge_obj_state);

		drm_atomic_sro_set_private_obj_state(state, bridge_obj,
						     bridge_obj_state);
		return 0;
	}

	drm_dbg_kms(dev, "Initializing Bridge %s state", bridge->name);

	bridge_obj_state = bridge_obj_funcs->atomic_create_state(bridge_obj);
	if (drm_WARN_ON(dev, IS_ERR(bridge_obj_state)))
		return PTR_ERR(bridge_obj_state);

	ret = bridge_obj_funcs->atomic_sro_readout_state(bridge_obj, state,
						       bridge_obj_state);
	if (ret) {
		drm_dbg_kms(dev, "Bridge %s readout failed: %pe\n",
			    bridge->name, ERR_PTR(ret));
		bridge_obj_funcs->atomic_destroy_state(bridge_obj,
						       bridge_obj_state);
		return ret;
	}

	drm_atomic_sro_set_private_obj_state(state, bridge_obj,
					     bridge_obj_state);

	return 0;
}

static int
drm_atomic_helper_sro_readout_bridge_chain_state(struct drm_device *dev,
						 struct drm_atomic_sro_state *state,
						 struct drm_encoder *encoder)
{
	int ret;

	drm_for_each_bridge_in_chain(encoder, bridge) {
		ret = drm_atomic_helper_sro_readout_bridge_state(dev, state, bridge);
		if (ret)
			return ret;
	}

	return 0;
}

static int
drm_atomic_helper_sro_readout_commit_bridges_state(struct drm_device *dev,
						   struct drm_atomic_sro_state *state,
						   struct drm_atomic_commit *commit)
{
	struct drm_connector_state *old_conn_state, *new_conn_state;
	struct drm_connector *connector;
	unsigned int i;

	if (!commit)
		return -EINVAL;

	/*
	 * Bridges can only be part of a commit through the connector
	 * they are routed to, either the one they were routed to before
	 * the commit or the one they are routed to after. Walking the
	 * chains of the encoders of the committed connectors thus
	 * covers all the bridges the commit can contain.
	 */
	for_each_oldnew_connector_in_state(commit, connector, old_conn_state, new_conn_state, i) {
		struct drm_encoder *old_encoder = old_conn_state->best_encoder;
		struct drm_encoder *new_encoder = new_conn_state->best_encoder;
		int ret;

		if (old_encoder) {
			ret = drm_atomic_helper_sro_readout_bridge_chain_state(dev,
									       state,
									       old_encoder);
			if (ret)
				return ret;
		}

		if (new_encoder && new_encoder != old_encoder) {
			ret = drm_atomic_helper_sro_readout_bridge_chain_state(dev,
									       state,
									       new_encoder);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * drm_atomic_helper_sro_readout_bridges_state - read out all bridge states from hardware
 * @dev: DRM device
 * @state: SRO state container to fill
 * @commit: optional atomic commit limiting the objects to read out
 *
 * Iterates over the bridge chains of the encoders of the connectors
 * that have a state in @commit when it isn't NULL, or over all the
 * encoders of @dev otherwise. For each bridge, allocates a state via
 * atomic_create_state, and calls the bridge's atomic_sro_readout_state
 * hook to fill it from hardware.
 *
 * Bridges are handled separately from other private objects because
 * bridge registration does not guarantee ordering. Traversing the
 * encoder bridge chains ensures each bridge can query the state of
 * preceding bridges in its chain.
 *
 * RETURNS:
 *
 * 0 on success, a negative error code otherwise.
 */
int
drm_atomic_helper_sro_readout_bridges_state(struct drm_device *dev,
					    struct drm_atomic_sro_state *state,
					    struct drm_atomic_commit *commit)
{
	struct drm_encoder *encoder;
	int ret;

	might_alloc(GFP_KERNEL);

	/*
	 * It works a bit differently for bridges. Indeed they rely on a
	 * drm_private_obj and drm_private_state, but bridge
	 * registration doesn't guarantee ordering.
	 *
	 * In order for each bridge callback to be able to query the
	 * previous bridge state, we thus need to read out the bridge
	 * state by looking at each encoder and then traversing its
	 * bridge list.
	 *
	 * And we'll then readout all the non-bridge drm_private_obj
	 * later.
	 */

	if (commit)
		return drm_atomic_helper_sro_readout_commit_bridges_state(dev, state, commit);

	drm_for_each_encoder(encoder, dev) {
		ret = drm_atomic_helper_sro_readout_bridge_chain_state(dev, state, encoder);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_sro_readout_bridges_state);

static int
drm_atomic_helper_sro_readout_private_obj_state(struct drm_device *dev,
						struct drm_atomic_sro_state *state,
						struct drm_private_obj *privobj)
{
	const struct drm_private_state_funcs *priv_funcs = privobj->funcs;
	struct drm_private_state *priv_state;
	int ret;

	/*
	 * The bridges are read out by
	 * drm_atomic_helper_sro_readout_bridges_state() instead, in
	 * bridge chain order.
	 */
	if (drm_private_obj_is_bridge(privobj))
		return 0;

	if (!priv_funcs->atomic_sro_readout_state) {
		/*
		 * Missing readout implementations are only allowed
		 * when drm.atomic_readout skips them, but the object
		 * still needs a valid state: fall back to a pristine
		 * state, like drm_mode_config_create_state() would.
		 */
		drm_warn(dev, "No private object %s readout implementation.",
			 privobj->name);

		priv_state = priv_funcs->atomic_create_state(privobj);
		if (drm_WARN_ON(dev, IS_ERR(priv_state)))
			return PTR_ERR(priv_state);

		drm_atomic_sro_set_private_obj_state(state, privobj, priv_state);
		return 0;
	}

	drm_dbg_kms(dev, "Initializing Private Object %s state", privobj->name);

	priv_state = priv_funcs->atomic_create_state(privobj);
	if (drm_WARN_ON(dev, IS_ERR(priv_state)))
		return PTR_ERR(priv_state);

	ret = priv_funcs->atomic_sro_readout_state(privobj, state,
						   priv_state);
	if (ret) {
		drm_dbg_kms(dev, "Private object %s readout failed: %pe\n",
			    privobj->name, ERR_PTR(ret));
		priv_funcs->atomic_destroy_state(privobj, priv_state);
		return ret;
	}

	drm_atomic_sro_set_private_obj_state(state, privobj, priv_state);

	return 0;
}

static int
drm_atomic_helper_sro_readout_commit_private_objs_state(struct drm_device *dev,
							struct drm_atomic_sro_state *state,
							struct drm_atomic_commit *commit)
{
	struct drm_private_state *new_obj_state;
	struct drm_private_obj *obj;
	unsigned int i;
	int ret;

	if (!commit)
		return -EINVAL;

	for_each_new_private_obj_in_state(commit, obj, new_obj_state, i) {
		ret = drm_atomic_helper_sro_readout_private_obj_state(dev, state, obj);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * drm_atomic_helper_sro_readout_private_objs_state - read out non-bridge private object states
 * @dev: DRM device
 * @state: SRO state container to fill
 * @commit: optional atomic commit limiting the objects to read out
 *
 * Iterates over the private objects that have a state in @commit when
 * it isn't NULL, or over all the private objects of @dev otherwise. For
 * each, allocates a state via atomic_create_state, and calls the
 * object's atomic_sro_readout_state hook to fill it from hardware.
 *
 * RETURNS:
 *
 * 0 on success, a negative error code otherwise.
 */
int
drm_atomic_helper_sro_readout_private_objs_state(struct drm_device *dev,
						 struct drm_atomic_sro_state *state,
						 struct drm_atomic_commit *commit)
{
	struct drm_private_obj *obj;
	int ret;

	might_alloc(GFP_KERNEL);

	if (commit)
		return drm_atomic_helper_sro_readout_commit_private_objs_state(dev, state, commit);

	drm_for_each_privobj(obj, dev) {
		ret = drm_atomic_helper_sro_readout_private_obj_state(dev, state, obj);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_sro_readout_private_objs_state);

/**
 * drm_atomic_helper_sro_build_state - build an SRO state from hardware
 * @dev: DRM device to build the state for
 * @commit: optional atomic commit limiting the objects to read out
 *
 * Allocates a &struct drm_atomic_sro_state and calls the readout hooks
 * for CRTCs, planes, connectors, bridges, and private objects in
 * sequence to fill it from the current hardware state.
 *
 * When @commit is NULL, the state of every object in the device is
 * read out, and the caller must hold all the modeset locks. When it
 * isn't NULL, only the objects that have a state in @commit are read
 * out, and the caller must hold the locks of the objects in @commit.
 *
 * This is the default implementation for
 * &drm_mode_config_helper_funcs.atomic_sro_build_state.
 *
 * RETURNS:
 *
 * A &struct drm_atomic_sro_state on success, an error pointer otherwise.
 */
struct drm_atomic_sro_state *
drm_atomic_helper_sro_build_state(struct drm_device *dev,
				  struct drm_atomic_commit *commit)
{
	struct drm_atomic_sro_state *state;
	struct drm_printer p = drm_dbg_printer(dev, DRM_UT_ATOMIC, NULL);
	int ret;

	might_alloc(GFP_KERNEL);

	drm_dbg_kms(dev, "Starting to build atomic state from hardware state.\n");

	state = drm_atomic_sro_state_alloc(dev);
	if (drm_WARN_ON(dev, IS_ERR(state)))
		return state;

	ret = drm_atomic_helper_sro_readout_crtcs_state(dev, state, commit);
	if (ret)
		goto err_state_free;

	ret = drm_atomic_helper_sro_readout_planes_state(dev, state, commit);
	if (ret)
		goto err_state_free;

	ret = drm_atomic_helper_sro_readout_connectors_state(dev, state,
							     commit);
	if (ret)
		goto err_state_free;

	ret = drm_atomic_helper_sro_readout_bridges_state(dev, state, commit);
	if (ret)
		goto err_state_free;

	ret = drm_atomic_helper_sro_readout_private_objs_state(dev, state,
							       commit);
	if (ret)
		goto err_state_free;

	drm_atomic_sro_state_print(state, &p);

	return state;

err_state_free:
	drm_atomic_sro_state_free(state);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_atomic_helper_sro_build_state);

/**
 * drm_atomic_helper_sro_readout_state - default &drm_mode_config_funcs.atomic_sro_readout_state implementation
 * @dev: DRM device to read out state for
 *
 * Checks if the device supports hardware state readout, builds the SRO
 * state via &drm_mode_config_helper_funcs.atomic_sro_build_state,
 * installs it as the current state of all DRM objects, and frees the
 * SRO container.
 *
 * The caller must hold all the modeset locks of @dev, as
 * drm_mode_config_create_initial_state(), the usual caller, does.
 *
 * RETURNS:
 *
 * 0 on success, -ENOTSUPP if the device does not support readout, a
 * negative error code otherwise.
 */
int drm_atomic_helper_sro_readout_state(struct drm_device *dev)
{
	const struct drm_mode_config_helper_funcs *funcs =
		dev->mode_config.helper_private;
	struct drm_atomic_sro_state *state;
	struct drm_colorop *colorop;

	if (!drm_atomic_sro_device_can_readout(dev)) {
		drm_info(dev, "Device doesn't support hardware state readout.");
		return -ENOTSUPP;
	}

	state = funcs->atomic_sro_build_state(dev, NULL);
	if (IS_ERR(state)) {
		drm_warn(dev, "Couldn't read out the hardware state: %pe\n",
			 state);
		return PTR_ERR(state);
	}

	/* TODO: Add colorop state readout support */
	drm_for_each_colorop(colorop, dev) {
		struct drm_colorop_state *colorop_state;

		/*
		 * The colorop states are already initialized when the
		 * colorops are created. Only create a pristine state for
		 * the ones that don't have one, like
		 * drm_mode_config_create_state() does.
		 */
		if (colorop->state)
			continue;

		colorop_state = drm_atomic_helper_colorop_create_state(colorop);
		if (IS_ERR(colorop_state)) {
			drm_atomic_sro_state_free(state);
			return PTR_ERR(colorop_state);
		}

		colorop->state = colorop_state;
	}

	drm_atomic_sro_install_state(state);
	drm_atomic_sro_state_free(state);

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_sro_readout_state);

static bool drm_atomic_helper_fb_compare(struct drm_printer *p,
					 struct drm_framebuffer *expected,
					 struct drm_framebuffer *actual)
{
	unsigned int i;
	bool ret = true;

	STATE_CHECK_FORMAT_INFO(ret, p, "framebuffer", expected, actual, format);

	for (i = 0; i < expected->format->num_planes; i++) {
		STATE_CHECK_U32(ret, p, "framebuffer", expected, actual, pitches[i]);
		STATE_CHECK_U32(ret, p, "framebuffer", expected, actual, offsets[i]);
	}

	STATE_CHECK_U64(ret, p, "framebuffer", expected, actual, modifier);
	STATE_CHECK_U32(ret, p, "framebuffer", expected, actual, width);
	STATE_CHECK_U32(ret, p, "framebuffer", expected, actual, height);
	STATE_CHECK_S32_X(ret, p, "framebuffer", expected, actual, flags);

	return ret;
}

/**
 * drm_atomic_helper_plane_compare_state - default &drm_plane_funcs.atomic_sro_compare_state hook
 * @plane: the plane being compared
 * @p: printer for reporting mismatches
 * @expected: the committed &struct drm_plane_state
 * @actual: the &struct drm_plane_state read out from hardware
 *
 * Compares the base &struct drm_plane_state fields of @actual to
 * @expected and reports any mismatches through @p. Drivers subclassing
 * the plane state should call this first, then compare their own fields.
 *
 * RETURNS:
 *
 * True if the states are identical, false otherwise.
 */
bool drm_atomic_helper_plane_compare_state(struct drm_plane *plane,
					   struct drm_printer *p,
					   struct drm_plane_state *expected,
					   struct drm_plane_state *actual)
{
	bool ret = true;

	STATE_CHECK_PTR(ret, p, plane->name, expected, actual, plane);
	STATE_CHECK_PTR(ret, p, plane->name, expected, actual, crtc);

	/* Both disabled, geometry is meaningless. */
	if (!expected->fb && !actual->fb)
		return ret;

	/* One enabled and the other disabled, that's a mismatch. */
	if (!expected->fb || !actual->fb) {
		drm_atomic_helper_print_state_mismatch(p,
						       plane->name,
						       "fb",
						       "expected framebuffer is %s, got %s",
						       expected->fb ? "non-NULL" : "NULL",
						       actual->fb ? "non-NULL" : "NULL");
		return false;
	}

	/* Both enabled, compare framebuffer and geometry. */
	if (!drm_atomic_helper_fb_compare(p, expected->fb, actual->fb))
		ret = false;

	STATE_CHECK_S32(ret, p, plane->name, expected, actual, crtc_x);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, crtc_y);
	STATE_CHECK_U32(ret, p, plane->name, expected, actual, crtc_w);
	STATE_CHECK_U32(ret, p, plane->name, expected, actual, crtc_h);
	STATE_CHECK_U32_16_16(ret, p, plane->name, expected, actual, src_x);
	STATE_CHECK_U32_16_16(ret, p, plane->name, expected, actual, src_y);
	STATE_CHECK_U32_16_16(ret, p, plane->name, expected, actual, src_w);
	STATE_CHECK_U32_16_16(ret, p, plane->name, expected, actual, src_h);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, hotspot_x);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, hotspot_y);
	STATE_CHECK_U16(ret, p, plane->name, expected, actual, alpha);
	STATE_CHECK_U16(ret, p, plane->name, expected, actual, pixel_blend_mode);
	STATE_CHECK_U32(ret, p, plane->name, expected, actual, rotation);
	STATE_CHECK_U32(ret, p, plane->name, expected, actual, normalized_zpos);
	STATE_CHECK_U32(ret, p, plane->name, expected, actual, color_encoding);
	STATE_CHECK_U32(ret, p, plane->name, expected, actual, color_range);

	/*
	 * NOTE: We can't check fb_damage_clips and ignore_damage_clips
	 * because they are transient, per-commit software fields that
	 * are reset when the state is duplicated and never stored in
	 * the hardware.
	 */

	STATE_CHECK_PTR(ret, p, plane->name, expected, actual, color_pipeline);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, src.x1);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, src.x2);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, src.y1);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, src.y2);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, dst.x1);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, dst.x2);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, dst.y1);
	STATE_CHECK_S32(ret, p, plane->name, expected, actual, dst.y2);
	STATE_CHECK_BOOL(ret, p, plane->name, expected, actual, visible);
	STATE_CHECK_U32(ret, p, plane->name, expected, actual, scaling_filter);

	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_plane_compare_state);

/**
 * drm_atomic_helper_crtc_compare_state - default &drm_crtc_funcs.atomic_sro_compare_state hook
 * @crtc: the CRTC being compared
 * @p: printer for reporting mismatches
 * @expected: the committed &struct drm_crtc_state
 * @actual: the &struct drm_crtc_state read out from hardware
 *
 * Compares the base &struct drm_crtc_state fields of @actual to
 * @expected and reports any mismatches through @p. Drivers subclassing
 * the CRTC state should call this first, then compare their own fields.
 *
 * RETURNS:
 *
 * True if the states are identical, false otherwise.
 */
bool drm_atomic_helper_crtc_compare_state(struct drm_crtc *crtc,
					  struct drm_printer *p,
					  struct drm_crtc_state *expected,
					  struct drm_crtc_state *actual)
{
	bool ret = true;

	STATE_CHECK_PTR(ret, p, crtc->name, expected, actual, crtc);
	STATE_CHECK_BOOL(ret, p, crtc->name, expected, actual, active);

	/* Both inactive, mode and properties are meaningless. */
	if (!expected->active && !actual->active)
		return ret;

	/* One active and the other inactive, that's a mismatch. */
	if (!expected->active || !actual->active)
		return false;

	/*
	 * Both active, compare everything. enable is only checked at
	 * this point: a DPMS off commit keeps enable set while the
	 * hardware, and thus the readout, reports an inactive CRTC.
	 */
	STATE_CHECK_BOOL(ret, p, crtc->name, expected, actual, enable);

	/* Both active, compare everything. */
	STATE_CHECK_BOOL(ret, p, crtc->name, expected, actual, no_vblank);
	STATE_CHECK_U32(ret, p, crtc->name, expected, actual, plane_mask);
	STATE_CHECK_U32(ret, p, crtc->name, expected, actual, connector_mask);
	STATE_CHECK_U32(ret, p, crtc->name, expected, actual, encoder_mask);

	STATE_CHECK_DISPLAY_MODE(ret, p, crtc->name, expected, actual, mode);
	STATE_CHECK_DISPLAY_MODE(ret, p, crtc->name, expected, actual, adjusted_mode);

	/*
	 * NOTE: We can't check mode_blob: its payload also carries the
	 * mode type and name, and neither can be reconstructed from the
	 * hardware. The modes themselves are covered by the checks
	 * above.
	 */
	STATE_CHECK_PROPERTY_BLOB(ret, p, crtc->name, expected, actual, degamma_lut);
	STATE_CHECK_PROPERTY_BLOB(ret, p, crtc->name, expected, actual, ctm);
	STATE_CHECK_PROPERTY_BLOB(ret, p, crtc->name, expected, actual, gamma_lut);
	STATE_CHECK_BOOL(ret, p, crtc->name, expected, actual, vrr_enabled);
	STATE_CHECK_BOOL(ret, p, crtc->name, expected, actual, self_refresh_active);
	STATE_CHECK_U32(ret, p, crtc->name, expected, actual, scaling_filter);
	STATE_CHECK_U64(ret, p, crtc->name, expected, actual, background_color);

	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_crtc_compare_state);

/**
 * drm_atomic_helper_connector_compare_state - default &drm_connector_funcs.atomic_sro_compare_state hook
 * @conn: the connector being compared
 * @p: printer for reporting mismatches
 * @expected: the committed &struct drm_connector_state
 * @actual: the &struct drm_connector_state read out from hardware
 *
 * Compares the base &struct drm_connector_state fields of @actual to
 * @expected and reports any mismatches through @p. Drivers subclassing
 * the connector state should call this first, then compare their own
 * fields.
 *
 * RETURNS:
 *
 * True if the states are identical, false otherwise.
 */
bool drm_atomic_helper_connector_compare_state(struct drm_connector *conn,
					       struct drm_printer *p,
					       struct drm_connector_state *expected,
					       struct drm_connector_state *actual)
{
	bool ret = true;

	STATE_CHECK_PTR(ret, p, conn->name, expected, actual, connector);

	/*
	 * Both unbound, the routing and the negotiated properties are
	 * meaningless: a disabled connector keeps the values it was
	 * last enabled with, while a state read out from a disabled
	 * pipeline is pristine.
	 */
	if (!expected->crtc && !actual->crtc)
		return ret;

	STATE_CHECK_PTR(ret, p, conn->name, expected, actual, crtc);
	STATE_CHECK_PTR(ret, p, conn->name, expected, actual, best_encoder);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, link_status);

	STATE_CHECK_U32(ret, p, conn->name, expected, actual, tv.select_subconnector);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, tv.subconnector);

	STATE_CHECK_BOOL(ret, p, conn->name, expected, actual, self_refresh_aware);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, picture_aspect_ratio);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, content_type);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, hdcp_content_type);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, scaling_mode);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, content_protection);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, colorspace);

	/*
	 * NOTE: We can't check max_bpc, max_requested_bpc,
	 * color_format or hdmi.broadcast_rgb because they are
	 * userspace requests negotiated during atomic_check into
	 * output_bpc, output_format and hdmi.is_limited_range. The
	 * hardware only stores the negotiated result.
	 */

	STATE_CHECK_U32(ret, p, conn->name, expected, actual, privacy_screen_sw_state);

	/*
	 * NOTE: We can't check hdr_output_metadata: it's a userspace
	 * request, and the hardware only stores the resulting HDR
	 * infoframe, compared below.
	 */

	/*
	 * NOTE: The infoframe payloads are only compared when the
	 * infoframe is enabled in both states: the disabled ones keep
	 * stale data from earlier commits around in the expected
	 * state, while the readout state is pristine.
	 */
	STATE_CHECK_BOOL(ret, p, conn->name, expected, actual, hdmi.infoframes.avi.set);
	if (expected->hdmi.infoframes.avi.set && actual->hdmi.infoframes.avi.set)
		STATE_CHECK_INFOFRAME(ret, p, conn->name, expected, actual, hdmi.infoframes.avi.data);
	STATE_CHECK_BOOL(ret, p, conn->name, expected, actual, hdmi.infoframes.hdr_drm.set);
	if (expected->hdmi.infoframes.hdr_drm.set && actual->hdmi.infoframes.hdr_drm.set)
		STATE_CHECK_INFOFRAME(ret, p, conn->name, expected, actual, hdmi.infoframes.hdr_drm.data);
	STATE_CHECK_BOOL(ret, p, conn->name, expected, actual, hdmi.infoframes.spd.set);
	if (expected->hdmi.infoframes.spd.set && actual->hdmi.infoframes.spd.set)
		STATE_CHECK_INFOFRAME(ret, p, conn->name, expected, actual, hdmi.infoframes.spd.data);
	STATE_CHECK_BOOL(ret, p, conn->name, expected, actual, hdmi.infoframes.hdmi.set);
	if (expected->hdmi.infoframes.hdmi.set && actual->hdmi.infoframes.hdmi.set)
		STATE_CHECK_INFOFRAME(ret, p, conn->name, expected, actual, hdmi.infoframes.hdmi.data);

	STATE_CHECK_BOOL(ret, p, conn->name, expected, actual, hdmi.is_limited_range);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, hdmi.output_bpc);
	STATE_CHECK_U32(ret, p, conn->name, expected, actual, hdmi.output_format);
	STATE_CHECK_U64(ret, p, conn->name, expected, actual, hdmi.tmds_char_rate);

	/*
	 * hdmi.scrambler_needed is derived from hdmi.tmds_char_rate, so
	 * comparing tmds_char_rate already covers it.
	 */

	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_connector_compare_state);

/**
 * drm_atomic_helper_bridge_compare_state - default &drm_bridge_funcs.atomic_sro_compare_state hook
 * @bridge: the bridge being compared
 * @p: printer for reporting mismatches
 * @expected: the committed &struct drm_bridge_state
 * @actual: the &struct drm_bridge_state read out from hardware
 *
 * Compares the base &struct drm_bridge_state fields of @actual to
 * @expected and reports any mismatches through @p. Drivers subclassing
 * the bridge state should call this first, then compare their own
 * fields.
 *
 * RETURNS:
 *
 * True if the states are identical, false otherwise.
 */
bool drm_atomic_helper_bridge_compare_state(struct drm_bridge *bridge,
					    struct drm_printer *p,
					    struct drm_bridge_state *expected,
					    struct drm_bridge_state *actual)
{
	bool ret = true;

	STATE_CHECK_PTR(ret, p, bridge->name, expected, actual, bridge);
	STATE_CHECK_U32_X(ret, p, bridge->name, expected, actual, input_bus_cfg.format);
	STATE_CHECK_U32_X(ret, p, bridge->name, expected, actual, input_bus_cfg.flags);
	STATE_CHECK_U32_X(ret, p, bridge->name, expected, actual, output_bus_cfg.format);
	STATE_CHECK_U32_X(ret, p, bridge->name, expected, actual, output_bus_cfg.flags);

	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_bridge_compare_state);

/**
 * drm_atomic_helper_print_state_mismatch - report a state comparison mismatch
 * @p: printer to report through
 * @name: human-readable name of the object (e.g. plane or CRTC name)
 * @field: name of the mismatching field
 * @format: printf-style format string describing the mismatch
 *
 * Helper used by atomic_sro_compare_state implementations and the
 * STATE_CHECK_* macros to report a field-level mismatch between the
 * committed state and the state read back from hardware.
 */
void __printf(4, 5)
drm_atomic_helper_print_state_mismatch(struct drm_printer *p,
				       const char *name,
				       const char *field,
				       const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	drm_printf(p, "%s configuration mismatch in %s %pV\n", name, field, &vaf);

	va_end(args);
}
EXPORT_SYMBOL(drm_atomic_helper_print_state_mismatch);
