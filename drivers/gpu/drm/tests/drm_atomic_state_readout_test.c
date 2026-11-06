// SPDX-License-Identifier: GPL-2.0
/*
 * Kunit test for the DRM atomic state readout infrastructure
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_atomic_state_readout.h>
#include <drm/drm_atomic_state_readout_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>

#include <kunit/test.h>

#include <linux/media-bus-format.h>

/*
 * Those tests assume that the atomic_readout module parameter of
 * drm_atomic_state_readout is left to its default value (enabled).
 *
 * The drm_atomic_sro_readout_mode suite tests the other values the
 * parameter can take, and its init/exit hooks save and restore it
 * around every test case.
 */
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
extern unsigned int atomic_readout;

struct sro_test_priv {
	struct drm_device drm;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_private_obj private_obj;
	struct drm_bridge bridge;

	unsigned int connector_destroy_count;
	unsigned int private_obj_destroy_count;
	bool connector_readout_called;
	bool hw_reset_called;

	/*
	 * Whether the mock hardware drives a picture. The readout
	 * hooks report a disabled pipeline when it isn't set, like a
	 * driver reading a powered down encoder would.
	 */
	bool hw_active;

	/* KUnit context, for the hooks that can't get it as an argument */
	struct kunit *test;
};

/*
 * The tests run with the mock hardware state described by those
 * constants.
 */
#define SRO_TEST_CRTC_X		42
#define SRO_TEST_CRTC_Y		24
#define SRO_TEST_MODE_VIC	16	/* 1920x1080@60Hz */
#define SRO_TEST_OTHER_MODE_VIC	4	/* 640x480@60Hz */

static int mock_plane_sro_readout_state(struct drm_plane *plane,
					struct drm_atomic_sro_state *state,
					struct drm_plane_state *plane_state)
{
	plane_state->visible = true;
	plane_state->crtc_x = SRO_TEST_CRTC_X;
	plane_state->crtc_y = SRO_TEST_CRTC_Y;

	return 0;
}

static const struct drm_plane_funcs mock_plane_with_sro_funcs = {
	.atomic_create_state		= drm_atomic_helper_plane_create_state,
	.atomic_destroy_state		= drm_atomic_helper_plane_destroy_state,
	.atomic_duplicate_state		= drm_atomic_helper_plane_duplicate_state,
	.atomic_sro_readout_state	= mock_plane_sro_readout_state,
	.atomic_sro_compare_state	= drm_atomic_helper_plane_compare_state,
};

static int mock_crtc_sro_readout_state(struct drm_crtc *crtc,
				       struct drm_atomic_sro_state *state,
				       struct drm_crtc_state *crtc_state)
{
	struct sro_test_priv *priv =
		container_of(crtc->dev, struct sro_test_priv, drm);
	struct drm_display_mode *mode;
	int ret;

	/* The pipeline is off, there's nothing to read out. */
	if (!priv->hw_active)
		return 0;

	mode = drm_kunit_display_mode_from_cea_vic(priv->test, crtc->dev,
						   SRO_TEST_MODE_VIC);
	if (!mode)
		return -EINVAL;

	/*
	 * drm_atomic_set_mode_for_crtc() also sets the mode blob an
	 * enabled CRTC needs to pass the atomic checks.
	 */
	ret = drm_atomic_set_mode_for_crtc(crtc_state, mode);
	if (ret)
		return ret;

	crtc_state->active = true;

	/* The connector read out is routed to that CRTC */
	crtc_state->connector_mask = drm_connector_mask(&priv->connector);

	return 0;
}

static const struct drm_crtc_funcs mock_crtc_with_sro_funcs = {
	.atomic_create_state		= drm_atomic_helper_crtc_create_state,
	.atomic_destroy_state		= drm_atomic_helper_crtc_destroy_state,
	.atomic_duplicate_state		= drm_atomic_helper_crtc_duplicate_state,
	.atomic_sro_readout_state	= mock_crtc_sro_readout_state,
	.atomic_sro_compare_state	= drm_atomic_helper_crtc_compare_state,
};

/*
 * Same as mock_crtc_with_sro_funcs, but without the comparison hook:
 * used to exercise the modes that tolerate missing comparison
 * implementations.
 */
static const struct drm_crtc_funcs mock_crtc_with_sro_no_compare_funcs = {
	.atomic_create_state		= drm_atomic_helper_crtc_create_state,
	.atomic_destroy_state		= drm_atomic_helper_crtc_destroy_state,
	.atomic_duplicate_state		= drm_atomic_helper_crtc_duplicate_state,
	.atomic_sro_readout_state	= mock_crtc_sro_readout_state,
};

static int mock_connector_sro_readout_state(struct drm_connector *conn,
					    struct drm_atomic_sro_state *state,
					    struct drm_connector_state *conn_state)
{
	struct sro_test_priv *priv =
		container_of(conn, struct sro_test_priv, connector);
	struct drm_crtc *crtc;

	priv->connector_readout_called = true;

	/* The pipeline is off, there's nothing to read out. */
	if (!priv->hw_active)
		return 0;

	/*
	 * The CRTC states must be read out before the connectors so
	 * that the connector readout hooks can find them.
	 */
	drm_for_each_crtc(crtc, conn->dev) {
		if (!drm_atomic_sro_get_crtc_state(state, crtc))
			continue;

		/*
		 * drm_atomic_set_crtc_for_connector() takes a connector
		 * reference when setting the crtc pointer, which is
		 * later released by
		 * __drm_atomic_helper_connector_destroy_state(). The
		 * readout bypasses that path, so take the reference
		 * explicitly.
		 */
		drm_connector_get(conn);
		conn_state->crtc = crtc;
		conn_state->best_encoder = &priv->encoder;
	}

	if (!conn_state->crtc)
		return -EINVAL;

	return 0;
}

static void mock_connector_destroy_state(struct drm_connector *conn,
					 struct drm_connector_state *conn_state)
{
	struct sro_test_priv *priv =
		container_of(conn, struct sro_test_priv, connector);

	priv->connector_destroy_count++;

	drm_atomic_helper_connector_destroy_state(conn, conn_state);
}

static const struct drm_connector_funcs mock_connector_with_sro_funcs = {
	.atomic_create_state		= drm_atomic_helper_connector_create_state,
	.atomic_destroy_state		= mock_connector_destroy_state,
	.atomic_duplicate_state		= drm_atomic_helper_connector_duplicate_state,
	.atomic_sro_readout_state	= mock_connector_sro_readout_state,
	.atomic_sro_compare_state	= drm_atomic_helper_connector_compare_state,
};

