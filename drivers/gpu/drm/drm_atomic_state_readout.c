// SPDX-License-Identifier: GPL-2.0-or-later

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_readout.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <linux/module.h>
#include <linux/sched/mm.h>

#include "drm_internal.h"
#include "drm_crtc_internal.h"

static unsigned int atomic_readout = DRM_ATOMIC_READOUT_ENABLED;
module_param_unsafe(atomic_readout, uint, 0400);
MODULE_PARM_DESC(atomic_readout,
		 "Enable Hardware State Readout (0 = disabled, 1 = enabled, 2 = ignore missing compares, 3 = ignore missing readouts and compares, default = 1)");

static bool drm_atomic_sro_can_readout(struct drm_device *dev)
{
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	struct drm_connector *connector;
	struct drm_private_obj *privobj;
	struct drm_connector_list_iter conn_iter;

	if (!dev->mode_config.helper_private ||
	    !dev->mode_config.helper_private->atomic_sro_build_state)
		return false;

	if (atomic_readout == DRM_ATOMIC_READOUT_SKIP_MISSING_READOUT)
		return true;

	if (!dev->mode_config.funcs->atomic_sro_readout_state)
		return false;

	drm_for_each_privobj(privobj, dev) {
		if (!privobj->funcs->atomic_sro_readout_state) {
			drm_dbg_atomic(dev,
				       "Private object %s missing readout callback",
				       privobj->name);
			return false;
		}
	}

	drm_for_each_plane(plane, dev) {
		if (!plane->funcs->atomic_sro_readout_state) {
			drm_dbg_atomic(dev, "Plane %s missing readout callback",
				       plane->name);
			return false;
		}
	}

	drm_for_each_crtc(crtc, dev) {
		if (!crtc->funcs->atomic_sro_readout_state) {
			drm_dbg_atomic(dev, "CRTC %s missing readout callback",
				       crtc->name);
			return false;
		}
	}

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		if (!connector->funcs->atomic_sro_readout_state) {
			drm_dbg_atomic(dev, "Connector %s missing readout callback",
				       connector->name);
			drm_connector_list_iter_end(&conn_iter);
			return false;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	return true;
}

/**
 * drm_atomic_sro_device_can_readout - check if a device supports hardware state readout
 * @dev: DRM device to check
 *
 * Verifies that the device is an atomic driver, that readout is
 * enabled, and that all KMS objects implement the relevant hooks.
 *
 * RETURNS:
 *
 * True if the device supports full hardware state readout, false
 * otherwise.
 */
bool drm_atomic_sro_device_can_readout(struct drm_device *dev)
{
	bool ret;

	if (!drm_drv_uses_atomic_modeset(dev))
		return false;

	if (atomic_readout == DRM_ATOMIC_READOUT_DISABLED)
		return false;

	ret = drm_atomic_sro_can_readout(dev);
	if (!ret)
		return false;

	return true;
}
EXPORT_SYMBOL(drm_atomic_sro_device_can_readout);

struct __drm_atomic_sro_plane {
	struct drm_plane *ptr;
	struct drm_plane_state *state;
};

struct __drm_atomic_sro_crtc {
	struct drm_crtc *ptr;
	struct drm_crtc_state *state;
};

struct __drm_atomic_sro_connector {
	struct drm_connector *ptr;
	struct drm_connector_state *state;
};

struct __drm_atomic_sro_private_obj {
	struct drm_private_obj *ptr;
	struct drm_private_state *state;
};

struct drm_atomic_sro_state {
	struct drm_device *dev;
	struct __drm_atomic_sro_plane *planes;
	struct __drm_atomic_sro_crtc *crtcs;
	struct __drm_atomic_sro_connector *connectors;
	unsigned int max_connector_index;
	struct __drm_atomic_sro_private_obj *private_objs;
	unsigned int num_private_objs;
};

static unsigned int max_connector_index(struct drm_device *dev)
{
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector;
	unsigned int max = 0;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter)
		max = max(max, connector->index);
	drm_connector_list_iter_end(&conn_iter);

	return max;
}

static unsigned int count_private_obj(struct drm_device *dev)
{
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_private_obj *obj;
	unsigned int count = 0;

	list_for_each_entry(obj, &config->privobj_list, head)
		count++;

	return count;
}

