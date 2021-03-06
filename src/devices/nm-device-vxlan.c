/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright 2013 - 2015 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-device-vxlan.h"

#include <string.h>

#include "nm-device-private.h"
#include "nm-manager.h"
#include "platform/nm-platform.h"
#include "nm-utils.h"
#include "nm-device-factory.h"
#include "nm-setting-vxlan.h"
#include "nm-setting-wired.h"
#include "settings/nm-settings.h"
#include "nm-act-request.h"
#include "nm-ip4-config.h"

#include "introspection/org.freedesktop.NetworkManager.Device.Vxlan.h"

#include "nm-device-logging.h"
_LOG_DECLARE_SELF(NMDeviceVxlan);

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE (NMDeviceVxlan,
	PROP_ID,
	PROP_LOCAL,
	PROP_GROUP,
	PROP_TOS,
	PROP_TTL,
	PROP_LEARNING,
	PROP_AGEING,
	PROP_LIMIT,
	PROP_SRC_PORT_MIN,
	PROP_SRC_PORT_MAX,
	PROP_DST_PORT,
	PROP_PROXY,
	PROP_RSC,
	PROP_L2MISS,
	PROP_L3MISS,
);

typedef struct {
	NMPlatformLnkVxlan props;
} NMDeviceVxlanPrivate;

struct _NMDeviceVxlan {
	NMDevice parent;
	NMDeviceVxlanPrivate _priv;
};

struct _NMDeviceVxlanClass {
	NMDeviceClass parent;
};

G_DEFINE_TYPE (NMDeviceVxlan, nm_device_vxlan, NM_TYPE_DEVICE)

#define NM_DEVICE_VXLAN_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMDeviceVxlan, NM_IS_DEVICE_VXLAN)

/*****************************************************************************/

static void
update_properties (NMDevice *device)
{
	NMDeviceVxlan *self = NM_DEVICE_VXLAN (device);
	NMDeviceVxlanPrivate *priv = NM_DEVICE_VXLAN_GET_PRIVATE (self);
	GObject *object = G_OBJECT (device);
	const NMPlatformLnkVxlan *props;

	props = nm_platform_link_get_lnk_vxlan (NM_PLATFORM_GET, nm_device_get_ifindex (device), NULL);
	if (!props) {
		_LOGW (LOGD_PLATFORM, "could not get vxlan properties");
		return;
	}

	g_object_freeze_notify (object);

	if (priv->props.parent_ifindex != props->parent_ifindex)
		nm_device_parent_set_ifindex (device, props->parent_ifindex);
	if (priv->props.id != props->id)
		_notify (self, PROP_ID);
	if (priv->props.local != props->local)
		_notify (self, PROP_LOCAL);
	if (memcmp (&priv->props.local6, &props->local6, sizeof (props->local6)) != 0)
		_notify (self, PROP_LOCAL);
	if (priv->props.group != props->group)
		_notify (self, PROP_GROUP);
	if (memcmp (&priv->props.group6, &props->group6, sizeof (props->group6)) != 0)
		_notify (self, PROP_GROUP);
	if (priv->props.tos != props->tos)
		_notify (self, PROP_TOS);
	if (priv->props.ttl != props->ttl)
		_notify (self, PROP_TTL);
	if (priv->props.learning != props->learning)
		_notify (self, PROP_LEARNING);
	if (priv->props.ageing != props->ageing)
		_notify (self, PROP_AGEING);
	if (priv->props.limit != props->limit)
		_notify (self, PROP_LIMIT);
	if (priv->props.src_port_min != props->src_port_min)
		_notify (self, PROP_SRC_PORT_MIN);
	if (priv->props.src_port_max != props->src_port_max)
		_notify (self, PROP_SRC_PORT_MAX);
	if (priv->props.dst_port != props->dst_port)
		_notify (self, PROP_DST_PORT);
	if (priv->props.proxy != props->proxy)
		_notify (self, PROP_PROXY);
	if (priv->props.rsc != props->rsc)
		_notify (self, PROP_RSC);
	if (priv->props.l2miss != props->l2miss)
		_notify (self, PROP_L2MISS);
	if (priv->props.l3miss != props->l3miss)
		_notify (self, PROP_L3MISS);

	priv->props = *props;

	g_object_thaw_notify (object);
}

