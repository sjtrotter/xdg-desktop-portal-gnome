/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "config.h"

#define GCR_API_SUBJECT_TO_CHANGE 1
#include <gcr/gcr.h>
#include <glib/gi18n.h>
#include <string.h>

#include "certificate.h"
#include "certificatedialog.h"
#include "certificatepindialog.h"

typedef struct
{
  GAsyncReadyCallback callback;
  gpointer data;
  GObject *source;
} Pending;

static Pending open_pending, round_pending, close_pending;
static guint ui_holds;
static guint failures;
static gboolean open_fails;
static XccContext *context = (XccContext *) &ui_holds;
static guint choices;
static XccUiId chosen_id;
static uint64_t chosen_revision, chosen_candidate;

/* Capture chooser consent at the test executable's SDK boundary. */
void
xcc_choose (XccContext *value, XccUiId id, uint64_t revision, uint64_t candidate)
{
  g_assert_true (value == context);
  choices++;
  chosen_id = id;
  chosen_revision = revision;
  chosen_candidate = candidate;
}

void
__wrap_xcc_context_hold_ui (XccContext *value)
{
  g_assert_true (value == context);
  ui_holds++;
}

void
__wrap_xcc_context_release_ui (XccContext *value)
{
  g_assert_true (value == context);
  g_assert_cmpuint (ui_holds, >, 0);
  ui_holds--;
}

void
__wrap_xcc_pin_ready (XccContext *value, XccUiId id)
{
}

void
__wrap_xcc_ui_failed (XccContext *value, XccUiId id, XccErrorCode error)
{
  g_assert_true (value == context);
  g_assert_cmpint (error, ==, XCC_ERROR_NO_DISPLAY);
  failures++;
}

static void
save_pending (Pending            *pending,
              GObject            *source,
              GAsyncReadyCallback callback,
              gpointer            data)
{
  g_assert_null (pending->callback);
  *pending = (Pending) { callback, data, source != NULL ? g_object_ref (source) : NULL };
}

static void
complete_pending (Pending *pending)
{
  Pending saved = *pending;
  g_autoptr(GTask) result = g_task_new (saved.source, NULL, NULL, NULL);

  g_assert_nonnull (saved.callback);
  *pending = (Pending) { 0 };
  g_task_return_boolean (result, TRUE);
  saved.callback (saved.source, G_ASYNC_RESULT (result), saved.data);
  g_clear_object (&saved.source);
}

void
__wrap_gcr_system_prompt_open_for_prompter_async (const char         *name,
                                                int                 timeout,
                                                GCancellable       *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer            data)
{
  g_assert_cmpuint (ui_holds, ==, 1);
  save_pending (&open_pending, NULL, callback, data);
}

GcrPrompt *
__wrap_gcr_system_prompt_open_finish (GAsyncResult *result, GError **error)
{
  g_assert_true (g_task_propagate_boolean (G_TASK (result), NULL));
  if (open_fails)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Test open failure");
      return NULL;
    }
  return GCR_PROMPT (g_object_new (GCR_TYPE_SYSTEM_PROMPT, NULL));
}

void
__wrap_gcr_prompt_password_async (GcrPrompt           *prompt,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             data)
{
  save_pending (&round_pending, G_OBJECT (prompt), callback, data);
}

const char *
__wrap_gcr_prompt_password_finish (GcrPrompt *prompt, GAsyncResult *result, GError **error)
{
  g_assert_true (g_task_propagate_boolean (G_TASK (result), NULL));
  return NULL;
}

void
__wrap_gcr_prompt_confirm_async (GcrPrompt           *prompt,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             data)
{
  save_pending (&round_pending, G_OBJECT (prompt), callback, data);
}

GcrPromptReply
__wrap_gcr_prompt_confirm_finish (GcrPrompt *prompt, GAsyncResult *result, GError **error)
{
  g_assert_true (g_task_propagate_boolean (G_TASK (result), NULL));
  return GCR_PROMPT_REPLY_CANCEL;
}

void
__wrap_gcr_prompt_close (GcrPrompt *prompt)
{
}

