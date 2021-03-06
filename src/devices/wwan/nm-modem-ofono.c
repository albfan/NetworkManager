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
 * Copyright (C) 2013 - 2016 Canonical Ltd.
 */

#include "nm-default.h"

#include "nm-modem-ofono.h"

#include <string.h>

#include "nm-core-internal.h"
#include "devices/nm-device-private.h"
#include "nm-modem.h"
#include "platform/nm-platform.h"
#include "nm-ip4-config.h"

#define VARIANT_IS_OF_TYPE_BOOLEAN(v)      ((v) != NULL && ( g_variant_is_of_type ((v), G_VARIANT_TYPE_BOOLEAN) ))
#define VARIANT_IS_OF_TYPE_STRING(v)       ((v) != NULL && ( g_variant_is_of_type ((v), G_VARIANT_TYPE_STRING) ))
#define VARIANT_IS_OF_TYPE_OBJECT_PATH(v)  ((v) != NULL && ( g_variant_is_of_type ((v), G_VARIANT_TYPE_OBJECT_PATH) ))
#define VARIANT_IS_OF_TYPE_STRING_ARRAY(v) ((v) != NULL && ( g_variant_is_of_type ((v), G_VARIANT_TYPE_STRING_ARRAY) ))
#define VARIANT_IS_OF_TYPE_DICTIONARY(v)   ((v) != NULL && ( g_variant_is_of_type ((v), G_VARIANT_TYPE_DICTIONARY) ))

/*****************************************************************************/

typedef struct {
	GHashTable *connect_properties;

	GDBusProxy *modem_proxy;
	GDBusProxy *connman_proxy;
	GDBusProxy *context_proxy;
	GDBusProxy *sim_proxy;

	GError *property_error;

	char *context_path;
	char *imsi;

	gboolean modem_online;
	gboolean gprs_attached;

	NMIP4Config *ip4_config;
} NMModemOfonoPrivate;

struct _NMModemOfono {
	NMModem parent;
	NMModemOfonoPrivate _priv;
};

struct _NMModemOfonoClass {
	NMModemClass parent;
};

G_DEFINE_TYPE (NMModemOfono, nm_modem_ofono, NM_TYPE_MODEM)

#define NM_MODEM_OFONO_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMModemOfono, NM_IS_MODEM_OFONO)

/*****************************************************************************/

#define _NMLOG_DOMAIN      LOGD_MB
#define _NMLOG_PREFIX_NAME "modem-ofono"
#define _NMLOG(level, ...) \
    G_STMT_START { \
        const NMLogLevel _level = (level); \
        \
        if (nm_logging_enabled (_level, (_NMLOG_DOMAIN))) { \
            NMModemOfono *const __self = (self); \
            char __prefix_name[128]; \
            const char *__uid; \
            \
            _nm_log (_level, (_NMLOG_DOMAIN), 0, NULL, NULL, \
                     "%s%s: " _NM_UTILS_MACRO_FIRST(__VA_ARGS__), \
                     _NMLOG_PREFIX_NAME, \
                     (__self \
                         ? ({ \
                                ((__uid = nm_modem_get_uid ((NMModem *) __self)) \
                                    ? nm_sprintf_buf (__prefix_name, "[%s]", __uid) \
                                    : "(null)"); \
                            }) \
                         : "") \
                     _NM_UTILS_MACRO_REST(__VA_ARGS__)); \
        } \
    } G_STMT_END

/*****************************************************************************/

static gboolean
ip_string_to_network_address (const gchar *str,
                              guint32 *out)
{
	guint32 addr = 0;
	gboolean success = FALSE;

	if (!str || inet_pton (AF_INET, str, &addr) != 1)
		addr = 0;
	else
		success = TRUE;

	*out = (guint32)addr;
	return success;
}

static void
get_capabilities (NMModem *_self,
                  NMDeviceModemCapabilities *modem_caps,
                  NMDeviceModemCapabilities *current_caps)
{
	/* FIXME: auto-detect capabilities to allow LTE */
	*modem_caps = NM_DEVICE_MODEM_CAPABILITY_GSM_UMTS;
	*current_caps = NM_DEVICE_MODEM_CAPABILITY_GSM_UMTS;
}

static void
update_modem_state (NMModemOfono *self)
{
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	NMModemState state = nm_modem_get_state (NM_MODEM (self));
	NMModemState new_state = NM_MODEM_STATE_DISABLED;
	const char *reason = NULL;

	_LOGI ("'Attached': %s 'Online': %s 'IMSI': %s",
	       priv->gprs_attached ? "true" : "false",
	       priv->modem_online ? "true" : "false",
	       priv->imsi);

	if (priv->modem_online == FALSE) {
		reason = "modem 'Online=false'";
	} else if (priv->imsi == NULL && state != NM_MODEM_STATE_ENABLING) {
		reason = "modem not ready";
	} else if (priv->gprs_attached == FALSE) {
		new_state = NM_MODEM_STATE_SEARCHING;
		reason = "modem searching";
	} else {
		new_state = NM_MODEM_STATE_REGISTERED;
		reason = "modem ready";
	}

	if (state != new_state)
		nm_modem_set_state (NM_MODEM (self), new_state, reason);
}