static int mock_connector_sro_readout_err(struct drm_connector *conn,
					  struct drm_atomic_sro_state *state,
					  struct drm_connector_state *conn_state)
{
	return -EINVAL;
}

static const struct drm_connector_funcs mock_connector_with_sro_err_funcs = {
	.atomic_create_state		= drm_atomic_helper_connector_create_state,
	.atomic_destroy_state		= mock_connector_destroy_state,
	.atomic_duplicate_state		= drm_atomic_helper_connector_duplicate_state,
	.atomic_sro_readout_state	= mock_connector_sro_readout_err,
	.atomic_sro_compare_state	= drm_atomic_helper_connector_compare_state,
};

static const struct drm_connector_helper_funcs mock_connector_helper_funcs = {
};

/*
 * The private object state is subclassed so that the tests can check
 * that it went through the readout hook.
 */
struct mock_privobj_state {
	struct drm_private_state base;
	bool readout;
};

static inline struct mock_privobj_state *
to_mock_privobj_state(struct drm_private_state *state)
{
	return container_of(state, struct mock_privobj_state, base);
}

static struct drm_private_state *
mock_private_obj_create_state(struct drm_private_obj *obj)
{
	struct mock_privobj_state *privobj_state;

	privobj_state = kzalloc_obj(*privobj_state);
	if (!privobj_state)
		return ERR_PTR(-ENOMEM);

	__drm_atomic_helper_private_obj_create_state(obj,
						     &privobj_state->base);

	return &privobj_state->base;
}

static void mock_private_obj_destroy_state(struct drm_private_obj *obj,
					   struct drm_private_state *obj_state)
{
	struct sro_test_priv *priv =
		container_of(obj, struct sro_test_priv, private_obj);

	priv->private_obj_destroy_count++;

	kfree(obj_state);
}

static int mock_private_obj_sro_readout_state(struct drm_private_obj *obj,
					      struct drm_atomic_sro_state *state,
					      struct drm_private_state *obj_state)
{
	to_mock_privobj_state(obj_state)->readout = true;

	return 0;
}

static bool mock_private_obj_compare_state(struct drm_private_obj *obj,
					   struct drm_printer *p,
					   struct drm_private_state *expected,
					   struct drm_private_state *actual)
{
	return to_mock_privobj_state(expected)->readout ==
		to_mock_privobj_state(actual)->readout;
}

static const struct drm_private_state_funcs mock_private_obj_with_sro_funcs = {
	.atomic_create_state		= mock_private_obj_create_state,
	.atomic_destroy_state		= mock_private_obj_destroy_state,
	.atomic_sro_readout_state	= mock_private_obj_sro_readout_state,
	.atomic_sro_compare_state	= mock_private_obj_compare_state,
};

static const struct drm_bridge_funcs mock_bridge_funcs = {
	.atomic_create_state		= drm_atomic_helper_bridge_create_state,
	.atomic_destroy_state		= drm_atomic_helper_bridge_destroy_state,
};

static void mock_private_obj_fini(void *ptr)
{
	drm_atomic_private_obj_fini(ptr);
}

static void mock_hw_reset(struct drm_device *drm)
{
	struct sro_test_priv *priv =
		container_of(drm, struct sro_test_priv, drm);

	priv->hw_reset_called = true;
}

static int mock_modeset_commit(struct drm_device *drm,
			       struct drm_atomic_commit *commit,
			       bool nonblock)
{
	struct sro_test_priv *priv =
		container_of(drm, struct sro_test_priv, drm);

	KUNIT_FAIL(priv->test, "unexpected modeset");

	return 0;
}

/*
 * The atomic_commit hook is a tripwire: reading out the hardware
 * state and installing it is a state read, not a modeset, so nothing
 * must ever go through the atomic commit machinery. atomic_check is
 * only there so that a commit doesn't crash before reaching the
 * tripwire.
 */
static const struct drm_mode_config_funcs mock_mode_config_with_sro_funcs = {
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = mock_modeset_commit,
	.atomic_sro_readout_state = drm_atomic_helper_sro_readout_state,
	.hw_reset = mock_hw_reset,
};

static int mock_sro_readout_err(struct drm_device *dev)
{
	return -EINVAL;
}

static const struct drm_mode_config_funcs mock_mode_config_with_sro_err_funcs = {
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = mock_modeset_commit,
	.atomic_sro_readout_state = mock_sro_readout_err,
	.hw_reset = mock_hw_reset,
};

static int mock_device_readout(struct drm_device *drm)
{
	struct sro_test_priv *priv =
		container_of(drm, struct sro_test_priv, drm);
	struct drm_atomic_sro_state *sro_state;
	struct drm_crtc_state *crtc_state;

	sro_state = drm_atomic_sro_state_alloc(drm);
	if (IS_ERR(sro_state))
		return PTR_ERR(sro_state);

	crtc_state = priv->crtc->funcs->atomic_create_state(priv->crtc);
	if (IS_ERR(crtc_state)) {
		drm_atomic_sro_state_free(sro_state);
		return PTR_ERR(crtc_state);
	}

	crtc_state->enable = true;
	crtc_state->active = true;

	drm_atomic_sro_set_crtc_state(sro_state, priv->crtc, crtc_state);
	drm_atomic_sro_install_state(sro_state);
	drm_atomic_sro_state_free(sro_state);

	return 0;
}

static const struct drm_mode_config_funcs mock_mode_config_with_sro_hw_reset_funcs = {
	.atomic_sro_readout_state = mock_device_readout,
	.hw_reset = mock_hw_reset,
};

static const struct drm_mode_config_funcs mock_mode_config_without_sro_with_hw_reset_funcs = {
	.hw_reset = mock_hw_reset,
};

static const struct drm_mode_config_helper_funcs mock_mode_config_helper_without_sro_funcs = {
};

static const struct drm_mode_config_helper_funcs mock_mode_config_helper_with_sro_funcs = {
	.atomic_sro_build_state = drm_atomic_helper_sro_build_state,
};

static struct drm_atomic_sro_state *
mock_build_state_error(struct drm_device *dev,
		       struct drm_atomic_commit *commit)
{
	return ERR_PTR(-EINVAL);
}

static const struct drm_mode_config_helper_funcs mock_mode_config_helper_with_err_funcs = {
	.atomic_sro_build_state = mock_build_state_error,
};

/*
 * Passing NULL for any of the object funcs will make the KUnit
 * helpers use their defaults, which don't implement the state
 * readout hooks.
 */
