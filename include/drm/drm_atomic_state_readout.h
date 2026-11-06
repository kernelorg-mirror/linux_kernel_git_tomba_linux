/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DRM_ATOMIC_SRO_H_
#define DRM_ATOMIC_SRO_H_

#include <linux/types.h>

struct drm_atomic_commit;
struct drm_atomic_sro_state;
struct drm_connector;
struct drm_connector_state;
struct drm_crtc;
struct drm_crtc_state;
struct drm_device;
struct drm_plane;
struct drm_plane_state;
struct drm_printer;
struct drm_private_obj;
struct drm_private_state;

enum drm_atomic_readout_status {
	DRM_ATOMIC_READOUT_DISABLED = 0,
	DRM_ATOMIC_READOUT_ENABLED,
	DRM_ATOMIC_READOUT_SKIP_MISSING_COMPARE,
	DRM_ATOMIC_READOUT_SKIP_MISSING_READOUT,
};

bool drm_atomic_sro_device_can_readout(struct drm_device *dev);

struct drm_atomic_sro_state *drm_atomic_sro_state_alloc(struct drm_device *dev);
void drm_atomic_sro_state_free(struct drm_atomic_sro_state *state);
void drm_atomic_sro_state_print(const struct drm_atomic_sro_state *state,
				struct drm_printer *p);

struct drm_device *
drm_atomic_sro_state_get_device(struct drm_atomic_sro_state *state);

struct drm_plane_state *
drm_atomic_sro_get_plane_state(struct drm_atomic_sro_state *state,
			       struct drm_plane *plane);
void drm_atomic_sro_set_plane_state(struct drm_atomic_sro_state *state,
				    struct drm_plane *plane,
				    struct drm_plane_state *plane_state);
struct drm_crtc_state *
drm_atomic_sro_get_crtc_state(struct drm_atomic_sro_state *state,
			      struct drm_crtc *crtc);
void drm_atomic_sro_set_crtc_state(struct drm_atomic_sro_state *state,
				   struct drm_crtc *crtc,
				   struct drm_crtc_state *crtc_state);
struct drm_connector_state *
drm_atomic_sro_get_connector_state(struct drm_atomic_sro_state *state,
				   struct drm_connector *connector);
void drm_atomic_sro_set_connector_state(struct drm_atomic_sro_state *state,
					struct drm_connector *conn,
					struct drm_connector_state *conn_state);
struct drm_private_state *
drm_atomic_sro_get_private_obj_state(struct drm_atomic_sro_state *state,
				     struct drm_private_obj *private_obj);
void drm_atomic_sro_set_private_obj_state(struct drm_atomic_sro_state *state,
					  struct drm_private_obj *obj,
					  struct drm_private_state *obj_state);

void drm_atomic_sro_install_state(struct drm_atomic_sro_state *state);
bool drm_atomic_sro_readout_and_compare(struct drm_atomic_commit *commit);

#endif /* DRM_ATOMIC_SRO_H_ */