/* Disconnect */
typedef struct {
	NMModemOfono *self;
	GSimpleAsyncResult *result;
	GCancellable *cancellable;
	gboolean warn;
} DisconnectContext;

static void
disconnect_context_complete (DisconnectContext *ctx)
{
	g_simple_async_result_complete_in_idle (ctx->result);
	if (ctx->cancellable)
		g_object_unref (ctx->cancellable);
	g_object_unref (ctx->result);
	g_object_unref (ctx->self);
	g_slice_free (DisconnectContext, ctx);
}

static gboolean
disconnect_context_complete_if_cancelled (DisconnectContext *ctx)
{
	GError *error = NULL;

	if (g_cancellable_set_error_if_cancelled (ctx->cancellable, &error)) {
		g_simple_async_result_take_error (ctx->result, error);
		disconnect_context_complete (ctx);
		return TRUE;
	}

	return FALSE;
}

static gboolean
disconnect_finish (NMModem *self,
                   GAsyncResult *result,
                   GError **error)
{
	return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

static void
disconnect_done (GDBusProxy *proxy,
				 GAsyncResult *result,
				 gpointer user_data)
{
	DisconnectContext *ctx = (DisconnectContext*) user_data;
	NMModemOfono *self = ctx->self;
	GError *error = NULL;

	g_dbus_proxy_call_finish (proxy, result, &error);
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		_LOGD ("disconnect cancelled");
		return;
	}

	if (error) {
		if (ctx->warn)
			_LOGW ("failed to disconnect modem: %s", error->message);
		g_clear_error (&error);
	}

	_LOGD ("modem disconnected");

	update_modem_state (self);
	disconnect_context_complete (ctx);
}

static void
disconnect (NMModem *modem,
            gboolean warn,
            GCancellable *cancellable,
            GAsyncReadyCallback callback,
            gpointer user_data)
{
	NMModemOfono *self = NM_MODEM_OFONO (modem);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	DisconnectContext *ctx;
	NMModemState state = nm_modem_get_state (NM_MODEM (self));

	_LOGD ("warn: %s modem_state: %s",
	       warn ? "TRUE" : "FALSE",
	       nm_modem_state_to_string (state));

	if (state != NM_MODEM_STATE_CONNECTED)
		return;

	ctx = g_slice_new (DisconnectContext);
	ctx->self = g_object_ref (self);
	ctx->warn = warn;

	if (callback) {
		ctx->result = g_simple_async_result_new (G_OBJECT (self),
		                                         callback,
		                                         user_data,
		                                         disconnect);
	}

	ctx->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
	if (disconnect_context_complete_if_cancelled (ctx))
		return;

	nm_modem_set_state (NM_MODEM (self),
	                    NM_MODEM_STATE_DISCONNECTING,
	                    nm_modem_state_to_string (NM_MODEM_STATE_DISCONNECTING));

	g_dbus_proxy_call (priv->context_proxy,
	                   "SetProperty",
	                   g_variant_new ("(sv)",
	                                  "Active",
	                                  g_variant_new ("b", warn)),
	                   G_DBUS_CALL_FLAGS_NONE,
	                   20000,
	                   NULL,
	                   (GAsyncReadyCallback) disconnect_done,
	                   ctx);
}

static void
deactivate_cleanup (NMModem *modem, NMDevice *device)
{
	NMModemOfono *self = NM_MODEM_OFONO (modem);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);

	/* TODO: cancel SimpleConnect() if any */

	g_clear_object (&priv->ip4_config);

	NM_MODEM_CLASS (nm_modem_ofono_parent_class)->deactivate_cleanup (modem, device);
}


static gboolean
check_connection_compatible (NMModem *modem,
                             NMConnection *connection)
{
	NMModemOfono *self = NM_MODEM_OFONO (modem);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	NMSettingConnection *s_con;
	NMSettingGsm *s_gsm;
	const char *uuid;
	const char *id;

	s_con = nm_connection_get_setting_connection (connection);
	g_assert (s_con);

	uuid = nm_connection_get_uuid (connection);
	id = nm_connection_get_id (connection);

	s_gsm = nm_connection_get_setting_gsm (connection);
	if (!s_gsm)
		return FALSE;

	if (!priv->imsi) {
		_LOGW ("skipping %s/%s: no IMSI", uuid, id);
		return FALSE;
	}

	if (strcmp (nm_setting_connection_get_connection_type (s_con), NM_SETTING_GSM_SETTING_NAME)) {
		_LOGD ("skipping %s/%s: not GSM", uuid, id);
		return FALSE;
	}

	if (!g_strrstr (id, "/context")) {
		_LOGD ("skipping %s/%s: unexpected ID", uuid, id);
		return FALSE;
	}

	if (!g_strrstr (id, priv->imsi)) {
		_LOGD ("skipping %s/%s: ID doesn't contain IMSI", uuid, id);
		return FALSE;
	}

	_LOGD ("%s/%s compatible with IMSI %s", uuid, id, priv->imsi);
	return TRUE;
}

