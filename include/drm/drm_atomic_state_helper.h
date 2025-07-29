/*
 * Copyright (C) 2018 Intel Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 * Daniel Vetter <daniel.vetter@ffwll.ch>
 */

#include <linux/string_choices.h>
#include <linux/types.h>

struct drm_atomic_state;
struct drm_bridge;
struct drm_bridge_state;
struct drm_crtc;
struct drm_crtc_state;
struct drm_plane;
struct drm_plane_state;
struct drm_printer;
struct drm_connector;
struct drm_connector_state;
struct drm_printer;
struct drm_private_obj;
struct drm_private_state;
struct drm_modeset_acquire_ctx;
struct drm_device;

void __drm_atomic_helper_crtc_state_reset(struct drm_crtc_state *state,
					  struct drm_crtc *crtc);
void __drm_atomic_helper_crtc_reset(struct drm_crtc *crtc,
				    struct drm_crtc_state *state);
void drm_atomic_helper_crtc_reset(struct drm_crtc *crtc);
void __drm_atomic_helper_crtc_duplicate_state(struct drm_crtc *crtc,
					      struct drm_crtc_state *state);
struct drm_crtc_state *
drm_atomic_helper_crtc_duplicate_state(struct drm_crtc *crtc);
void __drm_atomic_helper_crtc_destroy_state(struct drm_crtc_state *state);
void drm_atomic_helper_crtc_destroy_state(struct drm_crtc *crtc,
					  struct drm_crtc_state *state);
bool drm_atomic_helper_crtc_compare_state(struct drm_crtc *crtc,
					  struct drm_printer *p,
					  struct drm_crtc_state *expected,
					  struct drm_crtc_state *actual);

void __drm_atomic_helper_plane_state_reset(struct drm_plane_state *state,
					   struct drm_plane *plane);
void __drm_atomic_helper_plane_reset(struct drm_plane *plane,
				     struct drm_plane_state *state);
void drm_atomic_helper_plane_reset(struct drm_plane *plane);
void __drm_atomic_helper_plane_duplicate_state(struct drm_plane *plane,
					       struct drm_plane_state *state);
struct drm_plane_state *
drm_atomic_helper_plane_duplicate_state(struct drm_plane *plane);
void __drm_atomic_helper_plane_destroy_state(struct drm_plane_state *state);
void drm_atomic_helper_plane_destroy_state(struct drm_plane *plane,
					  struct drm_plane_state *state);
bool drm_atomic_helper_plane_compare_state(struct drm_plane *plane,
					   struct drm_printer *p,
					   struct drm_plane_state *expected,
					   struct drm_plane_state *actual);

void __drm_atomic_helper_connector_state_reset(struct drm_connector_state *conn_state,
					       struct drm_connector *connector);
void __drm_atomic_helper_connector_reset(struct drm_connector *connector,
					 struct drm_connector_state *conn_state);
void drm_atomic_helper_connector_reset(struct drm_connector *connector);
void drm_atomic_helper_connector_tv_reset(struct drm_connector *connector);
int drm_atomic_helper_connector_tv_check(struct drm_connector *connector,
					 struct drm_atomic_state *state);
void drm_atomic_helper_connector_tv_margins_reset(struct drm_connector *connector);
void
__drm_atomic_helper_connector_duplicate_state(struct drm_connector *connector,
					   struct drm_connector_state *state);
bool drm_atomic_helper_connector_compare_state(struct drm_connector *connector,
					       struct drm_printer *p,
					       struct drm_connector_state *expected,
					       struct drm_connector_state *actual);

struct drm_connector_state *
drm_atomic_helper_connector_duplicate_state(struct drm_connector *connector);
void
__drm_atomic_helper_connector_destroy_state(struct drm_connector_state *state);
void drm_atomic_helper_connector_destroy_state(struct drm_connector *connector,
					  struct drm_connector_state *state);
void __drm_atomic_helper_private_obj_duplicate_state(struct drm_private_obj *obj,
						     struct drm_private_state *state);

void __drm_atomic_helper_bridge_duplicate_state(struct drm_bridge *bridge,
						struct drm_bridge_state *state);
struct drm_bridge_state *
drm_atomic_helper_bridge_duplicate_state(struct drm_bridge *bridge);
void drm_atomic_helper_bridge_destroy_state(struct drm_bridge *bridge,
					    struct drm_bridge_state *state);
void __drm_atomic_helper_bridge_reset(struct drm_bridge *bridge,
				      struct drm_bridge_state *state);
struct drm_bridge_state *
drm_atomic_helper_bridge_reset(struct drm_bridge *bridge);

bool drm_atomic_helper_bridge_compare_state(struct drm_bridge *bridge,
					    struct drm_printer *p,
					    struct drm_bridge_state *expected,
					    struct drm_bridge_state *actual);

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
			      __stringify(name) " is not a bool");	\
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
			      __stringify(name) " is not a drm_display_mode structure"); \
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
			      __stringify(name) " is not an hdmi_infoframe union"); \
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
			      __stringify(name) " is not a drm_format_info pointer"); \
		if (sa->f != sb->f) {			\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %p4cc, got %p4cc", \
							       &sa->f->format, \
							       &sb->f->format); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_PROPERTY_BLOB(r, p, n, sa, sb, f)			\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, struct drm_property_blob *), \
			      __stringify(name) " is not a drm_property_blob pointer"); \
		if (sa->f != sb->f &&					\
		    ((sa->f->length != sb->f->length) ||		\
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
							       "expected %px, got %px",	\
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_S32(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (s32)0),		\
			      __stringify(name) " is not an s32");	\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %u, got %u", \
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_S32_X(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (s32)0),		\
			      __stringify(name) " is not an s32");	\
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
			      __stringify(name) " is not a u16");	\
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
			      __stringify(name) " is not a u32");	\
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
			      __stringify(name) " is not a u32");	\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %d.%06u, got %d.%06u", \
							       sa->f >> 16, ((sa->f && 0xffff) * 15625) >> 10, \
							       sb->f >> 16, ((sb->f && 0xffff) * 15625) >> 10); \
			r = false;					\
		}							\
	} while (0)

#define STATE_CHECK_U32_X(r, p, n, sa, sb, f)				\
	do {								\
		static_assert(__same_type(sa->f, sb->f),		\
			      __stringify(f) " field types don't match"); \
		static_assert(__same_type(sa->f, (u32)0),		\
			      __stringify(name) " is not a u32");	\
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
			      __stringify(name) " is not a u64");	\
		if (sa->f != sb->f) {					\
			drm_atomic_helper_print_state_mismatch(p,	\
							       n,	\
							       __stringify(f), \
							       "expected %llu, got %llu", \
							       sa->f, sb->f); \
			r = false;					\
		}							\
	} while (0)