/**
 * drm_atomic_sro_state_get_device - get the DRM device from an SRO state
 * @state: SRO state
 *
 * RETURNS:
 *
 * The &struct drm_device associated with the SRO state.
 */
struct drm_device *
drm_atomic_sro_state_get_device(struct drm_atomic_sro_state *state)
{
	return state->dev;
}
EXPORT_SYMBOL(drm_atomic_sro_state_get_device);

static int drm_atomic_sro_state_init(struct drm_device *dev,
				     struct drm_atomic_sro_state *state)
{
	int err;

	state->planes =
		kzalloc_objs(*state->planes, dev->mode_config.num_total_plane);
	if (!state->planes)
		return -ENOMEM;

	state->crtcs = kzalloc_objs(*state->crtcs, dev->mode_config.num_crtc);
	if (!state->crtcs) {
		err = -ENOMEM;
		goto err_free_planes;
	}

	state->max_connector_index = max_connector_index(dev);
	state->connectors = kzalloc_objs(*state->connectors,
					 state->max_connector_index + 1);
	if (!state->connectors) {
		err = -ENOMEM;
		goto err_free_crtcs;
	}

	state->private_objs =
		kzalloc_objs(*state->private_objs, count_private_obj(dev));
	if (!state->private_objs)  {
		err = -ENOMEM;
		goto err_free_connectors;
	}

	state->num_private_objs = 0;

	drm_dev_get(dev);
	state->dev = dev;

	return 0;

err_free_connectors:
	kfree(state->connectors);
err_free_crtcs:
	kfree(state->crtcs);
err_free_planes:
	kfree(state->planes);
	return err;
}

/**
 * drm_atomic_sro_state_alloc - allocate an SRO state container
 * @dev: DRM device to allocate the state for
 *
 * Allocates and initializes a &struct drm_atomic_sro_state that can hold
 * readout states for all KMS objects on @dev.
 *
 * The returned state must be freed with drm_atomic_sro_state_free().
 *
 * RETURNS:
 *
 * A new &struct drm_atomic_sro_state on success, an error pointer on
 * failure.
 */
struct drm_atomic_sro_state *drm_atomic_sro_state_alloc(struct drm_device *dev)
{
	struct drm_atomic_sro_state *state;
	int ret;

	might_alloc(GFP_KERNEL);

	state = kzalloc_obj(*state);
	if (!state)
		return ERR_PTR(-ENOMEM);

	ret = drm_atomic_sro_state_init(dev, state);
	if (ret) {
		kfree(state);
		return ERR_PTR(ret);
	}

	return state;
}
EXPORT_SYMBOL(drm_atomic_sro_state_alloc);

/**
 * drm_atomic_sro_state_free - free an SRO state container
 * @state: SRO state to free
 *
 * Frees a &struct drm_atomic_sro_state previously allocated with
 * drm_atomic_sro_state_alloc(). Any states that have not been
 * installed via drm_atomic_sro_install_state() are also freed.
 */
void drm_atomic_sro_state_free(struct drm_atomic_sro_state *state)
{
	struct drm_device *dev = state->dev;
	unsigned int i;

	for (i = 0; i < state->num_private_objs; i++) {
		struct drm_private_obj *obj = state->private_objs[i].ptr;
		struct drm_private_state *obj_state = state->private_objs[i].state;

		if (!obj || !obj_state)
			continue;

		obj->funcs->atomic_destroy_state(obj, obj_state);
		state->private_objs[i].state = NULL;
		state->private_objs[i].ptr = NULL;
	}

	kfree(state->private_objs);

	for (i = 0; i <= state->max_connector_index; i++) {
		struct drm_connector *conn = state->connectors[i].ptr;
		struct drm_connector_state *conn_state =
			state->connectors[i].state;

		if (!conn)
			continue;

		if (conn_state)
			conn->funcs->atomic_destroy_state(conn, conn_state);

		state->connectors[i].state = NULL;
		state->connectors[i].ptr = NULL;
		drm_connector_put(conn);
	}

	kfree(state->connectors);

	for (i = 0; i < state->dev->mode_config.num_crtc; i++) {
		struct drm_crtc *crtc = state->crtcs[i].ptr;
		struct drm_crtc_state *crtc_state =
			state->crtcs[i].state;

		if (!crtc || !crtc_state)
			continue;

		crtc->funcs->atomic_destroy_state(crtc, crtc_state);
		state->crtcs[i].state = NULL;
		state->crtcs[i].ptr = NULL;
	}

	kfree(state->crtcs);

	for (i = 0; i < state->dev->mode_config.num_total_plane; i++) {
		struct drm_plane *plane = state->planes[i].ptr;
		struct drm_plane_state *plane_state =
			state->planes[i].state;

		if (!plane || !plane_state)
			continue;

		plane->funcs->atomic_destroy_state(plane, plane_state);
		state->planes[i].state = NULL;
		state->planes[i].ptr = NULL;
	}

	kfree(state->planes);
	kfree(state);

	drm_dev_put(dev);
}
EXPORT_SYMBOL(drm_atomic_sro_state_free);