static void
handle_sim_property (GDBusProxy *proxy,
                     const char *property,
                     GVariant *v,
                     gpointer user_data)
{
	NMModemOfono *self = NM_MODEM_OFONO (user_data);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);

	if (g_strcmp0 (property, "SubscriberIdentity") == 0 && VARIANT_IS_OF_TYPE_STRING (v)) {
		gsize length;
		const char *value_str = g_variant_get_string (v, &length);

		_LOGD ("SubscriberIdentify found");

		/* Check for empty DBus string value */
		if (length &&
			g_strcmp0 (value_str, "(null)") != 0 &&
			g_strcmp0 (value_str, priv->imsi) != 0) {

			if (priv->imsi != NULL) {
				_LOGW ("SimManager:'SubscriberIdentity' changed: %s", priv->imsi);
				g_free(priv->imsi);
			}

			priv->imsi = g_strdup (value_str);
			update_modem_state (self);
		}
	}
}

static void
sim_property_changed (GDBusProxy *proxy,
                      const char *property,
                      GVariant *v,
                      gpointer user_data)
{
	GVariant *v_child = g_variant_get_child_value (v, 0);

	handle_sim_property (proxy, property, v_child, user_data);
	g_variant_unref (v_child);
}

static void
sim_get_properties_done (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	gs_unref_object NMModemOfono *self = NM_MODEM_OFONO (user_data);
	GError *error = NULL;
	GVariant *v_properties, *v_dict, *v;
	GVariantIter i;
	const char *property;

	v_properties = _nm_dbus_proxy_call_finish (proxy,
	                                           result,
	                                           G_VARIANT_TYPE ("(a{sv})"),
	                                           &error);
	if (!v_properties) {
		g_dbus_error_strip_remote_error (error);
		_LOGW ("error getting sim properties: %s", error->message);
		g_error_free (error);
		return;
	}

	_LOGD ("sim v_properties is type: %s", g_variant_get_type_string (v_properties));

	v_dict = g_variant_get_child_value (v_properties, 0);
	if (!v_dict) {
		_LOGW ("error getting sim properties: no v_dict");
		return;
	}

	_LOGD ("sim v_dict is type: %s", g_variant_get_type_string (v_dict));

	/*
	 * TODO:
	 * 1) optimize by looking up properties ( Online, Interfaces ), instead
	 *    of iterating
	 *
	 * 2) reduce code duplication between all of the get_properties_done
	 *    functions in this class.
	 */

	g_variant_iter_init (&i, v_dict);
	while (g_variant_iter_next (&i, "{&sv}", &property, &v)) {
		handle_sim_property (NULL, property, v, self);
		g_variant_unref (v);
	}

	g_variant_unref (v_dict);
	g_variant_unref (v_properties);
}

static void
handle_sim_iface (NMModemOfono *self, gboolean found)
{
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);

	_LOGD ("SimManager interface %sfound", found ? "" : "not ");

	if (!found && priv->sim_proxy) {
		_LOGI ("SimManager interface disappeared");
		g_signal_handlers_disconnect_by_data (priv->sim_proxy, NM_MODEM_OFONO (self));
		g_clear_object (&priv->sim_proxy);
		g_clear_pointer (&priv->imsi, g_free);
		update_modem_state (self);
	} else if (found && !priv->sim_proxy) {
		GError *error = NULL;

		_LOGI ("found new SimManager interface");

		priv->sim_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
		                                                 G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES
		                                                 | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
		                                                 NULL, /* GDBusInterfaceInfo */
		                                                 OFONO_DBUS_SERVICE,
		                                                 nm_modem_get_path (NM_MODEM (self)),
		                                                 OFONO_DBUS_INTERFACE_SIM_MANAGER,
		                                                 NULL, /* GCancellable */
		                                                 &error);
		if (priv->sim_proxy == NULL) {
			_LOGW ("failed to create SimManager proxy: %s", error->message);
			g_error_free (error);
			return;
		}

		/* Watch for custom ofono PropertyChanged signals */
		_nm_dbus_signal_connect (priv->sim_proxy,
		                         "PropertyChanged",
		                         G_VARIANT_TYPE ("(sv)"),
		                         G_CALLBACK (sim_property_changed),
		                         self);

		g_dbus_proxy_call (priv->sim_proxy,
		                   "GetProperties",
		                   NULL,
		                   G_DBUS_CALL_FLAGS_NONE,
		                   20000,
		                   NULL,
		                   (GAsyncReadyCallback) sim_get_properties_done,
		                   g_object_ref (self));
	}
}

