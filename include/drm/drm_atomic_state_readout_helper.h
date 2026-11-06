/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DRM_ATOMIC_SRO_HELPER_H_
#define DRM_ATOMIC_SRO_HELPER_H_

struct drm_atomic_commit;
struct drm_atomic_sro_state;
struct drm_device;

int
drm_atomic_helper_sro_readout_planes_state(struct drm_device *dev,
					   struct drm_atomic_sro_state *state,
					   struct drm_atomic_commit *commit);
int
drm_atomic_helper_sro_readout_crtcs_state(struct drm_device *dev,
					  struct drm_atomic_sro_state *state,
					  struct drm_atomic_commit *commit);
int
drm_atomic_helper_sro_readout_connectors_state(struct drm_device *dev,
					       struct drm_atomic_sro_state *state,
					       struct drm_atomic_commit *commit);
int
drm_atomic_helper_sro_readout_bridges_state(struct drm_device *dev,
					    struct drm_atomic_sro_state *state,
					    struct drm_atomic_commit *commit);
int
drm_atomic_helper_sro_readout_private_objs_state(struct drm_device *dev,
						 struct drm_atomic_sro_state *state,
						 struct drm_atomic_commit *commit);
struct drm_atomic_sro_state *
drm_atomic_helper_sro_build_state(struct drm_device *dev,
				  struct drm_atomic_commit *commit);
int drm_atomic_helper_sro_readout_state(struct drm_device *dev);

#endif /* DRM_ATOMIC_SRO_HELPER_H_ */
