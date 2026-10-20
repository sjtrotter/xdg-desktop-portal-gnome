/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

static inline GVariant *
error_results (const char *code)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "reason",
                        g_variant_new_string (code != NULL ? code : ""));
  return g_variant_builder_end (&builder);
}