static void
handle_connman_property (GDBusProxy *proxy,
                         const char *property,
                         GVariant *v,
                         gpointer user_data)
{
	NMModemOfono *self = NM_MODEM_OFONO (user_data);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);

	if (g_strcmp0 (property, "Attached") == 0 && VARIANT_IS_OF_TYPE_BOOLEAN (v)) {
		gboolean attached = g_variant_get_boolean (v);
		gboolean old_attached = priv->gprs_attached;

		_LOGD ("Attached: %s", attached ? "True" : "False");

		if (priv->gprs_attached != attached) {
			priv->gprs_attached = attached;

			_LOGI ("Attached %s -> %s",
			       old_attached ? "true" : "false",
			       attached ? "true" : "false");

			update_modem_state (self);
		}
	}
}

static void
connman_property_changed (GDBusProxy *proxy,
                        const char *property,
                        GVariant *v,
                        gpointer user_data)
{
	GVariant *v_child = g_variant_get_child_value (v, 0);

	handle_connman_property (proxy, property, v_child, user_data);
	g_variant_unref (v_child);
}

static void
connman_get_properties_done (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	gs_unref_object NMModemOfono *self = NM_MODEM_OFONO (user_data);
	GError *error = NULL;
	GVariant *v_properties, *v_dict, *v;
	GVariantIter i;
	const char *property;

	v_properties = _nm_dbus_proxy_call_finish (proxy,
		                                       result,
		                                       G_VARIANT_TYPE ("(a{sv})"),
		                                       &error);
	if (!v_properties) {
		g_dbus_error_strip_remote_error (error);
		_LOGW ("error getting connman properties: %s", error->message);
		g_error_free (error);
		return;
	}

	v_dict = g_variant_get_child_value (v_properties, 0);

	/*
	 * TODO:
	 * 1) optimize by looking up properties ( Online, Interfaces ), instead
	 *    of iterating
	 *
	 * 2) reduce code duplication between all of the get_properties_done
	 *    functions in this class.
	 */

	g_variant_iter_init (&i, v_dict);
	while (g_variant_iter_next (&i, "{&sv}", &property, &v)) {
		handle_connman_property (NULL, property, v, self);
		g_variant_unref (v);
	}

	g_variant_unref (v_dict);
	g_variant_unref (v_properties);
}

static void
handle_connman_iface (NMModemOfono *self, gboolean found)
{
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);

	_LOGD ("ConnectionManager interface %sfound", found ? "" : "not ");

	if (!found && priv->connman_proxy) {
		_LOGI ("ConnectionManager interface disappeared");

		g_signal_handlers_disconnect_by_data (priv->connman_proxy, NM_MODEM_OFONO (self));
		g_clear_object (&priv->connman_proxy);

		/* The connection manager proxy disappeared, we should
		 * consider the modem disabled.
		 */
		priv->gprs_attached = FALSE;

		update_modem_state (self);
	} else if (found && !priv->connman_proxy) {
		GError *error = NULL;

		_LOGI ("found new ConnectionManager interface");

		priv->connman_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
		                                                     G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES
		                                                     | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
		                                                     NULL, /* GDBusInterfaceInfo */
		                                                     OFONO_DBUS_SERVICE,
		                                                     nm_modem_get_path (NM_MODEM (self)),
		                                                     OFONO_DBUS_INTERFACE_CONNECTION_MANAGER,
		                                                     NULL, /* GCancellable */
		                                                     &error);
		if (priv->connman_proxy == NULL) {
			_LOGW ("failed to create ConnectionManager proxy: %s", error->message);
			g_error_free (error);
			return;
		}

		/* Watch for custom ofono PropertyChanged signals */
		_nm_dbus_signal_connect (priv->connman_proxy,
		                         "PropertyChanged",
		                         G_VARIANT_TYPE ("(sv)"),
		                         G_CALLBACK (connman_property_changed),
		                         self);

		g_dbus_proxy_call (priv->connman_proxy,
		                   "GetProperties",
		                   NULL,
		                   G_DBUS_CALL_FLAGS_NONE,
		                   20000,
		                   NULL,
		                   (GAsyncReadyCallback) connman_get_properties_done,
		                   g_object_ref (self));
	}
}