/**
 * drm_atomic_sro_state_print - prints drm atomic SRO state
 * @state: SRO state to print
 * @p: drm printer
 *
 * This function prints the SRO state snapshot using the drm printer which is
 * passed to it. This snapshot can be used for debugging purposes.
 */
void drm_atomic_sro_state_print(const struct drm_atomic_sro_state *state,
				struct drm_printer *p)
{
	struct drm_mode_config *config = &state->dev->mode_config;
	int i;

	if (!p) {
		drm_err(state->dev, "invalid drm printer\n");
		return;
	}

	drm_dbg_atomic(state->dev, "Printing readout state %p\n", state);

	for (i = 0; i < config->num_total_plane; i++) {
		struct drm_plane_state *plane_state = state->planes[i].state;

		if (!plane_state)
			continue;

		drm_atomic_plane_print_state(p, plane_state);
	}

	for (i = 0; i < config->num_crtc; i++) {
		struct drm_crtc_state *crtc_state = state->crtcs[i].state;

		if (!crtc_state)
			continue;

		drm_atomic_crtc_print_state(p, crtc_state);
	}

	for (i = 0; i <= state->max_connector_index; i++) {
		struct drm_connector_state *conn_state = state->connectors[i].state;

		if (!conn_state)
			continue;

		drm_atomic_connector_print_state(p, conn_state);
	}

	for (i = 0; i < state->num_private_objs; i++) {
		struct drm_private_state *obj_state = state->private_objs[i].state;

		if (!obj_state)
			continue;

		drm_atomic_private_obj_print_state(p, obj_state);
	}
}
EXPORT_SYMBOL(drm_atomic_sro_state_print);

/**
 * drm_atomic_sro_get_plane_state - get the plane state from an SRO state
 * @state: SRO state container
 * @plane: plane to get the state for
 *
 * RETURNS:
 *
 * The &struct drm_plane_state for @plane, or NULL if not yet read out.
 */
struct drm_plane_state *
drm_atomic_sro_get_plane_state(struct drm_atomic_sro_state *state,
			       struct drm_plane *plane)
{
	unsigned int index = drm_plane_index(plane);

	if (index >= state->dev->mode_config.num_total_plane)
		return NULL;

	return state->planes[index].state;
};
EXPORT_SYMBOL(drm_atomic_sro_get_plane_state);

/**
 * drm_atomic_sro_set_plane_state - store a plane state into an SRO state
 * @state: SRO state container
 * @plane: plane the state belongs to
 * @plane_state: plane state to store
 */
void drm_atomic_sro_set_plane_state(struct drm_atomic_sro_state *state,
				    struct drm_plane *plane,
				    struct drm_plane_state *plane_state)
{
	unsigned int index = drm_plane_index(plane);

	state->planes[index].ptr = plane;
	state->planes[index].state = plane_state;

	drm_dbg_atomic(plane->dev,
		       "Added [PLANE:%d:%s] %p state to readout state %p\n",
		       plane->base.id, plane->name, plane_state, state);
}
EXPORT_SYMBOL(drm_atomic_sro_set_plane_state);

/**
 * drm_atomic_sro_get_crtc_state - get the CRTC state from an SRO state
 * @state: SRO state container
 * @crtc: CRTC to get the state for
 *
 * RETURNS:
 *
 * The &struct drm_crtc_state for @crtc, or NULL if not yet read out.
 */