void
__wrap_gcr_system_prompt_close_async (GcrSystemPrompt    *prompt,
                                      GCancellable       *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer            data)
{
  save_pending (&close_pending, G_OBJECT (prompt), callback, data);
}

gboolean
__wrap_gcr_system_prompt_close_finish (GcrSystemPrompt *prompt, GAsyncResult *result, GError **error)
{
  g_assert_true (g_task_propagate_boolean (G_TASK (result), NULL));
  return TRUE;
}

static XccPinView
pin_view (void)
{
  XccPinView view = { .struct_size = sizeof (view), .abi_version = XCC_ABI_VERSION };

  view.token_label = "Token";
  view.caller_display = "Example";
  view.purpose = XCC_PURPOSE_SIGNING;
  return view;
}

static XccChooserView
chooser_view (void)
{
  XccChooserView view = { .struct_size = sizeof (view), .abi_version = XCC_ABI_VERSION };

  view.caller_name = "Example";
  view.app_id = "org.example.<b>App</b>";
  view.identity_level = XCC_IDENTITY_VERIFIED_SANDBOXED;
  view.may_sign = TRUE;
  view.lifetime_seconds = 300;
  return view;
}

static GtkLabel *
dialog_label (CertificateDialog *dialog, const char *name)
{
  return GTK_LABEL (gtk_widget_get_template_child (GTK_WIDGET (dialog), CERTIFICATE_TYPE_DIALOG, name));
}

static void
destroy_chooser (CertificateDialog *dialog)
{
  certificate_dialog_hide (dialog);
  gtk_window_destroy (GTK_WINDOW (dialog));
  g_object_unref (dialog);
}

static void
destroy_pin (CertificatePinDialog *dialog)
{
  certificate_pin_dialog_hide (dialog);
  gtk_window_destroy (GTK_WINDOW (dialog));
  g_object_unref (dialog);
}

static void
test_identity (void)
{
  XccChooserView view = chooser_view ();
  CertificateDialog *dialog = g_object_ref_sink (certificate_dialog_new (NULL, 1, &view));
  GtkLabel *app_id = dialog_label (dialog, "app_id");
  GtkWidget *identity = GTK_WIDGET (dialog_label (dialog, "identity"));

  g_assert_true (gtk_widget_get_next_sibling (GTK_WIDGET (dialog_label (dialog, "heading"))) == GTK_WIDGET (app_id));
  g_assert_true (gtk_widget_get_next_sibling (GTK_WIDGET (app_id)) == GTK_WIDGET (dialog_label (dialog, "body")));
  g_assert_cmpstr (gtk_label_get_text (app_id), ==, view.app_id);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (app_id)));
  g_assert_true (gtk_widget_has_css_class (GTK_WIDGET (app_id), "dim-label"));
  g_assert_false (gtk_label_get_use_markup (app_id));
  g_assert_false (gtk_label_get_wrap (app_id));
  g_assert_cmpint (gtk_label_get_ellipsize (app_id), ==, PANGO_ELLIPSIZE_END);
  view.identity_level = XCC_IDENTITY_DERIVED_HOST;
  certificate_dialog_update (dialog, &view);
  g_assert_cmpstr (gtk_label_get_text (app_id), ==, view.app_id);
  view.identity_level = XCC_IDENTITY_UNKNOWN;
  certificate_dialog_update (dialog, &view);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (app_id)));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (identity)), ==,
                   _("This app could not be identified. Continue only if you started this."));
  g_assert_true (gtk_widget_has_css_class (identity, "warning"));
  view.identity_level = XCC_IDENTITY_VERIFIED_SANDBOXED;
  view.display_flags = XCC_DISPLAY_CALLER_FALLBACK;
  certificate_dialog_update (dialog, &view);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (app_id)));
  view.display_flags = 0;
  view.caller_name = "";
  certificate_dialog_update (dialog, &view);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (app_id)));
  view.caller_name = "Example";
  view.app_id = NULL;
  certificate_dialog_update (dialog, &view);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (app_id)));
  view.app_id = "org.example.App";
  view.struct_size = XCC_CHOOSER_VIEW_V1_SIZE;
  certificate_dialog_update (dialog, &view);
  g_assert_cmpstr (gtk_label_get_text (app_id), ==, view.app_id);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (app_id)));
  destroy_chooser (dialog);
}