static void
handle_modem_property (GDBusProxy *proxy,
                       const char *property,
                       GVariant *v,
                       gpointer user_data)
{
	NMModemOfono *self = NM_MODEM_OFONO (user_data);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);

	if ((g_strcmp0 (property, "Online") == 0) && VARIANT_IS_OF_TYPE_BOOLEAN (v)) {
		gboolean online = g_variant_get_boolean (v);

		_LOGD ("Online: %s", online ? "True" : "False");

		if (online != priv->modem_online) {
			priv->modem_online = online;
			_LOGI ("modem is now %s", online ? "Online" : "Offline");
			update_modem_state (self);
		}

	} else if ((g_strcmp0 (property, "Interfaces") == 0) && VARIANT_IS_OF_TYPE_STRING_ARRAY (v)) {
		const char **array, **iter;
		gboolean found_connman = FALSE;
		gboolean found_sim = FALSE;

		_LOGD ("Interfaces found");

		array = g_variant_get_strv (v, NULL);
		if (array) {
			for (iter = array; *iter; iter++) {
				if (g_strcmp0 (OFONO_DBUS_INTERFACE_SIM_MANAGER, *iter) == 0)
					found_sim = TRUE;
				else if (g_strcmp0 (OFONO_DBUS_INTERFACE_CONNECTION_MANAGER, *iter) == 0)
					found_connman = TRUE;
			}
			g_free (array);
		}

		handle_sim_iface (self, found_sim);
		handle_connman_iface (self, found_connman);
	}
}

static void
modem_property_changed (GDBusProxy *proxy,
                        const char *property,
                        GVariant *v,
                        gpointer user_data)
{
	GVariant *v_child = g_variant_get_child_value (v, 0);

	handle_modem_property (proxy, property, v_child, user_data);
	g_variant_unref (v_child);
}

static void
modem_get_properties_done (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	gs_unref_object NMModemOfono *self = NM_MODEM_OFONO (user_data);
	GError *error = NULL;
	GVariant *v_properties, *v_dict, *v;
	GVariantIter i;
	const char *property;

	v_properties = _nm_dbus_proxy_call_finish (proxy,
	                                           result,
	                                           G_VARIANT_TYPE ("(a{sv})"),
	                                           &error);
	if (!v_properties) {
		g_dbus_error_strip_remote_error (error);
		_LOGW ("error getting modem properties: %s", error->message);
		g_error_free (error);
		return;
	}

	v_dict = g_variant_get_child_value (v_properties, 0);
	if (!v_dict) {
		_LOGW ("error getting modem properties: no v_dict");
		return;
	}

	/*
	 * TODO:
	 * 1) optimize by looking up properties ( Online, Interfaces ), instead
	 *    of iterating
	 *
	 * 2) reduce code duplication between all of the get_properties_done
	 *    functions in this class.
	 */

	g_variant_iter_init (&i, v_dict);
	while (g_variant_iter_next (&i, "{&sv}", &property, &v)) {
		handle_modem_property (NULL, property, v, self);
		g_variant_unref (v);
	}

	g_variant_unref (v_dict);
	g_variant_unref (v_properties);
}

static void
stage1_prepare_done (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	gs_unref_object NMModemOfono *self = NM_MODEM_OFONO (user_data);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	GError *error = NULL;

	g_clear_pointer (&priv->connect_properties, g_hash_table_destroy);

	g_dbus_proxy_call_finish (proxy, result, &error);
	if (error) {
		_LOGW ("connection failed: %s", error->message);

		nm_modem_emit_prepare_result (NM_MODEM (self), FALSE,
		                              NM_DEVICE_STATE_REASON_MODEM_BUSY);
		/*
		 * FIXME: add code to check for InProgress so that the
		 * connection doesn't continue to try and activate,
		 * leading to the connection being disabled, and a 5m
		 * timeout...
		 */

		g_clear_error (&error);
	}
}

