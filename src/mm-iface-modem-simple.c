/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2009 Novell, Inc.
 * Copyright (C) 2009 - 2011 Red Hat, Inc.
 * Copyright (C) 2011 Google, Inc.
 */

#include <ModemManager.h>
#include <libmm-common.h>

#include "mm-common-simple-properties.h"
#include "mm-bearer-list.h"
#include "mm-sim.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-simple.h"
#include "mm-log.h"

/*****************************************************************************/

typedef enum {
    CONNECTION_STEP_FIRST,
    CONNECTION_STEP_UNLOCK_CHECK,
    CONNECTION_STEP_ENABLE,
    CONNECTION_STEP_ALLOWED_MODE,
    CONNECTION_STEP_REGISTER,
    CONNECTION_STEP_BEARER,
    CONNECTION_STEP_CONNECT,
    CONNECTION_STEP_LAST
} ConnectionStep;

typedef struct {
    MmGdbusModemSimple *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemSimple *self;
    ConnectionStep step;

    /* Expected input properties */
    MMCommonConnectProperties *properties;

    /* Results to set */
    MMBearer *bearer;
} ConnectionContext;

static void
connection_context_free (ConnectionContext *ctx)
{
    g_object_unref (ctx->properties);
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    if (ctx->bearer)
        g_object_unref (ctx->bearer);
    g_free (ctx);
}

static void connection_step (ConnectionContext *ctx);

static void
connect_bearer_ready (MMBearer *bearer,
                      GAsyncResult *res,
                      ConnectionContext *ctx)
{
    GError *error = NULL;

    if (!mm_bearer_connect_finish (bearer, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        connection_context_free (ctx);
        return;
    }

    /* Bearer connected.... all done!!!!! */
    ctx->step++;
    connection_step (ctx);
}

static void
create_bearer_ready (MMIfaceModem *self,
                     GAsyncResult *res,
                     ConnectionContext *ctx)
{
    GError *error = NULL;

    ctx->bearer = mm_iface_modem_create_bearer_finish (self, res, &error);
    if (!ctx->bearer) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        connection_context_free (ctx);
        return;
    }

    /* Bearer available! */
    ctx->step++;
    connection_step (ctx);
}

