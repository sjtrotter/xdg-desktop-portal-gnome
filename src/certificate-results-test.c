/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "certificate-results.h"

static void
test_failure_reason (gconstpointer data)
{
  const char *expected = data;
  const char *reason = NULL;
  g_autoptr(GVariant) results = g_variant_ref_sink (error_results (expected));
  g_autoptr(GVariant) legacy = g_variant_lookup_value (results, "error", NULL);

  g_assert_cmpuint (g_variant_n_children (results), ==, 1);
  g_assert_true (g_variant_lookup (results, "reason", "&s", &reason));
  g_assert_cmpstr (reason, ==, expected != NULL ? expected : "");
  g_assert_null (legacy);
}

int
main (int argc, char **argv)
{
  const char *reasons[] = {
    "cancelled", "owner_gone", "backend_gone", "no_such_session",
    "invalid_request", "invalid_purpose", "invalid_filter", "device_error",
    "interaction_required", "no_display", "pin_locked", "token_removed"
  };

  g_test_init (&argc, &argv, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (reasons); i++)
    {
      g_autofree char *path = g_strdup_printf ("/certificate/results/%s", reasons[i]);
      g_test_add_data_func (path, reasons[i], test_failure_reason);
    }
  g_test_add_data_func ("/certificate/results/null", NULL, test_failure_reason);
  return g_test_run ();
}