static void
context_property_changed (GDBusProxy *proxy,
                          const char *property,
                          GVariant *v,
                          gpointer user_data)
{
	NMModemOfono *self = NM_MODEM_OFONO (user_data);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	NMPlatformIP4Address addr;
	gboolean ret = FALSE;
	GVariant *v_dict;
	const gchar *s, *addr_s;
	const gchar **array, **iter;
	guint32 address_network, gateway_network;
	guint prefix = 0;

	_LOGD ("PropertyChanged: %s", property);

	/*
	 * TODO: might be a good idea and re-factor this to mimic bluez-device,
	 * ie. have this function just check the key, and call a sub-func to
	 * handle the action.
	 */

	if (g_strcmp0 (property, "Settings") != 0)
		return;

	v_dict = g_variant_get_child_value (v, 0);
	if (!v_dict) {
		_LOGW ("error getting IPv4 Settings: no v_dict");
		goto out;
	}

	_LOGI ("IPv4 static Settings:");

	if (g_variant_lookup (v_dict, "Interface", "&s", &s)) {
		if (s && strlen (s)) {
			_LOGD ("Interface: %s", s);
			g_object_set (self,
			              NM_MODEM_DATA_PORT, g_strdup (s),
			              NM_MODEM_IP4_METHOD, NM_MODEM_IP_METHOD_STATIC,
			              NULL);
		} else {
			_LOGW ("Settings 'Interface'; empty");
			goto out;
		}

	} else {
		_LOGW ("Settings 'Interface' missing");
		goto out;
	}

	/* TODO: verify handling of ip4_config; check other places it's used... */
	g_clear_object (&priv->ip4_config);

	memset (&addr, 0, sizeof (addr));

	/*
	 * TODO:
	 *
	 * NM 1.2 changed the NMIP4Config constructor to take an ifindex
	 * ( vs. void pre 1.2 ), to tie config instance to a specific
	 * platform interface.
	 *
	 * This doesn't work for ofono, as the devices are created
	 * dynamically ( eg. ril_0, ril_1 ) in NMModemManager.  The
	 * device created doesn't really map directly to a platform
	 * link.  The closest would be one of the devices owned by
	 * rild ( eg. ccmin0 ), which is passed to us above as
	 * 'Interface'.
	 *
	 * This needs discussion with upstream.
	 */
	priv->ip4_config = nm_ip4_config_new (0);

	/* TODO: simply if/else error logic! */

	if (g_variant_lookup (v_dict, "Address", "&s", &addr_s)) {
		_LOGD ("Address: %s", addr_s);

		if (ip_string_to_network_address (addr_s, &address_network)) {
			addr.address = address_network;
			addr.addr_source = NM_IP_CONFIG_SOURCE_WWAN;
		} else {
			_LOGW ("can't convert 'Address' %s to addr", s);
			goto out;
		}

	} else {
		_LOGW ("Settings 'Address' missing");
		goto out;
	}

	if (g_variant_lookup (v_dict, "Netmask", "&s", &s)) {
		_LOGD ("Netmask: %s", s);

		if (s && ip_string_to_network_address (s, &address_network)) {
			prefix = nm_utils_ip4_netmask_to_prefix (address_network);
			if (prefix > 0)
				addr.plen = prefix;
		} else {
			_LOGW ("invalid 'Netmask': %s", s);
			goto out;
		}
	} else {
		_LOGW ("Settings 'Netmask' missing");
		goto out;
	}

	_LOGI ("Address: %s/%d", addr_s, prefix);

	nm_ip4_config_add_address (priv->ip4_config, &addr);

	if (g_variant_lookup (v_dict, "Gateway", "&s", &s)) {
		if (s && ip_string_to_network_address (s, &gateway_network)) {
			_LOGI ("Gateway: %s", s);
			nm_ip4_config_set_gateway (priv->ip4_config, gateway_network);
		} else {
			_LOGW ("invalid 'Gateway': %s", s);
			goto out;
		}
		nm_ip4_config_set_gateway (priv->ip4_config, gateway_network);
	} else {
		_LOGW ("Settings 'Gateway' missing");
		goto out;
	}

	if (g_variant_lookup (v_dict, "DomainNameServers", "^a&s", &array)) {
		if (array) {
			for (iter = array; *iter; iter++) {
				if (ip_string_to_network_address (*iter, &address_network) && address_network > 0) {
					_LOGI ("DNS: %s", *iter);
					nm_ip4_config_add_nameserver (priv->ip4_config, address_network);
				} else {
					_LOGW ("invalid NameServer: %s", *iter);
				}
			}

			if (iter == array) {
				_LOGW ("Settings: 'DomainNameServers': none specified");
				g_free (array);
				goto out;
			}
			g_free (array);
		}
	} else {
		_LOGW ("Settings 'DomainNameServers' missing");
		goto out;
	}

	if (g_variant_lookup (v_dict, "MessageProxy", "&s", &s)) {
		_LOGI ("MessageProxy: %s", s);
		if (s && ip_string_to_network_address (s, &address_network)) {
			NMPlatformIP4Route mms_route;

			mms_route.network = address_network;
			mms_route.plen = 32;
			mms_route.gateway = gateway_network;

			mms_route.metric = 1;

			nm_ip4_config_add_route (priv->ip4_config, &mms_route);
		} else {
			_LOGW ("invalid MessageProxy: %s", s);
		}
	}

	ret = TRUE;

out:
	if (nm_modem_get_state (NM_MODEM (self)) != NM_MODEM_STATE_CONNECTED) {
		_LOGI ("emitting PREPARE_RESULT: %s", ret ? "TRUE" : "FALSE");
		nm_modem_emit_prepare_result (NM_MODEM (self), ret,
		                              ret
		                                  ? NM_DEVICE_STATE_REASON_NONE
		                                  : NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE);
	} else {
		_LOGW ("MODEM_PPP_FAILED");
		nm_modem_emit_ppp_failed (NM_MODEM (self), NM_DEVICE_STATE_REASON_PPP_FAILED);
	}
}