static void
register_in_network_ready (MMIfaceModem3gpp *self,
                           GAsyncResult *res,
                           ConnectionContext *ctx)
{
    GError *error = NULL;

    if (!mm_iface_modem_3gpp_register_in_network_finish (
            MM_IFACE_MODEM_3GPP (self), res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        connection_context_free (ctx);
        return;
    }

    /* Registered now! */
    ctx->step++;
    connection_step (ctx);
}

static void
set_allowed_modes_ready (MMBaseModem *self,
                         GAsyncResult *res,
                         ConnectionContext *ctx)
{
    GError *error = NULL;

    if (!mm_iface_modem_set_allowed_modes_finish (MM_IFACE_MODEM (self), res, &error)) {
        /* If setting allowed modes is unsupported, keep on */
        if (!g_error_matches (error,
                              MM_CORE_ERROR,
                              MM_CORE_ERROR_UNSUPPORTED)) {
            g_dbus_method_invocation_take_error (ctx->invocation, error);
            connection_context_free (ctx);
            return;
        }
    }

    /* Allowed modes set... almost there! */
    ctx->step++;
    connection_step (ctx);
}

static void
enable_ready (MMBaseModem *self,
              GAsyncResult *res,
              ConnectionContext *ctx)
{
    GError *error = NULL;

    if (!MM_BASE_MODEM_GET_CLASS (self)->enable_finish (MM_BASE_MODEM (self), res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        connection_context_free (ctx);
        return;
    }

    /* Enabling done!, keep on!!! */
    ctx->step++;
    connection_step (ctx);
}

static void
send_pin_ready (MMSim *sim,
                GAsyncResult *res,
                ConnectionContext *ctx)
{
    GError *error = NULL;

    if (!mm_sim_send_pin_finish (sim, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        connection_context_free (ctx);
        return;
    }

    /* Sent pin and unlocked, cool. */
    ctx->step++;
    connection_step (ctx);
}

static void
unlock_check_ready (MMIfaceModem *self,
                    GAsyncResult *res,
                    ConnectionContext *ctx)
{
    GError *error = NULL;
    MMModemLock lock;
    MMSim *sim;

    lock = mm_iface_modem_unlock_check_finish (self, res, &error);
    if (error) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        connection_context_free (ctx);
        return;
    }

    /* If we are already unlocked, go on to next step */
    if (lock == MM_MODEM_LOCK_NONE) {
        ctx->step++;
        connection_step (ctx);
        return;
    }

    /* During simple connect we are only allowed to use SIM PIN */
    if (lock != MM_MODEM_LOCK_SIM_PIN ||
        !mm_common_connect_properties_get_pin (ctx->properties)) {
        GEnumClass *enum_class;
        GEnumValue *value;

        enum_class = G_ENUM_CLASS (g_type_class_ref (MM_TYPE_MODEM_LOCK));
        value = g_enum_get_value (enum_class, lock);
        g_dbus_method_invocation_return_error (
            ctx->invocation,
            MM_CORE_ERROR,
            MM_CORE_ERROR_UNAUTHORIZED,
            "Modem is locked with '%s' code; cannot unlock it",
            value->value_nick);
        g_type_class_unref (enum_class);
        connection_context_free (ctx);
        return;
    }

    /* Try to unlock the modem providing the PIN */
    sim = NULL;
    g_object_get (ctx->self,
                  MM_IFACE_MODEM_SIM, &sim,
                  NULL);
    if (!sim) {
        g_dbus_method_invocation_return_error (
            ctx->invocation,
            MM_CORE_ERROR,
            MM_CORE_ERROR_FAILED,
            "Cannot unlock modem, couldn't get access to the SIM");
        connection_context_free (ctx);
        return;
    }

    mm_sim_send_pin (sim,
                     mm_common_connect_properties_get_pin (ctx->properties),
                     NULL,
                     (GAsyncReadyCallback)send_pin_ready,
                     ctx);
    g_object_unref (sim);
}

static void
connection_step (ConnectionContext *ctx)
{
    switch (ctx->step) {
    case CONNECTION_STEP_FIRST:
        /* Fall down to next step */
        ctx->step++;

    case CONNECTION_STEP_UNLOCK_CHECK:
        mm_info ("Simple connect state (%d/%d): Unlock check",
                 ctx->step, CONNECTION_STEP_LAST);
        mm_iface_modem_unlock_check (MM_IFACE_MODEM (ctx->self),
                                     (GAsyncReadyCallback)unlock_check_ready,
                                     ctx);
        return;

    case CONNECTION_STEP_ENABLE:
        mm_info ("Simple connect state (%d/%d): Enable",
                 ctx->step, CONNECTION_STEP_LAST);
        MM_BASE_MODEM_GET_CLASS (ctx->self)->enable (MM_BASE_MODEM (ctx->self),
                                                     NULL, /* cancellable */
                                                     (GAsyncReadyCallback)enable_ready,
                                                     ctx);
        return;

    case CONNECTION_STEP_ALLOWED_MODE: {
        MMModemMode allowed_modes = MM_MODEM_MODE_ANY;
        MMModemMode preferred_mode = MM_MODEM_MODE_NONE;

        mm_info ("Simple connect state (%d/%d): Allowed mode",
                 ctx->step, CONNECTION_STEP_LAST);

        mm_common_connect_properties_get_allowed_modes (ctx->properties,
                                                        &allowed_modes,
                                                        &preferred_mode);
        mm_iface_modem_set_allowed_modes (MM_IFACE_MODEM (ctx->self),
                                          allowed_modes,
                                          preferred_mode,
                                          (GAsyncReadyCallback)set_allowed_modes_ready,
                                          ctx);
        return;
    }

    case CONNECTION_STEP_REGISTER:
        mm_info ("Simple connect state (%d/%d): Register",
                 ctx->step, CONNECTION_STEP_LAST);
        if (mm_iface_modem_is_3gpp (MM_IFACE_MODEM (ctx->self))) {
            mm_iface_modem_3gpp_register_in_network (
                MM_IFACE_MODEM_3GPP (ctx->self),
                mm_common_connect_properties_get_operator_id (ctx->properties),
                (GAsyncReadyCallback)register_in_network_ready,
                ctx);
            return;
        }
        /* Fall down to next step */
        ctx->step++;

    case CONNECTION_STEP_BEARER: {
        MMCommonBearerProperties *bearer_properties;

        mm_info ("Simple connect state (%d/%d): Bearer",
                 ctx->step, CONNECTION_STEP_LAST);

        bearer_properties = (mm_common_connect_properties_get_bearer_properties (
                                 ctx->properties));
        mm_iface_modem_create_bearer (
            MM_IFACE_MODEM (ctx->self),
            TRUE, /* Try to force bearer creation */
            bearer_properties,
            (GAsyncReadyCallback)create_bearer_ready,
            ctx);
        g_object_unref (bearer_properties);
        return;
    }

    case CONNECTION_STEP_CONNECT:
        mm_info ("Simple connect state (%d/%d): Connect",
                 ctx->step, CONNECTION_STEP_LAST);
        mm_bearer_connect (ctx->bearer,
                           NULL, /* no number given */
                           (GAsyncReadyCallback)connect_bearer_ready,
                           ctx);
        return;

    case CONNECTION_STEP_LAST:
        mm_info ("Simple connect state (%d/%d): All done",
                 ctx->step, CONNECTION_STEP_LAST);
        /* All done, yey! */
        mm_gdbus_modem_simple_complete_connect (ctx->skeleton,
                                                ctx->invocation,
                                                mm_bearer_get_path (ctx->bearer));
        connection_context_free (ctx);
        return;
    }

    g_assert_not_reached ();
}

static gboolean
handle_connect (MmGdbusModemSimple *skeleton,
                GDBusMethodInvocation *invocation,
                GVariant *dictionary,
                MMIfaceModemSimple *self)
{
    GError *error = NULL;
    MMCommonConnectProperties *properties;
    ConnectionContext *ctx;

    properties = mm_common_connect_properties_new_from_dictionary (dictionary, &error);
    if (!properties) {
        g_dbus_method_invocation_take_error (invocation, error);
        return TRUE;
    }

    ctx = g_new0 (ConnectionContext, 1);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);
    ctx->step = CONNECTION_STEP_FIRST;
    ctx->properties = properties;

    /* Start */
    connection_step (ctx);

    return TRUE;
}

/*****************************************************************************/

typedef struct {
    MMIfaceModemSimple *self;
    MmGdbusModemSimple *skeleton;
    GDBusMethodInvocation *invocation;
    gchar *bearer_path;
    GList *bearers;
    MMBearer *current;
} DisconnectionContext;

static void
disconnection_context_free (DisconnectionContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_free (ctx->bearer_path);
    if (ctx->current)
        g_object_unref (ctx->current);
    if (ctx->bearers) {
        g_list_foreach (ctx->bearers, (GFunc)g_object_unref, NULL);
        g_list_free (ctx->bearers);
    }
    g_free (ctx);
}

static void disconnect_next_bearer (DisconnectionContext *ctx);

static void
disconnect_ready (MMBearer *bearer,
                  GAsyncResult *res,
                  DisconnectionContext *ctx)
{
    GError *error = NULL;

    if (!mm_bearer_disconnect_finish (bearer, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        disconnection_context_free (ctx);
        return;
    }

    disconnect_next_bearer (ctx);
}

static void
disconnect_next_bearer (DisconnectionContext *ctx)
{
    if (ctx->current)
        g_clear_object (&ctx->current);

    /* No more bearers? all done! */
    if (!ctx->bearers) {
        mm_gdbus_modem_simple_complete_disconnect (ctx->skeleton,
                                                   ctx->invocation);
        disconnection_context_free (ctx);
        return;
    }

    ctx->current = MM_BEARER (ctx->bearers->data);
    ctx->bearers = g_list_delete_link (ctx->bearers, ctx->bearers);

    mm_bearer_disconnect (ctx->current,
                          (GAsyncReadyCallback)disconnect_ready,
                          ctx);
}

static void
build_connected_bearer_list (MMBearer *bearer,
                             DisconnectionContext *ctx)
{
    if (!ctx->bearer_path ||
        g_str_equal (ctx->bearer_path, mm_bearer_get_path (bearer)))
        ctx->bearers = g_list_prepend (ctx->bearers, g_object_ref (bearer));
}

static gboolean
handle_disconnect (MmGdbusModemSimple *skeleton,
                   GDBusMethodInvocation *invocation,
                   const gchar *bearer_path,
                   MMIfaceModemSimple *self)
{
    MMBearerList *list = NULL;
    DisconnectionContext *ctx;

    g_object_get (self,
                  MM_IFACE_MODEM_BEARER_LIST, &list,
                  NULL);

    ctx = g_new0 (DisconnectionContext, 1);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->self = g_object_ref (self);
    ctx->invocation = g_object_ref (invocation);

    if (bearer_path &&
        bearer_path[0] == '/' &&
        bearer_path[1]) {
        ctx->bearer_path = g_strdup (ctx->bearer_path);
    }

    mm_bearer_list_foreach (list,
                            (MMBearerListForeachFunc)build_connected_bearer_list,
                            ctx);

    if (ctx->bearer_path &&
        !ctx->bearers) {
        g_dbus_method_invocation_return_error (
            invocation,
            MM_CORE_ERROR,
            MM_CORE_ERROR_INVALID_ARGS,
            "Couldn't disconnect bearer '%s': not found",
            ctx->bearer_path);
        disconnection_context_free (ctx);
        return TRUE;
    }

    disconnect_next_bearer (ctx);
    return TRUE;
}

/*****************************************************************************/

static gboolean
handle_get_status (MmGdbusModemSimple *skeleton,
                   GDBusMethodInvocation *invocation,
                   MMIfaceModemSimple *self)
{
    MMCommonSimpleProperties *properties = NULL;
    GVariant *dictionary;

    g_object_get (self,
                  MM_IFACE_MODEM_SIMPLE_STATUS, &properties,
                  NULL);

    dictionary = mm_common_simple_properties_get_dictionary (properties);
    mm_gdbus_modem_simple_complete_get_status (skeleton, invocation, dictionary);
    g_variant_unref (dictionary);

    g_object_unref (properties);
    return TRUE;
}

/*****************************************************************************/

void
mm_iface_modem_simple_initialize (MMIfaceModemSimple *self)
{
    MmGdbusModemSimple *skeleton = NULL;

    g_return_if_fail (MM_IS_IFACE_MODEM_SIMPLE (self));

    /* Did we already create it? */
    g_object_get (self,
                  MM_IFACE_MODEM_SIMPLE_DBUS_SKELETON, &skeleton,
                  NULL);
    if (!skeleton) {
        skeleton = mm_gdbus_modem_simple_skeleton_new ();

        g_object_set (self,
                      MM_IFACE_MODEM_SIMPLE_DBUS_SKELETON, skeleton,
                      NULL);

        /* Handle method invocations */
        g_signal_connect (skeleton,
                          "handle-connect",
                          G_CALLBACK (handle_connect),
                          self);
        g_signal_connect (skeleton,
                          "handle-disconnect",
                          G_CALLBACK (handle_disconnect),
                          self);
        g_signal_connect (skeleton,
                          "handle-get-status",
                          G_CALLBACK (handle_get_status),
                          self);

        /* Finally, export the new interface */
        mm_gdbus_object_skeleton_set_modem_simple (MM_GDBUS_OBJECT_SKELETON (self),
                                                   MM_GDBUS_MODEM_SIMPLE (skeleton));
    }
    g_object_unref (skeleton);
}

void
mm_iface_modem_simple_shutdown (MMIfaceModemSimple *self)
{
    g_return_if_fail (MM_IS_IFACE_MODEM_SIMPLE (self));

    /* Unexport DBus interface and remove the skeleton */
    mm_gdbus_object_skeleton_set_modem_simple (MM_GDBUS_OBJECT_SKELETON (self), NULL);
    g_object_set (self,
                  MM_IFACE_MODEM_SIMPLE_DBUS_SKELETON, NULL,
                  NULL);
}

/*****************************************************************************/

static void
iface_modem_simple_init (gpointer g_iface)
{
    static gboolean initialized = FALSE;

    if (initialized)
        return;

    /* Properties */
    g_object_interface_install_property
        (g_iface,
         g_param_spec_object (MM_IFACE_MODEM_SIMPLE_DBUS_SKELETON,
                              "Simple DBus skeleton",
                              "DBus skeleton for the Simple interface",
                              MM_GDBUS_TYPE_MODEM_SIMPLE_SKELETON,
                              G_PARAM_READWRITE));

    g_object_interface_install_property
        (g_iface,
         g_param_spec_object (MM_IFACE_MODEM_SIMPLE_STATUS,
                              "Simple status",
                              "Compilation of status values",
                              MM_TYPE_COMMON_SIMPLE_PROPERTIES,
                              G_PARAM_READWRITE));

    initialized = TRUE;
}

GType
mm_iface_modem_simple_get_type (void)
{
    static GType iface_modem_simple_type = 0;

    if (!G_UNLIKELY (iface_modem_simple_type)) {
        static const GTypeInfo info = {
            sizeof (MMIfaceModemSimple), /* class_size */
            iface_modem_simple_init,      /* base_init */
            NULL,                  /* base_finalize */
        };

        iface_modem_simple_type = g_type_register_static (G_TYPE_INTERFACE,
                                                          "MMIfaceModemSimple",
                                                          &info,
                                                          0);

        g_type_interface_add_prerequisite (iface_modem_simple_type, MM_TYPE_IFACE_MODEM);
    }

    return iface_modem_simple_type;
}