static void
test_body (void)
{
  XccChooserView view = chooser_view ();
  CertificateDialog *dialog = g_object_ref_sink (certificate_dialog_new (NULL, 1, &view));
  g_autofree char *body = g_strdup (gtk_label_get_text (dialog_label (dialog, "body")));

  view.later_prompt = "SDK later-prompt signal";
  certificate_dialog_update (dialog, &view);
  g_assert_cmpstr (gtk_label_get_text (dialog_label (dialog, "body")), ==, body);
  view.later_prompt = "";
  certificate_dialog_update (dialog, &view);
  g_assert_cmpstr (gtk_label_get_text (dialog_label (dialog, "body")), ==, body);
  view.struct_size = XCC_CHOOSER_VIEW_V1_SIZE;
  view.later_prompt = "SDK later-prompt signal";
  certificate_dialog_update (dialog, &view);
  g_assert_null (strstr (gtk_label_get_text (dialog_label (dialog, "body")), "PIN"));
  destroy_chooser (dialog);
}

static GtkWidget *
find_menu_button (GtkWidget *widget)
{
  if (GTK_IS_MENU_BUTTON (widget))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_menu_button (child);

      if (found != NULL)
        return found;
    }
  return NULL;
}

static void
test_details (void)
{
  XccChooserCandidateView candidate = {
    .struct_size = sizeof (candidate), .abi_version = XCC_ABI_VERSION, .id = 1,
    .subject = "<b>Subject</b>", .issuer = "Issuer", .token = "Token", .reader = "Reader",
    .fingerprint = "AB:CD", .key_type = "RSA", .key_size = 2048,
    .detail = "SDK English summary must not be displayed", .expiry = "SDK English expiry",
    .not_after = 1893456000, .validity = XCC_VALIDITY_VALID
  };
  XccChooserView view = chooser_view ();
  CertificateDialog *dialog;
  const char *names[] = { N_("Subject"), N_("Issuer"), N_("Security token"), N_("Reader"), N_("Key"), N_("Validity"), N_("Fingerprint") };

  view.candidates = &candidate;
  view.n_candidates = 1;
  dialog = g_object_ref_sink (certificate_dialog_new (NULL, 1, &view));
  for (guint state = 0; state < 4; state++)
    {
      GtkWidget *button;
      GtkGrid *grid;
      g_autofree char *validity = NULL;
      g_autofree char *key = g_strdup_printf (_("%s %u-bit"), "RSA", 2048u);

      candidate.validity = state;
      certificate_dialog_update (dialog, &view);
      button = find_menu_button (GTK_WIDGET (gtk_widget_get_template_child (GTK_WIDGET (dialog), CERTIFICATE_TYPE_DIALOG, "candidates")));
      g_assert_nonnull (button);
      grid = GTK_GRID (gtk_popover_get_child (gtk_menu_button_get_popover (GTK_MENU_BUTTON (button))));
      for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (gtk_grid_get_child_at (grid, 0, i))), ==, _(names[i]));
      if (state == XCC_VALIDITY_VALID || state == XCC_VALIDITY_EXPIRED)
        {
          g_autoptr(GDateTime) expiry = g_date_time_new_from_unix_local (candidate.not_after);
          g_autofree char *date = g_date_time_format (expiry, _("%-d %b %Y"));

          validity = g_strdup_printf (state == XCC_VALIDITY_VALID ? _("Expires %s") : _("Expired on %s"), date);
        }
      else
        validity = g_strdup (state == XCC_VALIDITY_NOT_YET_VALID ? _("Not yet valid") : _("Unknown validity date"));
      const char *values[] = { candidate.subject, candidate.issuer, candidate.token, candidate.reader, key, validity, candidate.fingerprint };
      for (guint i = 0; i < G_N_ELEMENTS (values); i++)
        {
          GtkLabel *label = GTK_LABEL (gtk_grid_get_child_at (grid, 1, i));

          g_assert_cmpstr (gtk_label_get_text (label), ==, values[i]);
          g_assert_false (gtk_label_get_use_markup (label));
        }
    }
  candidate.struct_size = XCC_CHOOSER_CANDIDATE_VIEW_V1_SIZE;
  certificate_dialog_update (dialog, &view);
  GtkWidget *button = find_menu_button (GTK_WIDGET (gtk_widget_get_template_child (GTK_WIDGET (dialog), CERTIFICATE_TYPE_DIALOG, "candidates")));
  GtkGrid *grid = GTK_GRID (gtk_popover_get_child (gtk_menu_button_get_popover (GTK_MENU_BUTTON (button))));

  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (gtk_grid_get_child_at (grid, 1, 4))), ==, _("Unknown key type"));
  destroy_chooser (dialog);
}