static struct sro_test_priv *
mock_device_create(struct kunit *test,
		   const struct drm_mode_config_funcs *mode_config_funcs,
		   const struct drm_mode_config_helper_funcs *helper_funcs,
		   const struct drm_plane_funcs *plane_funcs,
		   const struct drm_crtc_funcs *crtc_funcs,
		   const struct drm_connector_funcs *connector_funcs)
{
	struct sro_test_priv *priv;
	struct drm_device *drm;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	priv = drm_kunit_helper_alloc_drm_device(test, dev,
						 struct sro_test_priv, drm,
						 DRIVER_MODESET |
						 DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	drm = &priv->drm;

	priv->test = test;
	priv->hw_active = true;

	drm->mode_config.funcs = mode_config_funcs;
	drm->mode_config.helper_private = helper_funcs;

	priv->plane = drm_kunit_helper_create_primary_plane(test, drm,
							    plane_funcs,
							    NULL,
							    NULL, 0, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->plane);

	priv->crtc = drm_kunit_helper_create_crtc(test, drm,
						  priv->plane, NULL,
						  crtc_funcs,
						  NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->crtc);

	ret = drmm_encoder_init(drm, &priv->encoder, NULL,
				DRM_MODE_ENCODER_NONE, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	priv->encoder.possible_crtcs = drm_crtc_mask(priv->crtc);

	ret = drmm_connector_init(drm, &priv->connector,
				  connector_funcs,
				  DRM_MODE_CONNECTOR_VIRTUAL, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_connector_helper_add(&priv->connector,
				 &mock_connector_helper_funcs);

	drm_connector_attach_encoder(&priv->connector, &priv->encoder);

	drm_atomic_private_obj_init(drm, &priv->private_obj,
				    "test private obj",
				    &mock_private_obj_with_sro_funcs);
	kunit_add_action_or_reset(test, mock_private_obj_fini,
				  &priv->private_obj);

	return priv;
}

static struct sro_test_priv *
mock_device_with_sro_create(struct kunit *test)
{
	return mock_device_create(test,
				  &mock_mode_config_with_sro_funcs,
				  &mock_mode_config_helper_with_sro_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_funcs,
				  &mock_connector_with_sro_funcs);
}

/*
 * State readout container tests
 */

/*
 * Test that a state container can be allocated and freed for a device,
 * and that it reports the device it was created for.
 */
static void drm_test_sro_state_alloc_free(struct kunit *test)
{
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_atomic_sro_state *sro_state;

	sro_state = drm_atomic_sro_state_alloc(&priv->drm);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sro_state);

	KUNIT_EXPECT_PTR_EQ(test, drm_atomic_sro_state_get_device(sro_state),
			    &priv->drm);

	drm_atomic_sro_state_free(sro_state);
}

/*
 * Test that objects with no state stored in the container report no
 * state.
 */
static void drm_test_sro_state_get_unset(struct kunit *test)
{
	struct drm_connector *connector;
	struct drm_private_obj *obj;
	struct sro_test_priv *priv;
	struct drm_atomic_sro_state *sro_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	struct drm_private_state *obj_state;

	priv = mock_device_with_sro_create(test);
	connector = &priv->connector;
	obj = &priv->private_obj;

	sro_state = drm_atomic_sro_state_alloc(&priv->drm);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sro_state);

	/* No state has been stored yet */
	plane_state = drm_atomic_sro_get_plane_state(sro_state, priv->plane);
	KUNIT_EXPECT_NULL(test, plane_state);

	crtc_state = drm_atomic_sro_get_crtc_state(sro_state, priv->crtc);
	KUNIT_EXPECT_NULL(test, crtc_state);

	conn_state = drm_atomic_sro_get_connector_state(sro_state, connector);
	KUNIT_EXPECT_NULL(test, conn_state);

	obj_state = drm_atomic_sro_get_private_obj_state(sro_state, obj);
	KUNIT_EXPECT_NULL(test, obj_state);

	drm_atomic_sro_state_free(sro_state);
}

/*
 * Test that states stored in the container can be retrieved from it,
 * and that the container destroys the states that haven't been
 * installed when it is freed.
 */
static void drm_test_sro_state_set_get(struct kunit *test)
{
	struct drm_connector *connector;
	struct drm_private_obj *obj;
	struct sro_test_priv *priv;
	struct drm_atomic_sro_state *sro_state;
	struct drm_plane_state *plane_state, *plane_readback;
	struct drm_crtc_state *crtc_state, *crtc_readback;
	struct drm_connector_state *conn_state, *conn_readback;
	struct drm_private_state *obj_state, *obj_readback;

	priv = mock_device_with_sro_create(test);
	connector = &priv->connector;
	obj = &priv->private_obj;

	sro_state = drm_atomic_sro_state_alloc(&priv->drm);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sro_state);

	plane_state = priv->plane->funcs->atomic_create_state(priv->plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, plane_state);

	crtc_state = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	conn_state = connector->funcs->atomic_create_state(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	obj_state = mock_private_obj_create_state(obj);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, obj_state);

	drm_atomic_sro_set_plane_state(sro_state, priv->plane, plane_state);
	drm_atomic_sro_set_crtc_state(sro_state, priv->crtc, crtc_state);
	drm_atomic_sro_set_connector_state(sro_state, connector, conn_state);
	drm_atomic_sro_set_private_obj_state(sro_state, obj, obj_state);

	plane_readback = drm_atomic_sro_get_plane_state(sro_state, priv->plane);
	KUNIT_EXPECT_PTR_EQ(test, plane_readback, plane_state);

	crtc_readback = drm_atomic_sro_get_crtc_state(sro_state, priv->crtc);
	KUNIT_EXPECT_PTR_EQ(test, crtc_readback, crtc_state);

	conn_readback = drm_atomic_sro_get_connector_state(sro_state,
							   connector);
	KUNIT_EXPECT_PTR_EQ(test, conn_readback, conn_state);

	obj_readback = drm_atomic_sro_get_private_obj_state(sro_state, obj);
	KUNIT_EXPECT_PTR_EQ(test, obj_readback, obj_state);

	drm_atomic_sro_state_free(sro_state);

	/* The uninstalled states have been destroyed by the container */
	KUNIT_EXPECT_EQ(test, priv->connector_destroy_count, 1);
	KUNIT_EXPECT_EQ(test, priv->private_obj_destroy_count, 1);
}

/*
 * Test that drm_atomic_sro_install_state() makes the read out states
 * the current states of the objects and transfers their ownership:
 * freeing the container afterwards doesn't destroy them.
 */