static NMDeviceCapabilities
get_generic_capabilities (NMDevice *dev)
{
	return NM_DEVICE_CAP_IS_SOFTWARE;
}

static void
link_changed (NMDevice *device,
              const NMPlatformLink *pllink)
{
	NM_DEVICE_CLASS (nm_device_vxlan_parent_class)->link_changed (device, pllink);
	update_properties (device);
}

static void
unrealize_notify (NMDevice *device)
{
	NMDeviceVxlan *self = NM_DEVICE_VXLAN (device);
	NMDeviceVxlanPrivate *priv = NM_DEVICE_VXLAN_GET_PRIVATE (self);
	guint i;

	NM_DEVICE_CLASS (nm_device_vxlan_parent_class)->unrealize_notify (device);

	memset (&priv->props, 0, sizeof (NMPlatformLnkVxlan));

	for (i = 1; i < _PROPERTY_ENUMS_LAST; i++)
		g_object_notify_by_pspec (G_OBJECT (self), obj_properties[i]);
}

static gboolean
create_and_realize (NMDevice *device,
                    NMConnection *connection,
                    NMDevice *parent,
                    const NMPlatformLink **out_plink,
                    GError **error)
{
	const char *iface = nm_device_get_iface (device);
	NMPlatformError plerr;
	NMPlatformLnkVxlan props = { };
	NMSettingVxlan *s_vxlan;
	const char *str;
	int ret;

	s_vxlan = nm_connection_get_setting_vxlan (connection);
	g_assert (s_vxlan);

	if (parent)
		props.parent_ifindex = nm_device_get_ifindex (parent);

	props.id = nm_setting_vxlan_get_id (s_vxlan);

	str = nm_setting_vxlan_get_local (s_vxlan);
	if (str) {
		ret = inet_pton (AF_INET, str, &props.local);
		if (ret != 1)
			ret = inet_pton (AF_INET6, str, &props.local6);
		if (ret != 1)
			return FALSE;
	}

	str = nm_setting_vxlan_get_remote (s_vxlan);
	ret = inet_pton (AF_INET, str, &props.group);
	if (ret != 1)
		ret = inet_pton (AF_INET6, str, &props.group6);
	if (ret != 1)
		return FALSE;

	props.tos = nm_setting_vxlan_get_tos (s_vxlan);
	props.ttl = nm_setting_vxlan_get_ttl (s_vxlan);
	props.learning = nm_setting_vxlan_get_learning (s_vxlan);
	props.ageing = nm_setting_vxlan_get_ageing (s_vxlan);
	props.limit = nm_setting_vxlan_get_limit (s_vxlan);
	props.src_port_min = nm_setting_vxlan_get_source_port_min (s_vxlan);
	props.src_port_max = nm_setting_vxlan_get_source_port_max (s_vxlan);
	props.dst_port = nm_setting_vxlan_get_destination_port (s_vxlan);
	props.proxy = nm_setting_vxlan_get_proxy (s_vxlan);
	props.rsc = nm_setting_vxlan_get_rsc (s_vxlan);
	props.l2miss = nm_setting_vxlan_get_l2_miss (s_vxlan);
	props.l3miss = nm_setting_vxlan_get_l3_miss (s_vxlan);

	plerr = nm_platform_link_vxlan_add (NM_PLATFORM_GET, iface, &props, out_plink);
	if (plerr != NM_PLATFORM_ERROR_SUCCESS) {
		g_set_error (error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_CREATION_FAILED,
		             "Failed to create VXLAN interface '%s' for '%s': %s",
		             iface,
		             nm_connection_get_id (connection),
		             nm_platform_error_to_string (plerr));
		return FALSE;
	}

	return TRUE;
}