struct drm_crtc_state *
drm_atomic_sro_get_crtc_state(struct drm_atomic_sro_state *state,
			      struct drm_crtc *crtc)
{
	unsigned int index = drm_crtc_index(crtc);

	if (index >= state->dev->mode_config.num_crtc)
		return NULL;

	return state->crtcs[index].state;
};
EXPORT_SYMBOL(drm_atomic_sro_get_crtc_state);

/**
 * drm_atomic_sro_set_crtc_state - store a CRTC state into an SRO state
 * @state: SRO state container
 * @crtc: CRTC the state belongs to
 * @crtc_state: CRTC state to store
 */
void drm_atomic_sro_set_crtc_state(struct drm_atomic_sro_state *state,
				   struct drm_crtc *crtc,
				   struct drm_crtc_state *crtc_state)
{
	unsigned int index = drm_crtc_index(crtc);

	state->crtcs[index].ptr = crtc;
	state->crtcs[index].state = crtc_state;

	drm_dbg_atomic(state->dev,
		       "Added [CRTC:%d:%s] %p state to readout state %p\n",
		       crtc->base.id, crtc->name, crtc_state, state);
}
EXPORT_SYMBOL(drm_atomic_sro_set_crtc_state);

/**
 * drm_atomic_sro_get_connector_state - get the connector state from an SRO state
 * @state: SRO state container
 * @connector: connector to get the state for
 *
 * RETURNS:
 *
 * The &struct drm_connector_state for @connector, or NULL if not yet
 * read out.
 */
struct drm_connector_state *
drm_atomic_sro_get_connector_state(struct drm_atomic_sro_state *state,
				   struct drm_connector *connector)
{
	unsigned int index = drm_connector_index(connector);

	/*
	 * The connectors array is sized at allocation time. A
	 * connector registered since then has no state in the
	 * container.
	 */
	if (index > state->max_connector_index)
		return NULL;

	return state->connectors[index].state;
};
EXPORT_SYMBOL(drm_atomic_sro_get_connector_state);

/**
 * drm_atomic_sro_set_connector_state - store a connector state into an SRO state
 * @state: SRO state container
 * @conn: connector the state belongs to
 * @conn_state: connector state to store
 *
 * Takes a reference on @conn which is released when the state is
 * installed or freed.
 */
void drm_atomic_sro_set_connector_state(struct drm_atomic_sro_state *state,
					struct drm_connector *conn,
					struct drm_connector_state *conn_state)
{
	unsigned int index = drm_connector_index(conn);

	drm_connector_get(conn);
	state->connectors[index].ptr = conn;
	state->connectors[index].state = conn_state;

	drm_dbg_atomic(conn->dev,
		       "Added [CONNECTOR:%d:%s] %p state to readout state %p\n",
		       conn->base.id, conn->name, conn_state, state);
}
EXPORT_SYMBOL(drm_atomic_sro_set_connector_state);

/**
 * drm_atomic_sro_get_private_obj_state - get a private object state from an SRO state
 * @state: SRO state container
 * @obj: private object to get the state for
 *
 * RETURNS:
 *
 * The &struct drm_private_state for @obj, or NULL if not yet read out.
 */
struct drm_private_state *
drm_atomic_sro_get_private_obj_state(struct drm_atomic_sro_state *state,
				     struct drm_private_obj *obj)
{
	unsigned int i;

	for (i = 0; i < state->num_private_objs; i++)
		if (obj == state->private_objs[i].ptr)
			return state->private_objs[i].state;

	return NULL;
}
EXPORT_SYMBOL(drm_atomic_sro_get_private_obj_state);

/**
 * drm_atomic_sro_set_private_obj_state - store a private object state into an SRO state
 * @state: SRO state container
 * @obj: private object the state belongs to
 * @obj_state: private object state to store
 */
void drm_atomic_sro_set_private_obj_state(struct drm_atomic_sro_state *state,
					  struct drm_private_obj *obj,
					  struct drm_private_state *obj_state)
{
	unsigned int index = state->num_private_objs;

	state->private_objs[index].ptr = obj;
	state->private_objs[index].state = obj_state;
	state->num_private_objs += 1;

	drm_dbg_atomic(state->dev,
		       "Added new private object %p state %p to readout state %p\n", obj,
		       obj_state, state);
}
EXPORT_SYMBOL(drm_atomic_sro_set_private_obj_state);

