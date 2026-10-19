/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gio/gio.h>
#include <gtk/gtk.h>

gboolean certificate_init (GDBusConnection *connection, GError **error);
void certificate_shutdown (void);
void certificate_window_present (GtkWindow *window,
                                 const char *parent_window,
                                 const char *activation_token);