static void drm_test_sro_state_install(struct kunit *test)
{
	struct drm_connector *connector;
	struct drm_private_obj *obj;
	struct sro_test_priv *priv;
	struct drm_atomic_sro_state *sro_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	struct drm_private_state *obj_state;

	priv = mock_device_with_sro_create(test);
	connector = &priv->connector;
	obj = &priv->private_obj;

	sro_state = drm_atomic_sro_state_alloc(&priv->drm);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sro_state);

	plane_state = priv->plane->funcs->atomic_create_state(priv->plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, plane_state);

	crtc_state = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);
	crtc_state->enable = true;
	crtc_state->active = true;

	conn_state = connector->funcs->atomic_create_state(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	obj_state = mock_private_obj_create_state(obj);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, obj_state);

	drm_atomic_sro_set_plane_state(sro_state, priv->plane, plane_state);
	drm_atomic_sro_set_crtc_state(sro_state, priv->crtc, crtc_state);
	drm_atomic_sro_set_connector_state(sro_state, connector, conn_state);
	drm_atomic_sro_set_private_obj_state(sro_state, obj, obj_state);

	drm_atomic_sro_install_state(sro_state);

	/* The read out states are now owned by the objects */
	KUNIT_EXPECT_PTR_EQ(test, priv->plane->state, plane_state);
	KUNIT_EXPECT_PTR_EQ(test, priv->crtc->state, crtc_state);
	KUNIT_EXPECT_PTR_EQ(test, connector->state, conn_state);
	KUNIT_EXPECT_PTR_EQ(test, obj->state, obj_state);
	KUNIT_EXPECT_TRUE(test, priv->crtc->state->active);

	/*
	 * Freeing the container after the installation must not destroy
	 * the installed states.
	 */
	drm_atomic_sro_state_free(sro_state);

	KUNIT_EXPECT_PTR_EQ(test, priv->plane->state, plane_state);
	KUNIT_EXPECT_PTR_EQ(test, priv->crtc->state, crtc_state);
	KUNIT_EXPECT_PTR_EQ(test, connector->state, conn_state);
	KUNIT_EXPECT_PTR_EQ(test, obj->state, obj_state);

	/*
	 * The connector had no state before the installation, while the
	 * state the private object was initialized with by
	 * drm_atomic_private_obj_init() has been destroyed when
	 * replaced by the read out one.
	 */
	KUNIT_EXPECT_EQ(test, priv->connector_destroy_count, 0);
	KUNIT_EXPECT_EQ(test, priv->private_obj_destroy_count, 1);
}

/*
 * Test that installing a state on an object that already has one
 * destroys the state that was installed before.
 */
static void drm_test_sro_state_install_replaces_state(struct kunit *test)
{
	struct drm_connector *connector;
	struct sro_test_priv *priv;
	struct drm_atomic_sro_state *sro_state;
	struct drm_connector_state *old_state, *new_state;

	priv = mock_device_with_sro_create(test);
	connector = &priv->connector;

	old_state = connector->funcs->atomic_create_state(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_state);
	connector->state = old_state;

	sro_state = drm_atomic_sro_state_alloc(&priv->drm);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sro_state);

	new_state = connector->funcs->atomic_create_state(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_state);
	drm_atomic_sro_set_connector_state(sro_state, connector, new_state);

	drm_atomic_sro_install_state(sro_state);
	drm_atomic_sro_state_free(sro_state);

	KUNIT_EXPECT_PTR_EQ(test, connector->state, new_state);

	/* The state that was installed before has been destroyed */
	KUNIT_EXPECT_EQ(test, priv->connector_destroy_count, 1);
}

/*
 * Test that objects with no state in the container are left untouched
 * by the installation.
 */
static void drm_test_sro_state_install_partial(struct kunit *test)
{
	struct drm_private_state *initial_obj_state;
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_atomic_sro_state *sro_state;
	struct drm_crtc_state *crtc_state;

	/* The private object is initialized with a state of its own */
	initial_obj_state = priv->private_obj.state;
	KUNIT_ASSERT_NOT_NULL(test, initial_obj_state);

	sro_state = drm_atomic_sro_state_alloc(&priv->drm);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sro_state);

	crtc_state = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	drm_atomic_sro_set_crtc_state(sro_state, priv->crtc, crtc_state);

	drm_atomic_sro_install_state(sro_state);
	drm_atomic_sro_state_free(sro_state);

	KUNIT_EXPECT_PTR_EQ(test, priv->crtc->state, crtc_state);
	KUNIT_EXPECT_NULL(test, priv->plane->state);
	KUNIT_EXPECT_NULL(test, priv->connector.state);

	/* The private object has been left untouched */
	KUNIT_EXPECT_PTR_EQ(test, priv->private_obj.state, initial_obj_state);
	KUNIT_EXPECT_EQ(test, priv->private_obj_destroy_count, 0);
}

static struct kunit_case drm_atomic_sro_state_tests[] = {
	KUNIT_CASE(drm_test_sro_state_alloc_free),
	KUNIT_CASE(drm_test_sro_state_get_unset),
	KUNIT_CASE(drm_test_sro_state_set_get),
	KUNIT_CASE(drm_test_sro_state_install),
	KUNIT_CASE(drm_test_sro_state_install_replaces_state),
	KUNIT_CASE(drm_test_sro_state_install_partial),
	{ }
};

static struct kunit_suite drm_atomic_sro_state_test_suite = {
	.name = "drm_atomic_sro_state",
	.test_cases = drm_atomic_sro_state_tests,
};

/*
 * Readout tests
 */

/*
 * Test that a successful readout calls the readout hook of every
 * object, including the private objects, and installs the states it
 * built as the current states of the objects. The connectors are read
 * out after the CRTCs so that their hooks can look the CRTC states up.
 */
static void drm_test_sro_readout_state(struct kunit *test)
{
	struct mock_privobj_state *privobj_state;
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	int ret;

	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* The states read out have been installed on the objects */
	KUNIT_EXPECT_TRUE(test, priv->crtc->state->enable);
	KUNIT_EXPECT_TRUE(test, priv->crtc->state->active);
	KUNIT_EXPECT_TRUE(test, priv->plane->state->visible);
	KUNIT_EXPECT_EQ(test, priv->plane->state->crtc_x, SRO_TEST_CRTC_X);
	KUNIT_EXPECT_EQ(test, priv->plane->state->crtc_y, SRO_TEST_CRTC_Y);
	KUNIT_EXPECT_PTR_EQ(test, priv->connector.state->crtc, priv->crtc);

	KUNIT_ASSERT_NOT_NULL(test, priv->private_obj.state);
	privobj_state = to_mock_privobj_state(priv->private_obj.state);
	KUNIT_EXPECT_TRUE(test, privobj_state->readout);
}