static gboolean
match_parent (NMDeviceVxlan *self, const char *parent)
{
	NMDevice *parent_device;

	g_return_val_if_fail (parent != NULL, FALSE);

	parent_device = nm_device_parent_get_device (NM_DEVICE (self));
	if (!parent_device)
		return FALSE;

	if (nm_utils_is_uuid (parent)) {
		NMActRequest *parent_req;
		NMConnection *parent_connection;

		/* If the parent is a UUID, the connection matches if our parent
		 * device has that connection activated.
		 */
		parent_req = nm_device_get_act_request (parent_device);
		if (!parent_req)
			return FALSE;

		parent_connection = nm_active_connection_get_applied_connection (NM_ACTIVE_CONNECTION (parent_req));
		if (!parent_connection)
			return FALSE;

		if (g_strcmp0 (parent, nm_connection_get_uuid (parent_connection)) != 0)
			return FALSE;
	} else {
		/* interface name */
		if (g_strcmp0 (parent, nm_device_get_ip_iface (parent_device)) != 0)
			return FALSE;
	}

	return TRUE;
}

static gboolean
address_matches (const char *str, in_addr_t addr4, struct in6_addr *addr6)
{
	in_addr_t new_addr4 = 0;
	struct in6_addr new_addr6 = { };

	if (!str) {
		return    addr4 == 0
		       && !memcmp (addr6, &in6addr_any, sizeof (in6addr_any));
	}

	if (inet_pton (AF_INET, str, &new_addr4) == 1)
		return new_addr4 == addr4;
	else if (inet_pton (AF_INET6, str, &new_addr6) == 1)
		return !memcmp (&new_addr6, addr6, sizeof (new_addr6));
	else
		return FALSE;
}

static gboolean
check_connection_compatible (NMDevice *device, NMConnection *connection)
{
	NMDeviceVxlanPrivate *priv = NM_DEVICE_VXLAN_GET_PRIVATE ((NMDeviceVxlan *) device);
	NMSettingVxlan *s_vxlan;
	const char *parent;

	if (!NM_DEVICE_CLASS (nm_device_vxlan_parent_class)->check_connection_compatible (device, connection))
		return FALSE;

	s_vxlan = nm_connection_get_setting_vxlan (connection);
	if (!s_vxlan)
		return FALSE;

	if (nm_device_is_real (device)) {
		parent = nm_setting_vxlan_get_parent (s_vxlan);
		if (   parent
		    && !match_parent (NM_DEVICE_VXLAN (device), parent))
			return FALSE;

		if (priv->props.id != nm_setting_vxlan_get_id (s_vxlan))
			return FALSE;

		if (!address_matches (nm_setting_vxlan_get_local (s_vxlan), priv->props.local, &priv->props.local6))
			return FALSE;

		if (!address_matches (nm_setting_vxlan_get_remote (s_vxlan), priv->props.group, &priv->props.group6))
			return FALSE;

		if (priv->props.src_port_min != nm_setting_vxlan_get_source_port_min (s_vxlan))
			return FALSE;

		if (priv->props.src_port_max != nm_setting_vxlan_get_source_port_max (s_vxlan))
			return FALSE;

		if (priv->props.dst_port != nm_setting_vxlan_get_destination_port (s_vxlan))
			return FALSE;

		if (priv->props.tos != nm_setting_vxlan_get_tos (s_vxlan))
			return FALSE;

		if (priv->props.ttl != nm_setting_vxlan_get_ttl (s_vxlan))
			return FALSE;

		if (priv->props.learning != nm_setting_vxlan_get_learning (s_vxlan))
			return FALSE;

		if (priv->props.ageing != nm_setting_vxlan_get_ageing (s_vxlan))
			return FALSE;

		if (priv->props.proxy != nm_setting_vxlan_get_proxy (s_vxlan))
			return FALSE;

		if (priv->props.rsc != nm_setting_vxlan_get_rsc (s_vxlan))
			return FALSE;

		if (priv->props.l2miss != nm_setting_vxlan_get_l2_miss (s_vxlan))
			return FALSE;

		if (priv->props.l3miss != nm_setting_vxlan_get_l3_miss (s_vxlan))
			return FALSE;
	}

	return TRUE;
}