static NMActStageReturn
static_stage3_ip4_config_start (NMModem *modem,
                                NMActRequest *req,
                                NMDeviceStateReason *out_failure_reason)
{
	NMModemOfono *self = NM_MODEM_OFONO (modem);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	GError *error = NULL;

	if (!priv->ip4_config) {
		_LOGD ("IP4 config not ready(?)");
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	_LOGD ("IP4 config is done; setting modem_state -> CONNECTED");
	g_signal_emit_by_name (self, NM_MODEM_IP4_CONFIG_RESULT, priv->ip4_config, error);

	/* Signal listener takes ownership of the IP4Config */
	priv->ip4_config = NULL;

	nm_modem_set_state (NM_MODEM (self),
	                    NM_MODEM_STATE_CONNECTED,
	                    nm_modem_state_to_string (NM_MODEM_STATE_CONNECTED));
	return NM_ACT_STAGE_RETURN_POSTPONE;
}

static void
context_proxy_new_cb (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	gs_unref_object NMModemOfono *self = NM_MODEM_OFONO (user_data);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	GError *error = NULL;

	priv->context_proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
	if (error) {
		_LOGE ("failed to create ofono ConnectionContext DBus proxy: %s", error->message);
		nm_modem_emit_prepare_result (NM_MODEM (self), FALSE,
		                              NM_DEVICE_STATE_REASON_MODEM_BUSY);
		return;
	}

	if (!priv->gprs_attached) {
		nm_modem_emit_prepare_result (NM_MODEM (self), FALSE,
		                              NM_DEVICE_STATE_REASON_MODEM_NO_CARRIER);
		return;
	}

	/* We have an old copy of the settings from a previous activation,
	 * clear it so that we can gate getting the IP config from oFono
	 * on whether or not we have already received them
	 */
	g_clear_object (&priv->ip4_config);

	/* Watch for custom ofono PropertyChanged signals */
	_nm_dbus_signal_connect (priv->context_proxy,
	                         "PropertyChanged",
	                         G_VARIANT_TYPE ("(sv)"),
	                         G_CALLBACK (context_property_changed),
	                         self);

	g_dbus_proxy_call (priv->context_proxy,
	                   "SetProperty",
	                   g_variant_new ("(sv)",
	                                  "Active",
	                                   g_variant_new ("b", TRUE)),
	                   G_DBUS_CALL_FLAGS_NONE,
	                   20000,
	                   NULL,
	                   (GAsyncReadyCallback) stage1_prepare_done,
	                   g_object_ref (self));
}

static void
do_context_activate (NMModemOfono *self)
{
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);

	g_return_if_fail (NM_IS_MODEM_OFONO (self));

	g_clear_object (&priv->context_proxy);
	g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
	                          G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
	                          NULL,
	                          OFONO_DBUS_SERVICE,
	                          priv->context_path,
	                          OFONO_DBUS_INTERFACE_CONNECTION_CONTEXT,
	                          NULL,
	                          (GAsyncReadyCallback) context_proxy_new_cb,
	                          g_object_ref (self));
}

static GHashTable *
create_connect_properties (NMConnection *connection)
{
	NMSettingGsm *setting;
	GHashTable *properties;
	const char *str;

	setting = nm_connection_get_setting_gsm (connection);
	properties = g_hash_table_new (g_str_hash, g_str_equal);

	str = nm_setting_gsm_get_apn (setting);
	if (str)
		g_hash_table_insert (properties, "AccessPointName", g_strdup (str));

	str = nm_setting_gsm_get_username (setting);
	if (str)
		g_hash_table_insert (properties, "Username", g_strdup (str));

	str = nm_setting_gsm_get_password (setting);
	if (str)
		g_hash_table_insert (properties, "Password", g_strdup (str));

	return properties;
}

