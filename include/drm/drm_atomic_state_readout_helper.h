/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DRM_ATOMIC_SRO_HELPER_H_
#define DRM_ATOMIC_SRO_HELPER_H_

#include <linux/string_choices.h>

struct drm_atomic_commit;
struct drm_atomic_sro_state;
struct drm_device;
struct drm_printer;

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

void __printf(4, 5)
drm_atomic_helper_print_state_mismatch(struct drm_printer *p,
				       const char *name,
				       const char *field,
				       const char *format, ...);

#define STATE_CHECK_BOOL(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, bool),			\
			      __stringify(f) " is not a bool");		\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %s, got %s", \
							       str_yes_no(sa->f), \
							       str_yes_no(sb->f)); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_DISPLAY_MODE(r, p, n, sa, sb, f)			\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, struct drm_display_mode), \
			      __stringify(f) " is not a drm_display_mode structure"); \
		if (!drm_mode_equal(&sa->f, &sb->f)) {			\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected " DRM_MODE_FMT ", got " DRM_MODE_FMT, \
							       DRM_MODE_ARG(&sa->f), \
							       DRM_MODE_ARG(&sb->f)); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_INFOFRAME(r, p, n, sa, sb, f)			\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, union hdmi_infoframe), \
			      __stringify(f) " is not an hdmi_infoframe union"); \
		if (memcmp(&sa->f, &sb->f, sizeof(union hdmi_infoframe))) { \
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "infoframes don't match"); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_FORMAT_INFO(r, p, n, sa, sb, f)			\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, const struct drm_format_info *), \
			      __stringify(f) " is not a drm_format_info pointer"); \
		if (sa->f != sb->f) {			\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %p4cc, got %p4cc", \
							       sa->f ? &sa->f->format : NULL, \
							       sb->f ? &sb->f->format : NULL); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_PROPERTY_BLOB(r, p, n, sa, sb, f)			\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, struct drm_property_blob *), \
			      __stringify(f) " is not a drm_property_blob pointer"); \
		if (sa->f != sb->f &&					\
		    (!sa->f || !sb->f ||				\
		     sa->f->length != sb->f->length ||			\
		     memcmp(sa->f->data, sb->f->data, sa->f->length))) { \
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "blobs don't match"); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_PTR(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %p, got %p",	\
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_S32(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (s32)0),		\
			      __stringify(f) " is not an s32");		\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %d, got %d", \
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_S32_X(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (s32)0),		\
			      __stringify(f) " is not an s32");		\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %x, got %x", \
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_U16(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (u16)0),		\
			      __stringify(f) " is not a u16");		\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %u, got %u", \
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_U32(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (u32)0),		\
			      __stringify(f) " is not a u32");		\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %u, got %u", \
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_U32_16_16(r, p, n, sa, sb, f)			\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (u32)0),		\
			      __stringify(f) " is not a u32");		\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %d.%06u, got %d.%06u", \
							       sa->f >> 16, ((sa->f & 0xffff) * 15625) >> 10, \
							       sb->f >> 16, ((sb->f & 0xffff) * 15625) >> 10); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_U32_X(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (u32)0),		\
			      __stringify(f) " is not a u32");		\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %08x, got %08x", \
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_U64(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (u64)0),		\
			      __stringify(f) " is not a u64");		\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %llu, got %llu", \
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#endif /* DRM_ATOMIC_SRO_HELPER_H_ */