static gboolean
complete_connection (NMDevice *device,
                     NMConnection *connection,
                     const char *specific_object,
                     const GSList *existing_connections,
                     GError **error)
{
	NMSettingVxlan *s_vxlan;

	nm_utils_complete_generic (NM_PLATFORM_GET,
	                           connection,
	                           NM_SETTING_VXLAN_SETTING_NAME,
	                           existing_connections,
	                           NULL,
	                           _("VXLAN connection"),
	                           NULL,
	                           TRUE);

	s_vxlan = nm_connection_get_setting_vxlan (connection);
	if (!s_vxlan) {
		g_set_error_literal (error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_INVALID_CONNECTION,
		                     "A 'vxlan' setting is required.");
		return FALSE;
	}

	return TRUE;
}

static void
update_connection (NMDevice *device, NMConnection *connection)
{
	NMDeviceVxlanPrivate *priv = NM_DEVICE_VXLAN_GET_PRIVATE ((NMDeviceVxlan *) device);
	NMSettingVxlan *s_vxlan = nm_connection_get_setting_vxlan (connection);
	NMDevice *parent_device;
	const char *setting_parent;
	const char *new_parent = NULL;

	if (!s_vxlan) {
		s_vxlan = (NMSettingVxlan *) nm_setting_vxlan_new ();
		nm_connection_add_setting (connection, (NMSetting *) s_vxlan);
	}

	if (priv->props.id != nm_setting_vxlan_get_id (s_vxlan))
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_ID, priv->props.id, NULL);

	parent_device = nm_device_parent_get_device (device);

	/* Update parent in the connection; default to parent's interface name */
	if (parent_device) {
		new_parent = nm_device_get_iface (parent_device);
		setting_parent = nm_setting_vxlan_get_parent (s_vxlan);
		if (setting_parent && nm_utils_is_uuid (setting_parent)) {
			NMConnection *parent_connection;

			/* Don't change a parent specified by UUID if it's still valid */
			parent_connection = (NMConnection *) nm_settings_get_connection_by_uuid (nm_device_get_settings (device),
			                                                                         setting_parent);
			if (parent_connection && nm_device_check_connection_compatible (parent_device, parent_connection))
				new_parent = NULL;
		}
	}
	g_object_set (s_vxlan, NM_SETTING_VXLAN_PARENT, new_parent, NULL);

	if (!address_matches (nm_setting_vxlan_get_remote (s_vxlan), priv->props.group, &priv->props.group6)) {
		if (priv->props.group) {
			g_object_set (s_vxlan, NM_SETTING_VXLAN_REMOTE,
			              nm_utils_inet4_ntop (priv->props.group, NULL),
			              NULL);
		} else {
			g_object_set (s_vxlan, NM_SETTING_VXLAN_REMOTE,
			              nm_utils_inet6_ntop (&priv->props.group6, NULL),
			              NULL);
		}
	}

	if (!address_matches (nm_setting_vxlan_get_local (s_vxlan), priv->props.local, &priv->props.local6)) {
		if (priv->props.local) {
			g_object_set (s_vxlan, NM_SETTING_VXLAN_LOCAL,
			              nm_utils_inet4_ntop (priv->props.local, NULL),
			              NULL);
		} else if (memcmp (&priv->props.local6, &in6addr_any, sizeof (in6addr_any))) {
			g_object_set (s_vxlan, NM_SETTING_VXLAN_LOCAL,
			              nm_utils_inet6_ntop (&priv->props.local6, NULL),
			              NULL);
		}
	}

	if (priv->props.src_port_min != nm_setting_vxlan_get_source_port_min (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_SOURCE_PORT_MIN,
		              priv->props.src_port_min, NULL);
	}

	if (priv->props.src_port_max != nm_setting_vxlan_get_source_port_max (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_SOURCE_PORT_MAX,
		              priv->props.src_port_max, NULL);
	}

	if (priv->props.dst_port != nm_setting_vxlan_get_destination_port (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_DESTINATION_PORT,
		              priv->props.dst_port, NULL);
	}

	if (priv->props.tos != nm_setting_vxlan_get_tos (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_TOS,
		              priv->props.tos, NULL);
	}

	if (priv->props.ttl != nm_setting_vxlan_get_ttl (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_TTL,
		              priv->props.ttl, NULL);
	}

	if (priv->props.learning != nm_setting_vxlan_get_learning (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_LEARNING,
		              priv->props.learning, NULL);
	}

	if (priv->props.ageing != nm_setting_vxlan_get_ageing (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_AGEING,
		              priv->props.ageing, NULL);
	}

	if (priv->props.proxy != nm_setting_vxlan_get_proxy (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_PROXY,
		              priv->props.proxy, NULL);
	}

	if (priv->props.rsc != nm_setting_vxlan_get_rsc (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_RSC,
		              priv->props.rsc, NULL);
	}

	if (priv->props.l2miss != nm_setting_vxlan_get_l2_miss (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_L2_MISS,
		              priv->props.l2miss, NULL);
	}

	if (priv->props.l3miss != nm_setting_vxlan_get_l3_miss (s_vxlan)) {
		g_object_set (G_OBJECT (s_vxlan), NM_SETTING_VXLAN_L3_MISS,
		              priv->props.l3miss, NULL);
	}
}