static GtkWidget *
find_spinner (GtkWidget *widget)
{
  if (GTK_IS_SPINNER (widget))
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_spinner (child);

      if (found != NULL)
        return found;
    }
  return NULL;
}

static void
test_reading (void)
{
  XccChooserView view = chooser_view ();
  CertificateDialog *dialog = g_object_ref_sink (certificate_dialog_new (context, 1, &view));
  GtkWidget *accept = GTK_WIDGET (gtk_widget_get_template_child (GTK_WIDGET (dialog), CERTIFICATE_TYPE_DIALOG, "accept"));
  GtkListBox *candidates = GTK_LIST_BOX (gtk_widget_get_template_child (GTK_WIDGET (dialog), CERTIFICATE_TYPE_DIALOG, "candidates"));
  GtkSpinner *spinner = GTK_SPINNER (find_spinner (GTK_WIDGET (dialog)));
  GtkStack *status;
  GtkWidget *placeholder;
  GtkLabel *heading;
  GtkLabel *body;

  g_assert_nonnull (spinner);
  status = GTK_STACK (gtk_widget_get_parent (GTK_WIDGET (spinner)));
  placeholder = gtk_widget_get_parent (GTK_WIDGET (status));
  heading = GTK_LABEL (gtk_widget_get_next_sibling (GTK_WIDGET (status)));
  body = GTK_LABEL (gtk_widget_get_next_sibling (GTK_WIDGET (heading)));
  g_assert_cmpstr (gtk_label_get_text (heading), ==, _("Insert your security token"));
  g_assert_false (gtk_spinner_get_spinning (spinner));
  for (guint i = 0; i < 2; i++)
    {
      view.tokens_present = i;
      view.tokens_reading = TRUE;
      view.revision++;
      certificate_dialog_update (dialog, &view);
      g_assert_cmpstr (gtk_label_get_text (heading), ==, _("Reading security token…"));
      g_assert_cmpstr (gtk_label_get_text (body), ==, _("This can take a few seconds."));
      g_assert_true (gtk_widget_has_css_class (GTK_WIDGET (body), "dim-label"));
      g_assert_true (gtk_widget_get_child_visible (placeholder));
      g_assert_true (gtk_stack_get_visible_child (status) == GTK_WIDGET (spinner));
      g_assert_true (gtk_spinner_get_spinning (spinner));
      g_assert_false (gtk_widget_get_sensitive (accept));
      gtk_test_accessible_assert_property (candidates, GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           _("Reading security token… This can take a few seconds."));
      certificate_dialog_update (dialog, &view);
      g_assert_false (gtk_widget_get_sensitive (accept));
      view.tokens_reading = FALSE;
      view.revision++;
      certificate_dialog_update (dialog, &view);
      g_assert_cmpstr (gtk_label_get_text (heading), ==, i ?
                       _("No usable certificates on this token") : _("Insert your security token"));
      g_assert_cmpstr (gtk_label_get_text (body), ==, i ?
                       _("Try a different security token.") : _("Certificates on it will appear here."));
      g_assert_false (gtk_spinner_get_spinning (spinner));
    }
  view.tokens_reading = TRUE;
  view.struct_size = offsetof (XccChooserView, tokens_reading);
  certificate_dialog_update (dialog, &view);
  g_assert_cmpstr (gtk_label_get_text (heading), ==, _("No usable certificates on this token"));
  g_assert_false (gtk_spinner_get_spinning (spinner));
  g_assert_false (gtk_widget_get_sensitive (accept));

  XccChooserCandidateView candidate = {
    .struct_size = sizeof (candidate), .abi_version = XCC_ABI_VERSION, .id = 1,
    .subject = "Subject", .issuer = "Issuer", .token = "Token", .key_type = "RSA"
  };
  view.struct_size = sizeof (view);
  view.tokens_reading = FALSE;
  view.candidates = &candidate;
  view.n_candidates = 1;
  view.revision++;
  certificate_dialog_update (dialog, &view);
  GtkListBoxRow *row = gtk_list_box_get_row_at_index (candidates, 0);

  g_assert_nonnull (row);
  g_object_ref (row);
  gtk_list_box_select_row (candidates, row);
  g_assert_true (gtk_widget_get_sensitive (accept));
  gtk_window_present (GTK_WINDOW (dialog));
  while (g_main_context_iteration (NULL, FALSE))
    ;
  GtkMenuButton *details = GTK_MENU_BUTTON (find_menu_button (GTK_WIDGET (row)));
  GtkPopover *popover = gtk_menu_button_get_popover (details);
  XccChooserCandidateView refreshed = candidate;
  const gboolean reading_states[] = { TRUE, FALSE, TRUE, FALSE };

  gtk_menu_button_popup (details);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (popover)));
  choices = 0;
  for (guint i = 0; i < G_N_ELEMENTS (reading_states); i++)
    {
      /* Reading on/off, then reading followed by a fresh completed snapshot. */
      view.tokens_reading = reading_states[i];
      view.revision++;
      if (i == G_N_ELEMENTS (reading_states) - 1)
        view.candidates = &refreshed;
      certificate_dialog_update (dialog, &view);
      g_assert_true (gtk_list_box_get_row_at_index (candidates, 0) == row);
      g_assert_true (gtk_list_box_get_selected_row (candidates) == row);
      g_assert_true (gtk_widget_get_sensitive (accept));
      g_assert_false (gtk_spinner_get_spinning (spinner));
      g_assert_true (gtk_widget_get_visible (GTK_WIDGET (popover)));
      g_signal_emit_by_name (accept, "clicked");
      g_assert_cmpuint (choices, ==, i + 1);
      g_assert_cmpuint (chosen_id, ==, 1);
      g_assert_cmpuint (chosen_revision, ==, view.revision);
      g_assert_cmpuint (chosen_candidate, ==, candidate.id);
      g_assert_false (gtk_widget_get_sensitive (accept));
      g_signal_emit_by_name (accept, "clicked");
      g_assert_cmpuint (choices, ==, i + 1);
    }

  /* A details-only change must still replace the row, even with the same ID. */
  gtk_menu_button_popdown (details);
  view.tokens_reading = TRUE;
  refreshed.fingerprint = "AB:CD";
  view.revision++;
  certificate_dialog_update (dialog, &view);
  GtkListBoxRow *updated = gtk_list_box_get_row_at_index (candidates, 0);
  GtkGrid *grid = GTK_GRID (gtk_popover_get_child (gtk_menu_button_get_popover (
    GTK_MENU_BUTTON (find_menu_button (GTK_WIDGET (updated))))));

  g_assert_true (updated != row);
  g_assert_true (gtk_list_box_get_selected_row (candidates) == updated);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (gtk_grid_get_child_at (grid, 1, 6))), ==, refreshed.fingerprint);
  g_assert_true (gtk_widget_get_sensitive (accept));
  g_object_unref (row);
  destroy_chooser (dialog);
}