/*
 * Test that a device without an atomic_sro_build_state helper can't be
 * read out, and that nothing is installed.
 */
static void drm_test_sro_readout_state_no_helper(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	/*
	 * Everything is in place for the readout except the
	 * atomic_sro_build_state helper.
	 */
	priv = mock_device_create(test,
				  &mock_mode_config_with_sro_funcs,
				  &mock_mode_config_helper_without_sro_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_funcs,
				  &mock_connector_with_sro_funcs);

	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, -ENOTSUPP);

	/* Nothing has been installed */
	KUNIT_EXPECT_NULL(test, priv->crtc->state);
	KUNIT_EXPECT_NULL(test, priv->plane->state);
}

/*
 * Test that a device whose mode configuration doesn't implement the
 * atomic_sro_readout_state hook can't be read out either.
 */
static void drm_test_sro_readout_state_no_hook(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	/*
	 * Everything is in place for the readout except the
	 * atomic_sro_readout_state mode configuration hook.
	 */
	priv = mock_device_create(test,
				  &mock_mode_config_without_sro_with_hw_reset_funcs,
				  &mock_mode_config_helper_with_sro_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_funcs,
				  &mock_connector_with_sro_funcs);

	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, -ENOTSUPP);

	/* Nothing has been installed */
	KUNIT_EXPECT_NULL(test, priv->crtc->state);
	KUNIT_EXPECT_NULL(test, priv->plane->state);
}

/*
 * Test that a device where an object doesn't implement the readout
 * hooks can't be read out either.
 */
static void drm_test_sro_readout_state_missing_hooks(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	/*
	 * The default KUnit helpers object implementations don't have
	 * any readout hook.
	 */
	priv = mock_device_create(test,
				  &mock_mode_config_with_sro_funcs,
				  &mock_mode_config_helper_with_sro_funcs,
				  NULL, NULL,
				  &mock_connector_with_sro_funcs);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, -ENOTSUPP);

	KUNIT_EXPECT_NULL(test, priv->crtc->state);
	KUNIT_EXPECT_NULL(test, priv->plane->state);
}

/*
 * Test that an error from atomic_sro_build_state() is reported to the
 * caller as-is, and that no state has been installed.
 */
static void drm_test_sro_readout_state_error(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	priv = mock_device_create(test,
				  &mock_mode_config_with_sro_funcs,
				  &mock_mode_config_helper_with_err_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_funcs,
				  &mock_connector_with_sro_funcs);

	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	/* Nothing has been installed */
	KUNIT_EXPECT_NULL(test, priv->crtc->state);
	KUNIT_EXPECT_NULL(test, priv->plane->state);
}

/*
 * Test that drm_atomic_sro_readout_and_compare() builds a fresh state
 * from the hardware and compares it to the commit: a commit state
 * duplicated from the state read out of the hardware is identical,
 * while a modified one is reported as a mismatch.
 */
static void drm_test_sro_readout_and_compare(struct kunit *test)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *commit;
	struct sro_test_priv *priv;
	struct drm_crtc_state *crtc_state;
	bool identical;
	int ret;

	priv = mock_device_with_sro_create(test);

	/* Install the mock hardware state */
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_modeset_acquire_init(&ctx, 0);

	commit = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, commit);

	crtc_state = drm_atomic_get_crtc_state(commit, priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	/*
	 * The commit state has been duplicated from the state read out
	 * of the mock hardware, so the two are identical.
	 */
	identical = drm_atomic_sro_readout_and_compare(commit);
	KUNIT_EXPECT_TRUE(test, identical);

	crtc_state->active = false;
	identical = drm_atomic_sro_readout_and_compare(commit);
	KUNIT_EXPECT_FALSE(test, identical);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that the readout and compare of a commit is scoped to the
 * objects of the commit: the objects that the commit doesn't contain
 * are neither read out nor compared.
 */
static void drm_test_sro_readout_and_compare_scope(struct kunit *test)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *commit;
	struct sro_test_priv *priv;
	struct drm_crtc_state *crtc_state;
	bool identical;
	int ret;

	priv = mock_device_with_sro_create(test);

	/* Install the mock hardware state */
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * The connector readout hook sets connector_readout_called
	 * when it runs. Reset it so that we can check whether it runs
	 * again.
	 */
	priv->connector_readout_called = false;

	drm_modeset_acquire_init(&ctx, 0);

	commit = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, commit);

	/*
	 * The commit only contains a state for the CRTC, duplicated
	 * from the state read out of the mock hardware, so the two
	 * are identical.
	 */
	crtc_state = drm_atomic_get_crtc_state(commit, priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	identical = drm_atomic_sro_readout_and_compare(commit);
	KUNIT_EXPECT_TRUE(test, identical);

	/*
	 * The connector isn't part of the commit, so its readout hook
	 * must not have run.
	 */
	KUNIT_EXPECT_FALSE(test, priv->connector_readout_called);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that the state installed by the readout lets the first commit
 * tell whether a modeset is needed: a commit asking for the
 * configuration already programmed in the hardware doesn't need one,
 * and a commit changing the configuration does.
 */
static void drm_test_sro_readout_no_modeset(struct kunit *test)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *commit;
	struct sro_test_priv *priv;
	struct drm_crtc_state *crtc_state;
	struct drm_display_mode *mode;
	int ret;

	priv = mock_device_with_sro_create(test);

	ret = drm_mode_config_create_initial_state(&priv->drm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * A commit with the same configuration as the hardware
	 * doesn't need a modeset.
	 */
	drm_modeset_acquire_init(&ctx, 0);

	commit = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, commit);

	crtc_state = drm_atomic_get_crtc_state(commit, priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	ret = drm_atomic_check_only(commit);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, crtc_state->mode_changed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	/* A commit with a different configuration needs a modeset. */
	drm_modeset_acquire_init(&ctx, 0);

retry:
	commit = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, commit);

	crtc_state = drm_atomic_get_crtc_state(commit, priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	mode = drm_kunit_display_mode_from_cea_vic(test, &priv->drm,
						   SRO_TEST_OTHER_MODE_VIC);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	ret = drm_atomic_set_mode_for_crtc(crtc_state, mode);
	KUNIT_ASSERT_EQ(test, ret, 0);

	commit->allow_modeset = true;

	ret = drm_atomic_check_only(commit);
	if (ret == -EDEADLK)
		goto backoff;
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, crtc_state->mode_changed);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return;

backoff:
	drm_atomic_commit_clear(commit);
	drm_modeset_backoff(&ctx);
	goto retry;
}

