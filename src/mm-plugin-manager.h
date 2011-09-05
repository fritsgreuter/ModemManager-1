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
 * Copyright (C) 2011 Aleksander Morgado <aleksander@gnu.org>
 */

#ifndef MM_PLUGIN_MANAGER_H
#define MM_PLUGIN_MANAGER_H

#include <glib-object.h>

#define MM_TYPE_PLUGIN_MANAGER            (mm_plugin_manager_get_type ())
#define MM_PLUGIN_MANAGER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_PLUGIN_MANAGER, MMPluginManager))
#define MM_PLUGIN_MANAGER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), MM_TYPE_PLUGIN_MANAGER, MMPluginManagerClass))
#define MM_IS_PLUGIN_MANAGER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_PLUGIN_MANAGER))
#define MM_IS_PLUGIN_MANAGER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), MM_TYPE_PLUGIN_MANAGER))
#define MM_PLUGIN_MANAGER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), MM_TYPE_PLUGIN_MANAGER, MMPluginManagerClass))

typedef struct _MMPluginManagerPrivate MMPluginManagerPrivate;

typedef struct {
    GObject parent;
    MMPluginManagerPrivate *priv;
} MMPluginManager;

typedef struct {
    GObjectClass parent;
} MMPluginManagerClass;

GType mm_plugin_manager_get_type (void);

MMPluginManager *mm_plugin_manager_new (void);

#endif /* MM_PLUGIN_MANAGER_H */