static NMActStageReturn
act_stage1_prepare (NMModem *modem,
                    NMConnection *connection,
                    NMDeviceStateReason *out_failure_reason)
{
	NMModemOfono *self = NM_MODEM_OFONO (modem);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	const char *context_id;
	char **id = NULL;

	context_id = nm_connection_get_id (connection);
	id = g_strsplit (context_id, "/", 0);
	g_return_val_if_fail (id[2], NM_ACT_STAGE_RETURN_FAILURE);

	_LOGD ("trying %s %s", id[1], id[2]);

	g_free (priv->context_path);
	priv->context_path = g_strdup_printf ("%s/%s",
	                                      nm_modem_get_path (modem),
	                                      id[2]);
	g_strfreev (id);

	if (!priv->context_path) {
		NM_SET_OUT (out_failure_reason, NM_DEVICE_STATE_REASON_GSM_APN_FAILED);
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	if (priv->connect_properties)
		g_hash_table_destroy (priv->connect_properties);

	priv->connect_properties = create_connect_properties (connection);

	_LOGI ("activating context %s", priv->context_path);

	if (nm_modem_get_state (modem) == NM_MODEM_STATE_REGISTERED) {
		do_context_activate (self);
	} else {
		_LOGW ("could not activate context: modem is not registered.");
		NM_SET_OUT (out_failure_reason, NM_DEVICE_STATE_REASON_MODEM_NO_CARRIER);
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	return NM_ACT_STAGE_RETURN_POSTPONE;
}

static void
modem_proxy_new_cb (GDBusProxy *proxy, GAsyncResult *result, gpointer user_data)
{
	gs_unref_object NMModemOfono *self = NM_MODEM_OFONO (user_data);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);
	GError *error = NULL;

	priv->modem_proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
	if (error) {
		_LOGE ("failed to create ofono modem DBus proxy: %s", error->message);
		return;
	}

	/* Watch for custom ofono PropertyChanged signals */
	_nm_dbus_signal_connect (priv->modem_proxy,
	                         "PropertyChanged",
	                         G_VARIANT_TYPE ("(sv)"),
	                         G_CALLBACK (modem_property_changed),
	                         self);

	g_dbus_proxy_call (priv->modem_proxy,
	                   "GetProperties",
	                   NULL,
	                   G_DBUS_CALL_FLAGS_NONE,
	                   20000,
	                   NULL,
	                   (GAsyncReadyCallback) modem_get_properties_done,
	                   g_object_ref (self));
}

/*****************************************************************************/

static void
nm_modem_ofono_init (NMModemOfono *self)
{
}

static void
constructed (GObject *object)
{
	NMModemOfono *self = NM_MODEM_OFONO (object);

	g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
	                          G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
	                          NULL,
	                          OFONO_DBUS_SERVICE,
	                          nm_modem_get_path (NM_MODEM (self)),
	                          OFONO_DBUS_INTERFACE_MODEM,
	                          NULL,
	                          (GAsyncReadyCallback) modem_proxy_new_cb,
	                          g_object_ref (self));
}

NMModem *
nm_modem_ofono_new (const char *path)
{
	gs_free char *basename = NULL;

	g_return_val_if_fail (path != NULL, NULL);

	nm_log_info (LOGD_MB, "ofono: creating new Ofono modem path %s", path);

	/* Use short modem name (not its object path) as the NM device name (which
	 * comes from NM_MODEM_UID)and the device ID.
	 */
	basename = g_path_get_basename (path);

	return (NMModem *) g_object_new (NM_TYPE_MODEM_OFONO,
	                                 NM_MODEM_PATH, path,
	                                 NM_MODEM_UID, basename,
	                                 NM_MODEM_DEVICE_ID, basename,
	                                 NM_MODEM_CONTROL_PORT, "ofono", /* mandatory */
	                                 NM_MODEM_DRIVER, "ofono",
	                                 NM_MODEM_STATE, (int) NM_MODEM_STATE_INITIALIZING,
	                                 NULL);
}

static void
dispose (GObject *object)
{
	NMModemOfono *self = NM_MODEM_OFONO (object);
	NMModemOfonoPrivate *priv = NM_MODEM_OFONO_GET_PRIVATE (self);

	if (priv->connect_properties) {
		g_hash_table_destroy (priv->connect_properties);
		priv->connect_properties = NULL;
	}

	g_clear_object (&priv->ip4_config);

	if (priv->modem_proxy) {
		g_signal_handlers_disconnect_by_data (priv->modem_proxy, NM_MODEM_OFONO (self));
		g_clear_object (&priv->modem_proxy);
	}

	g_clear_object (&priv->connman_proxy);
	g_clear_object (&priv->context_proxy);

	if (priv->sim_proxy) {
		g_signal_handlers_disconnect_by_data (priv->sim_proxy, NM_MODEM_OFONO (self));
		g_clear_object (&priv->sim_proxy);
	}

	g_free (priv->imsi);
	priv->imsi = NULL;

	G_OBJECT_CLASS (nm_modem_ofono_parent_class)->dispose (object);
}

static void
nm_modem_ofono_class_init (NMModemOfonoClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMModemClass *modem_class = NM_MODEM_CLASS (klass);

	object_class->constructed = constructed;
	object_class->dispose = dispose;

	modem_class->get_capabilities = get_capabilities;
	modem_class->disconnect = disconnect;
	modem_class->disconnect_finish = disconnect_finish;
	modem_class->deactivate_cleanup = deactivate_cleanup;
	modem_class->check_connection_compatible = check_connection_compatible;

	modem_class->act_stage1_prepare = act_stage1_prepare;
	modem_class->static_stage3_ip4_config_start = static_stage3_ip4_config_start;
}