/*
 * Test that blanking a pipeline isn't reported as a mismatch. A DPMS
 * off commit only clears drm_crtc_state.active and leaves the
 * connector bound to its CRTC, while the hardware readout of a
 * disabled pipeline reports an unbound connector: the two states
 * differ, but there is nothing to compare for a pipeline that doesn't
 * drive a picture anymore.
 */
static void drm_test_sro_readout_and_compare_blanked(struct kunit *test)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector_state *conn_state;
	struct drm_plane_state *plane_state;
	struct drm_atomic_commit *commit;
	struct sro_test_priv *priv;
	struct drm_crtc_state *crtc_state;
	bool identical;
	int ret;

	priv = mock_device_with_sro_create(test);

	/* Install the mock hardware state, with the pipeline enabled */
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_modeset_acquire_init(&ctx, 0);

	commit = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, commit);

	crtc_state = drm_atomic_get_crtc_state(commit, priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	conn_state = drm_atomic_get_connector_state(commit, &priv->connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	plane_state = drm_atomic_get_plane_state(commit, priv->plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, plane_state);

	ret = drm_atomic_set_crtc_for_plane(plane_state, priv->crtc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * A DPMS off commit only clears active: the connector and the
	 * plane are still bound to their CRTC in the committed state.
	 */
	crtc_state->active = false;
	KUNIT_ASSERT_PTR_EQ(test, conn_state->crtc, priv->crtc);

	/* The hardware is now off */
	priv->hw_active = false;

	identical = drm_atomic_sro_readout_and_compare(commit);
	KUNIT_EXPECT_TRUE(test, identical);

	/*
	 * A pipeline that should be driving a picture but isn't is
	 * still reported as a mismatch.
	 */
	crtc_state->active = true;

	identical = drm_atomic_sro_readout_and_compare(commit);
	KUNIT_EXPECT_FALSE(test, identical);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static struct kunit_case drm_atomic_sro_readout_tests[] = {
	KUNIT_CASE(drm_test_sro_readout_state),
	KUNIT_CASE(drm_test_sro_readout_state_no_helper),
	KUNIT_CASE(drm_test_sro_readout_state_no_hook),
	KUNIT_CASE(drm_test_sro_readout_state_missing_hooks),
	KUNIT_CASE(drm_test_sro_readout_state_error),
	KUNIT_CASE(drm_test_sro_readout_and_compare),
	KUNIT_CASE(drm_test_sro_readout_and_compare_scope),
	KUNIT_CASE(drm_test_sro_readout_and_compare_blanked),
	KUNIT_CASE(drm_test_sro_readout_no_modeset),
	{ }
};

static struct kunit_suite drm_atomic_sro_readout_test_suite = {
	.name = "drm_atomic_sro_readout",
	.test_cases = drm_atomic_sro_readout_tests,
};

/*
 * Readout module parameter tests
 */

/*
 * The drm.atomic_readout module parameter is global: save the mode
 * the kernel was booted with before every test case, and restore it
 * afterwards. The exit hook runs even when an assertion aborts the
 * test, and KUnit executes the test cases of every suite one at a
 * time, so no other suite can observe the change.
 */
static int drm_test_sro_readout_mode_init(struct kunit *test)
{
	test->priv = (void *)(unsigned long)atomic_readout;

	return 0;
}

static void drm_test_sro_readout_mode_exit(struct kunit *test)
{
	atomic_readout = (unsigned long)test->priv;
}

/*
 * Test that a device whose objects all implement the readout and
 * comparison hooks can be read out, unless the readout is disabled
 * entirely.
 */
static void drm_test_sro_readout_mode_disabled(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	priv = mock_device_with_sro_create(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	atomic_readout = DRM_ATOMIC_READOUT_ENABLED;
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, 0);

	atomic_readout = DRM_ATOMIC_READOUT_DISABLED;
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, -ENOTSUPP);
}

/*
 * Test that a device whose CRTC has no comparison hook can only be
 * read out by the modes tolerating missing comparison
 * implementations.
 */
static void drm_test_sro_readout_mode_skip_missing_compare(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	priv = mock_device_create(test,
				  &mock_mode_config_with_sro_funcs,
				  &mock_mode_config_helper_with_sro_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_no_compare_funcs,
				  &mock_connector_with_sro_funcs);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	atomic_readout = DRM_ATOMIC_READOUT_ENABLED;
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, -ENOTSUPP);

	atomic_readout = DRM_ATOMIC_READOUT_SKIP_MISSING_COMPARE;
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Test that a device whose plane and CRTC have no readout hook can
 * only be read out by the mode tolerating missing readout
 * implementations, and that the objects without a hook then get a
 * pristine state installed rather than none at all.
 */
static void drm_test_sro_readout_mode_skip_missing_readout(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	priv = mock_device_create(test,
				  &mock_mode_config_with_sro_funcs,
				  &mock_mode_config_helper_with_sro_funcs,
				  NULL, NULL,
				  &mock_connector_with_sro_funcs);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	atomic_readout = DRM_ATOMIC_READOUT_ENABLED;
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, -ENOTSUPP);
	KUNIT_EXPECT_NULL(test, priv->plane->state);
	KUNIT_EXPECT_NULL(test, priv->crtc->state);

	atomic_readout = DRM_ATOMIC_READOUT_SKIP_MISSING_READOUT;
	ret = drm_atomic_helper_sro_readout_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_NOT_NULL(test, priv->plane->state);
	KUNIT_EXPECT_NOT_NULL(test, priv->crtc->state);
}

static struct kunit_case drm_atomic_sro_readout_mode_tests[] = {
	KUNIT_CASE(drm_test_sro_readout_mode_disabled),
	KUNIT_CASE(drm_test_sro_readout_mode_skip_missing_compare),
	KUNIT_CASE(drm_test_sro_readout_mode_skip_missing_readout),
	{ }
};

static struct kunit_suite drm_atomic_sro_readout_mode_test_suite = {
	.name = "drm_atomic_sro_readout_mode",
	.init = drm_test_sro_readout_mode_init,
	.exit = drm_test_sro_readout_mode_exit,
	.test_cases = drm_atomic_sro_readout_mode_tests,
};

/*
 * Compare helper tests
 */

/*
 * Test that two identical plane states compare as equal, without
 * reporting any mismatch.
 */
static void drm_test_sro_compare_plane_identical(struct kunit *test)
{
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_plane_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	state1 = priv->plane->funcs->atomic_create_state(priv->plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = priv->plane->funcs->atomic_create_state(priv->plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	identical = drm_atomic_helper_plane_compare_state(priv->plane, p,
							  state1, state2);
	KUNIT_EXPECT_TRUE(test, identical);

	priv->plane->funcs->atomic_destroy_state(priv->plane, state1);
	priv->plane->funcs->atomic_destroy_state(priv->plane, state2);
}

/*
 * Test that plane states differing by a single field compare as
 * different, and that the mismatching field is reported through the
 * printer.
 */
static void drm_test_sro_compare_plane_mismatch(struct kunit *test)
{
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_plane_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	state1 = priv->plane->funcs->atomic_create_state(priv->plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = priv->plane->funcs->atomic_create_state(priv->plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	state2->crtc = priv->crtc;

	identical = drm_atomic_helper_plane_compare_state(priv->plane, p,
							  state1, state2);
	KUNIT_EXPECT_FALSE(test, identical);

	priv->plane->funcs->atomic_destroy_state(priv->plane, state1);
	priv->plane->funcs->atomic_destroy_state(priv->plane, state2);
}

/*
 * Test that two identical active CRTC states compare as equal.
 */
static void drm_test_sro_compare_crtc_identical(struct kunit *test)
{
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_crtc_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	state1 = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	state1->enable = true;
	state1->active = true;
	state2->enable = true;
	state2->active = true;

	identical = drm_atomic_helper_crtc_compare_state(priv->crtc, p,
							 state1, state2);
	KUNIT_EXPECT_TRUE(test, identical);

	priv->crtc->funcs->atomic_destroy_state(priv->crtc, state1);
	priv->crtc->funcs->atomic_destroy_state(priv->crtc, state2);
}

/*
 * Test that two inactive CRTCs compare as equal even though their
 * modes differ: the modes are meaningless while inactive and must not
 * be compared.
 */
static void drm_test_sro_compare_crtc_inactive(struct kunit *test)
{
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_display_mode *mode1, *mode2;
	struct drm_crtc_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	state1 = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	/* Two different but valid modes */
	mode1 = drm_kunit_display_mode_from_cea_vic(test, &priv->drm, 16);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mode1);

	mode2 = drm_kunit_display_mode_from_cea_vic(test, &priv->drm, 4);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mode2);

	/*
	 * Both CRTCs are inactive: the modes are meaningless and must
	 * not be compared.
	 */
	drm_mode_copy(&state1->mode, mode1);
	drm_mode_copy(&state2->mode, mode2);

	identical = drm_atomic_helper_crtc_compare_state(priv->crtc, p,
							 state1, state2);
	KUNIT_EXPECT_TRUE(test, identical);

	priv->crtc->funcs->atomic_destroy_state(priv->crtc, state1);
	priv->crtc->funcs->atomic_destroy_state(priv->crtc, state2);
}

/*
 * Test that an active CRTC and an inactive one compare as different.
 */
static void drm_test_sro_compare_crtc_active_mismatch(struct kunit *test)
{
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_crtc_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	state1 = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = priv->crtc->funcs->atomic_create_state(priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	state1->enable = true;
	state1->active = true;

	identical = drm_atomic_helper_crtc_compare_state(priv->crtc, p,
							 state1, state2);
	KUNIT_EXPECT_FALSE(test, identical);

	priv->crtc->funcs->atomic_destroy_state(priv->crtc, state1);
	priv->crtc->funcs->atomic_destroy_state(priv->crtc, state2);
}

/*
 * Test that two identical connector states compare as equal.
 */
static void drm_test_sro_compare_connector_identical(struct kunit *test)
{
	struct drm_connector *connector;
	struct sro_test_priv *priv;
	struct drm_connector_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	priv = mock_device_with_sro_create(test);
	connector = &priv->connector;

	state1 = connector->funcs->atomic_create_state(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = connector->funcs->atomic_create_state(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	identical =
		drm_atomic_helper_connector_compare_state(connector, p,
							  state1, state2);
	KUNIT_EXPECT_TRUE(test, identical);

	connector->funcs->atomic_destroy_state(connector, state1);
	connector->funcs->atomic_destroy_state(connector, state2);
}

/*
 * Test that connector states whose CRTCs differ compare as different.
 */
static void drm_test_sro_compare_connector_mismatch(struct kunit *test)
{
	struct drm_connector *connector;
	struct sro_test_priv *priv;
	struct drm_connector_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	priv = mock_device_with_sro_create(test);
	connector = &priv->connector;

	state1 = connector->funcs->atomic_create_state(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = connector->funcs->atomic_create_state(connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	/*
	 * A state bound to a CRTC holds a reference on its connector,
	 * released by __drm_atomic_helper_connector_destroy_state():
	 * take it since the crtc pointer is set by hand here.
	 */
	drm_connector_get(connector);
	state2->crtc = priv->crtc;

	identical =
		drm_atomic_helper_connector_compare_state(connector, p,
							  state1, state2);
	KUNIT_EXPECT_FALSE(test, identical);

	connector->funcs->atomic_destroy_state(connector, state1);
	connector->funcs->atomic_destroy_state(connector, state2);
}

/*
 * Test that two identical bridge states compare as equal.
 */
static void drm_test_sro_compare_bridge_identical(struct kunit *test)
{
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_bridge_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	priv->bridge.funcs = &mock_bridge_funcs;
	priv->bridge.name = "test bridge";

	state1 = drm_atomic_helper_bridge_create_state(&priv->bridge);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = drm_atomic_helper_bridge_create_state(&priv->bridge);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	identical = drm_atomic_helper_bridge_compare_state(&priv->bridge, p,
							   state1, state2);
	KUNIT_EXPECT_TRUE(test, identical);

	priv->bridge.funcs->atomic_destroy_state(&priv->bridge, state1);
	priv->bridge.funcs->atomic_destroy_state(&priv->bridge, state2);
}

/*
 * Test that bridge states whose input bus formats differ compare as
 * different.
 */
static void drm_test_sro_compare_bridge_mismatch(struct kunit *test)
{
	struct sro_test_priv *priv = mock_device_with_sro_create(test);
	struct drm_bridge_state *state1, *state2;
	struct drm_printer *p;
	bool identical;

	p = drm_kunit_helper_printer(test, LOGLEVEL_ERR);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p);

	priv->bridge.funcs = &mock_bridge_funcs;
	priv->bridge.name = "test bridge";

	state1 = drm_atomic_helper_bridge_create_state(&priv->bridge);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state1);

	state2 = drm_atomic_helper_bridge_create_state(&priv->bridge);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state2);

	state2->input_bus_cfg.format = MEDIA_BUS_FMT_RGB888_1X24;

	identical = drm_atomic_helper_bridge_compare_state(&priv->bridge, p,
							   state1, state2);
	KUNIT_EXPECT_FALSE(test, identical);

	priv->bridge.funcs->atomic_destroy_state(&priv->bridge, state1);
	priv->bridge.funcs->atomic_destroy_state(&priv->bridge, state2);
}

static struct kunit_case drm_atomic_sro_compare_tests[] = {
	KUNIT_CASE(drm_test_sro_compare_plane_identical),
	KUNIT_CASE(drm_test_sro_compare_plane_mismatch),
	KUNIT_CASE(drm_test_sro_compare_crtc_identical),
	KUNIT_CASE(drm_test_sro_compare_crtc_inactive),
	KUNIT_CASE(drm_test_sro_compare_crtc_active_mismatch),
	KUNIT_CASE(drm_test_sro_compare_connector_identical),
	KUNIT_CASE(drm_test_sro_compare_connector_mismatch),
	KUNIT_CASE(drm_test_sro_compare_bridge_identical),
	KUNIT_CASE(drm_test_sro_compare_bridge_mismatch),
	{ }
};

static struct kunit_suite drm_atomic_sro_compare_test_suite = {
	.name = "drm_atomic_sro_compare",
	.test_cases = drm_atomic_sro_compare_tests,
};

/*
 * drm_mode_config_create_initial_state() tests
 */

/*
 * Test that when the readout succeeds,
 * drm_mode_config_create_initial_state() installs the states it read
 * out and doesn't reset the hardware.
 */
static void drm_test_mode_config_initial_state_readout(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	priv = mock_device_create(test,
				  &mock_mode_config_with_sro_hw_reset_funcs,
				  &mock_mode_config_helper_without_sro_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_funcs,
				  &mock_connector_with_sro_funcs);

	ret = drm_mode_config_create_initial_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* The read out state has been installed */
	KUNIT_EXPECT_TRUE(test, priv->crtc->state->enable);
	KUNIT_EXPECT_TRUE(test, priv->crtc->state->active);

	/* No fallback to a hardware reset has been needed */
	KUNIT_EXPECT_FALSE(test, priv->hw_reset_called);
}

/*
 * Test that when the driver reports the hardware configuration as
 * unsupported, drm_mode_config_create_initial_state() falls back to
 * the regular initialization path: the hardware is reset and pristine
 * states are created.
 */
static void drm_test_mode_config_initial_state_readout_error(struct kunit *test)
{
	struct mock_privobj_state *privobj_state;
	struct sro_test_priv *priv;
	int ret;

	priv = mock_device_create(test,
				  &mock_mode_config_with_sro_err_funcs,
				  &mock_mode_config_helper_without_sro_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_funcs,
				  &mock_connector_with_sro_funcs);

	ret = drm_mode_config_create_initial_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* The regular initialization path has been taken */
	KUNIT_EXPECT_TRUE(test, priv->hw_reset_called);

	/* A pristine state has been created for all the objects */
	KUNIT_ASSERT_NOT_NULL(test, priv->crtc->state);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->enable);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->active);
	KUNIT_ASSERT_NOT_NULL(test, priv->plane->state);
	KUNIT_ASSERT_NOT_NULL(test, priv->connector.state);

	KUNIT_ASSERT_NOT_NULL(test, priv->private_obj.state);
	privobj_state = to_mock_privobj_state(priv->private_obj.state);
	KUNIT_EXPECT_FALSE(test, privobj_state->readout);
}

/*
 * Test that when an object readout hook reports the hardware
 * configuration as unsupported, drm_mode_config_create_initial_state()
 * falls back to the regular initialization path.
 */
static void
drm_test_mode_config_initial_state_object_readout_error(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	priv = mock_device_create(test,
				  &mock_mode_config_with_sro_funcs,
				  &mock_mode_config_helper_with_sro_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_funcs,
				  &mock_connector_with_sro_err_funcs);

	ret = drm_mode_config_create_initial_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* The regular initialization path has been taken */
	KUNIT_EXPECT_TRUE(test, priv->hw_reset_called);

	/* A pristine state has been created for all the objects */
	KUNIT_ASSERT_NOT_NULL(test, priv->crtc->state);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->enable);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->active);
	KUNIT_ASSERT_NOT_NULL(test, priv->plane->state);
	KUNIT_ASSERT_NOT_NULL(test, priv->connector.state);
}

/*
 * Test that a driver without readout support takes the regular
 * initialization path as well.
 */
static void drm_test_mode_config_initial_state_no_readout(struct kunit *test)
{
	struct sro_test_priv *priv;
	int ret;

	priv = mock_device_create(test,
				  &mock_mode_config_without_sro_with_hw_reset_funcs,
				  &mock_mode_config_helper_without_sro_funcs,
				  &mock_plane_with_sro_funcs,
				  &mock_crtc_with_sro_funcs,
				  &mock_connector_with_sro_funcs);

	ret = drm_mode_config_create_initial_state(&priv->drm);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* The regular initialization path has been taken */
	KUNIT_EXPECT_TRUE(test, priv->hw_reset_called);

	KUNIT_ASSERT_NOT_NULL(test, priv->crtc->state);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->enable);
	KUNIT_EXPECT_FALSE(test, priv->crtc->state->active);
	KUNIT_ASSERT_NOT_NULL(test, priv->plane->state);
	KUNIT_ASSERT_NOT_NULL(test, priv->connector.state);
}

static struct kunit_case drm_mode_config_initial_state_tests[] = {
	KUNIT_CASE(drm_test_mode_config_initial_state_readout),
	KUNIT_CASE(drm_test_mode_config_initial_state_readout_error),
	KUNIT_CASE(drm_test_mode_config_initial_state_object_readout_error),
	KUNIT_CASE(drm_test_mode_config_initial_state_no_readout),
	{ }
};

static struct kunit_suite drm_mode_config_initial_state_test_suite = {
	.name = "drm_mode_config_initial_state",
	.test_cases = drm_mode_config_initial_state_tests,
};

kunit_test_suites(
	&drm_atomic_sro_compare_test_suite,
	&drm_atomic_sro_readout_test_suite,
	&drm_atomic_sro_readout_mode_test_suite,
	&drm_atomic_sro_state_test_suite,
	&drm_mode_config_initial_state_test_suite,
);

MODULE_AUTHOR("Maxime Ripard <mripard@kernel.org>");
MODULE_DESCRIPTION("Kunit test for the DRM atomic state readout");
MODULE_LICENSE("GPL");