static NMActStageReturn
act_stage1_prepare (NMDevice *device, NMDeviceStateReason *out_failure_reason)
{
	NMActStageReturn ret;

	ret = NM_DEVICE_CLASS (nm_device_vxlan_parent_class)->act_stage1_prepare (device, out_failure_reason);
	if (ret != NM_ACT_STAGE_RETURN_SUCCESS)
		return ret;

	if (!nm_device_hw_addr_set_cloned (device, nm_device_get_applied_connection (device), FALSE))
		return NM_ACT_STAGE_RETURN_FAILURE;

	return NM_ACT_STAGE_RETURN_SUCCESS;
}

/*****************************************************************************/

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMDeviceVxlanPrivate *priv = NM_DEVICE_VXLAN_GET_PRIVATE ((NMDeviceVxlan *) object);

	switch (prop_id) {
	case PROP_ID:
		g_value_set_uint (value, priv->props.id);
		break;
	case PROP_GROUP:
		if (priv->props.group)
			g_value_set_string (value, nm_utils_inet4_ntop (priv->props.group, NULL));
		else if (!IN6_IS_ADDR_UNSPECIFIED (&priv->props.group6))
			g_value_set_string (value, nm_utils_inet6_ntop (&priv->props.group6, NULL));
		break;
	case PROP_LOCAL:
		if (priv->props.local)
			g_value_set_string (value, nm_utils_inet4_ntop (priv->props.local, NULL));
		else if (!IN6_IS_ADDR_UNSPECIFIED (&priv->props.local6))
			g_value_set_string (value, nm_utils_inet6_ntop (&priv->props.local6, NULL));
		break;
	case PROP_TOS:
		g_value_set_uchar (value, priv->props.tos);
		break;
	case PROP_TTL:
		g_value_set_uchar (value, priv->props.ttl);
		break;
	case PROP_LEARNING:
		g_value_set_boolean (value, priv->props.learning);
		break;
	case PROP_AGEING:
		g_value_set_uint (value, priv->props.ageing);
		break;
	case PROP_LIMIT:
		g_value_set_uint (value, priv->props.limit);
		break;
	case PROP_DST_PORT:
		g_value_set_uint (value, priv->props.dst_port);
		break;
	case PROP_SRC_PORT_MIN:
		g_value_set_uint (value, priv->props.src_port_min);
		break;
	case PROP_SRC_PORT_MAX:
		g_value_set_uint (value, priv->props.src_port_max);
		break;
	case PROP_PROXY:
		g_value_set_boolean (value, priv->props.proxy);
		break;
	case PROP_RSC:
		g_value_set_boolean (value, priv->props.rsc);
		break;
	case PROP_L2MISS:
		g_value_set_boolean (value, priv->props.l2miss);
		break;
	case PROP_L3MISS:
		g_value_set_boolean (value, priv->props.l3miss);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*****************************************************************************/

static void
nm_device_vxlan_init (NMDeviceVxlan *self)
{
}

static void
nm_device_vxlan_class_init (NMDeviceVxlanClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceClass *device_class = NM_DEVICE_CLASS (klass);

	NM_DEVICE_CLASS_DECLARE_TYPES (klass, NULL, NM_LINK_TYPE_VXLAN)

	object_class->get_property = get_property;

	device_class->link_changed = link_changed;
	device_class->unrealize_notify = unrealize_notify;
	device_class->connection_type = NM_SETTING_VXLAN_SETTING_NAME;
	device_class->create_and_realize = create_and_realize;
	device_class->check_connection_compatible = check_connection_compatible;
	device_class->complete_connection = complete_connection;
	device_class->get_generic_capabilities = get_generic_capabilities;
	device_class->update_connection = update_connection;
	device_class->act_stage1_prepare = act_stage1_prepare;
	device_class->get_configured_mtu = nm_device_get_configured_mtu_for_wired;

	obj_properties[PROP_ID] =
	     g_param_spec_uint (NM_DEVICE_VXLAN_ID, "", "",
	                        0, G_MAXUINT32, 0,
	                        G_PARAM_READABLE |
	                        G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_LOCAL] =
	     g_param_spec_string (NM_DEVICE_VXLAN_LOCAL, "", "",
	                          NULL,
	                          G_PARAM_READABLE |
	                          G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_GROUP] =
	     g_param_spec_string (NM_DEVICE_VXLAN_GROUP, "", "",
	                          NULL,
	                          G_PARAM_READABLE |
	                          G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_TOS] =
	     g_param_spec_uchar (NM_DEVICE_VXLAN_TOS, "", "",
	                         0, 255, 0,
	                         G_PARAM_READABLE |
	                         G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_TTL] =
	     g_param_spec_uchar (NM_DEVICE_VXLAN_TTL, "", "",
	                         0, 255, 0,
	                         G_PARAM_READABLE |
	                         G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_LEARNING] =
	     g_param_spec_boolean (NM_DEVICE_VXLAN_LEARNING, "", "",
	                           FALSE,
	                           G_PARAM_READABLE |
	                           G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_AGEING] =
	     g_param_spec_uint (NM_DEVICE_VXLAN_AGEING, "", "",
	                        0, G_MAXUINT32, 0,
	                        G_PARAM_READABLE |
	                        G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_LIMIT] =
	     g_param_spec_uint (NM_DEVICE_VXLAN_LIMIT, "", "",
	                        0, G_MAXUINT32, 0,
	                        G_PARAM_READABLE |
	                        G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_SRC_PORT_MIN] =
	     g_param_spec_uint (NM_DEVICE_VXLAN_SRC_PORT_MIN, "", "",
	                        0, 65535, 0,
	                        G_PARAM_READABLE |
	                        G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_SRC_PORT_MAX] =
	     g_param_spec_uint (NM_DEVICE_VXLAN_SRC_PORT_MAX, "", "",
	                        0, 65535, 0,
	                        G_PARAM_READABLE |
	                        G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_DST_PORT] =
	     g_param_spec_uint (NM_DEVICE_VXLAN_DST_PORT, "", "",
	                        0, 65535, 0,
	                        G_PARAM_READABLE |
	                        G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_PROXY] =
	     g_param_spec_boolean (NM_DEVICE_VXLAN_PROXY, "", "",
	                           FALSE,
	                           G_PARAM_READABLE |
	                           G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_RSC] =
	     g_param_spec_boolean (NM_DEVICE_VXLAN_RSC, "", "",
	                           FALSE,
	                           G_PARAM_READABLE |
	                           G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_L2MISS] =
	     g_param_spec_boolean (NM_DEVICE_VXLAN_L2MISS, "", "",
	                           FALSE,
	                           G_PARAM_READABLE |
	                           G_PARAM_STATIC_STRINGS);

	obj_properties[PROP_L3MISS] =
	     g_param_spec_boolean (NM_DEVICE_VXLAN_L3MISS, "", "",
	                           FALSE,
	                           G_PARAM_READABLE |
	                           G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, _PROPERTY_ENUMS_LAST, obj_properties);

	nm_exported_object_class_add_interface (NM_EXPORTED_OBJECT_CLASS (klass),
	                                        NMDBUS_TYPE_DEVICE_VXLAN_SKELETON,
	                                        NULL);
}

/*****************************************************************************/

#define NM_TYPE_VXLAN_DEVICE_FACTORY (nm_vxlan_device_factory_get_type ())
#define NM_VXLAN_DEVICE_FACTORY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_VXLAN_DEVICE_FACTORY, NMVxlanDeviceFactory))

static NMDevice *
create_device (NMDeviceFactory *factory,
               const char *iface,
               const NMPlatformLink *plink,
               NMConnection *connection,
               gboolean *out_ignore)
{
	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_VXLAN,
	                                  NM_DEVICE_IFACE, iface,
	                                  NM_DEVICE_TYPE_DESC, "Vxlan",
	                                  NM_DEVICE_DEVICE_TYPE, NM_DEVICE_TYPE_VXLAN,
	                                  NM_DEVICE_LINK_TYPE, NM_LINK_TYPE_VXLAN,
	                                  NULL);
}