static void
test_window_groups (void)
{
  XccChooserView view = chooser_view ();
  XccPinView pin = pin_view ();
  CertificateDialog *first = g_object_ref_sink (certificate_dialog_new (NULL, 1, &view));
  CertificateDialog *second = g_object_ref_sink (certificate_dialog_new (NULL, 2, &view));
  CertificatePinDialog *third = g_object_ref_sink (certificate_pin_dialog_new (NULL, 3, &pin));
  GtkWindow *other = GTK_WINDOW (g_object_ref_sink (gtk_window_new ()));
  GtkWindowGroup *group = gtk_window_get_group (GTK_WINDOW (first));

  g_assert_true (gtk_window_has_group (GTK_WINDOW (first)));
  g_assert_true (gtk_window_has_group (GTK_WINDOW (second)));
  g_assert_true (gtk_window_has_group (GTK_WINDOW (third)));
  g_assert_true (group != gtk_window_get_group (GTK_WINDOW (second)));
  g_assert_true (group != gtk_window_get_group (GTK_WINDOW (third)));
  g_assert_true (group != gtk_window_get_group (other));
  g_assert_true (gtk_window_get_group (GTK_WINDOW (second)) != gtk_window_get_group (GTK_WINDOW (third)));
  destroy_chooser (first);
  destroy_chooser (second);
  destroy_pin (third);
  gtk_window_destroy (other);
  g_object_unref (other);
}