/**
 * drm_atomic_sro_install_state - install readout state into DRM objects
 * @state: SRO state to install
 *
 * Takes a &struct drm_atomic_sro_state built by
 * &drm_mode_config_helper_funcs.atomic_sro_build_state and installs its
 * contents as the current state of each DRM object (setting
 * &drm_crtc.state, &drm_plane.state, &drm_connector.state and
 * &drm_private_obj.state).
 *
 * For each object, the optional atomic_sro_install_state hook is
 * called before the state pointer is updated, allowing drivers to
 * perform any needed action.
 */
void drm_atomic_sro_install_state(struct drm_atomic_sro_state *state)
{
	unsigned int i;

	for (i = 0; i <= state->max_connector_index; i++) {
		const struct drm_connector_funcs *conn_funcs;
		struct drm_connector_state *conn_state;
		struct drm_connector *conn;

		conn = state->connectors[i].ptr;
		if (!conn)
			continue;

		conn_funcs = conn->funcs;
		if (!conn_funcs)
			continue;

		if (conn->state) {
			conn_funcs->atomic_destroy_state(conn, conn->state);
			conn->state = NULL;
		}

		conn_state = state->connectors[i].state;
		if (!conn_state)
			continue;

		if (conn_funcs->atomic_sro_install_state)
			conn_funcs->atomic_sro_install_state(conn, conn_state);

		conn->state = conn_state;
		state->connectors[i].state = NULL;
		state->connectors[i].ptr = NULL;
		drm_connector_put(conn);
	}

	for (i = 0; i < state->dev->mode_config.num_crtc; i++) {
		const struct drm_crtc_funcs *crtc_funcs;
		struct drm_crtc_state *crtc_state;
		struct drm_crtc *crtc;

		crtc = state->crtcs[i].ptr;
		if (!crtc)
			continue;

		crtc_funcs = crtc->funcs;
		if (!crtc_funcs)
			continue;

		if (crtc->state) {
			crtc_funcs->atomic_destroy_state(crtc, crtc->state);
			crtc->state = NULL;
		}

		crtc_state = state->crtcs[i].state;
		if (!crtc_state)
			continue;

		if (crtc_funcs->atomic_sro_install_state)
			crtc_funcs->atomic_sro_install_state(crtc, crtc_state);

		crtc->state = crtc_state;
		state->crtcs[i].state = NULL;
		state->crtcs[i].ptr = NULL;
	}

	for (i = 0; i < state->dev->mode_config.num_total_plane; i++) {
		const struct drm_plane_funcs *plane_funcs;
		struct drm_plane_state *plane_state;
		struct drm_plane *plane;

		plane = state->planes[i].ptr;
		if (!plane)
			continue;

		plane_funcs = plane->funcs;
		if (!plane_funcs)
			continue;

		if (plane->state) {
			plane_funcs->atomic_destroy_state(plane, plane->state);
			plane->state = NULL;
		}

		plane_state = state->planes[i].state;
		if (!plane_state)
			continue;

		if (plane_funcs->atomic_sro_install_state)
			plane_funcs->atomic_sro_install_state(plane, plane_state);

		plane->state = plane_state;
		state->planes[i].state = NULL;
		state->planes[i].ptr = NULL;
	}

	for (i = 0; i < count_private_obj(state->dev); i++) {
		const struct drm_private_state_funcs *obj_funcs;
		struct drm_private_state *obj_state;
		struct drm_private_obj *obj;

		obj = state->private_objs[i].ptr;
		if (!obj)
			continue;

		obj_funcs = obj->funcs;
		if (!obj_funcs)
			continue;

		if (obj->state) {
			obj_funcs->atomic_destroy_state(obj, obj->state);
			obj->state = NULL;
		}

		obj_state = state->private_objs[i].state;
		if (!obj_state)
			continue;

		if (obj_funcs->atomic_sro_install_state)
			obj_funcs->atomic_sro_install_state(obj, obj_state);

		obj->state = obj_state;
		state->private_objs[i].state = NULL;
		state->private_objs[i].ptr = NULL;
	}
	state->num_private_objs = 0;
}
EXPORT_SYMBOL(drm_atomic_sro_install_state);