static const char *
get_connection_parent (NMDeviceFactory *factory, NMConnection *connection)
{
	NMSettingVxlan *s_vxlan;

	g_return_val_if_fail (nm_connection_is_type (connection, NM_SETTING_VXLAN_SETTING_NAME), NULL);

	s_vxlan = nm_connection_get_setting_vxlan (connection);
	g_assert (s_vxlan);

	return nm_setting_vxlan_get_parent (s_vxlan);
}

static char *
get_connection_iface (NMDeviceFactory *factory,
                      NMConnection *connection,
                      const char *parent_iface)
{
	const char *ifname;
	NMSettingVxlan *s_vxlan;

	g_return_val_if_fail (nm_connection_is_type (connection, NM_SETTING_VXLAN_SETTING_NAME), NULL);

	s_vxlan = nm_connection_get_setting_vxlan (connection);
	g_assert (s_vxlan);

	if (nm_setting_vxlan_get_parent (s_vxlan) && !parent_iface)
		return NULL;

	ifname = nm_connection_get_interface_name (connection);
	return g_strdup (ifname);
}

NM_DEVICE_FACTORY_DEFINE_INTERNAL (VXLAN, Vxlan, vxlan,
	NM_DEVICE_FACTORY_DECLARE_LINK_TYPES (NM_LINK_TYPE_VXLAN)
	NM_DEVICE_FACTORY_DECLARE_SETTING_TYPES (NM_SETTING_VXLAN_SETTING_NAME),
	factory_class->create_device = create_device;
	factory_class->get_connection_parent = get_connection_parent;
	factory_class->get_connection_iface = get_connection_iface;
);