static void
test_pending_open (void)
{
  XccPinView view = pin_view ();
  CertificatePinDialog *dialog = g_object_ref_sink (certificate_pin_system_new (context, 1, &view));

  destroy_pin (dialog);
  g_assert_cmpuint (ui_holds, ==, 1);
  complete_pending (&open_pending);
  g_assert_cmpuint (ui_holds, ==, 1);
  complete_pending (&close_pending);
  g_assert_cmpuint (ui_holds, ==, 0);
}

static void
test_open_failure (void)
{
  XccPinView view = pin_view ();
  CertificatePinDialog *dialog;

  open_fails = TRUE;
  failures = 0;
  dialog = g_object_ref_sink (certificate_pin_system_new (context, 1, &view));
  complete_pending (&open_pending);
  g_assert_cmpuint (failures, ==, 1);
  destroy_pin (dialog);
  g_assert_cmpuint (ui_holds, ==, 0);
  open_fails = FALSE;
}

static void
test_pending_round (gconstpointer data)
{
  guint flags = GPOINTER_TO_UINT (data);
  XccPinView view = pin_view ();
  CertificatePinDialog *dialog;
  Pending *first = flags & 2 ? &close_pending : &round_pending;
  Pending *last = flags & 2 ? &round_pending : &close_pending;

  view.protected_path = flags & 1;
  dialog = g_object_ref_sink (certificate_pin_system_new (context, 1, &view));
  complete_pending (&open_pending);
  g_assert_nonnull (round_pending.callback);
  destroy_pin (dialog);
  complete_pending (first);
  g_assert_cmpuint (ui_holds, ==, 1);
  complete_pending (last);
  g_assert_cmpuint (ui_holds, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  /* Accessible property assertions need an AT context, even when Meson
   * disables the platform accessibility backend with GTK_A11Y=none. */
  g_setenv ("GTK_A11Y", "test", TRUE);
  if (!gtk_init_check ())
    return 77;
  adw_init ();
  g_test_add_func ("/certificate/chooser/identity", test_identity);
  g_test_add_func ("/certificate/chooser/body", test_body);
  g_test_add_func ("/certificate/chooser/details", test_details);
  g_test_add_func ("/certificate/chooser/reading", test_reading);
  g_test_add_func ("/certificate/window-groups", test_window_groups);
  g_test_add_func ("/certificate/system/pending-open", test_pending_open);
  g_test_add_func ("/certificate/system/open-failure", test_open_failure);
  g_test_add_data_func ("/certificate/system/password-first", GUINT_TO_POINTER (0), test_pending_round);
  g_test_add_data_func ("/certificate/system/confirm-first", GUINT_TO_POINTER (1), test_pending_round);
  g_test_add_data_func ("/certificate/system/close-before-password", GUINT_TO_POINTER (2), test_pending_round);
  g_test_add_data_func ("/certificate/system/close-before-confirm", GUINT_TO_POINTER (3), test_pending_round);
  return g_test_run ();
}
