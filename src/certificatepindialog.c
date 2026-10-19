/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include "certificate.h"
#include "certificatepindialog.h"

struct _CertificatePinDialog
{
  AdwWindow parent_instance;
  XccContext *context;
  XccUiId id;
  guint64 warning_id;
  gboolean protected_path;
  gboolean busy;
  gboolean submitted;
  gboolean locked;
  gboolean hidden;
  gboolean ready;
  GtkLabel *description;
  GtkLabel *hint;
  GtkLabel *status;
  GtkWidget *protected_note;
  GtkPasswordEntry *entry;
  GtkButton *submit;
  GtkButton *cancel;
  GtkSpinner *spinner;
};

G_DEFINE_TYPE (CertificatePinDialog, certificate_pin_dialog, ADW_TYPE_WINDOW)

/* GTK_A11Y=none has a stub context without an announce vfunc. */
static void
announce_status (CertificatePinDialog *self)
{
  g_autoptr(GtkATContext) context = NULL;
  g_autofree char *text = NULL;

  if (!gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;
  context = gtk_accessible_get_at_context (GTK_ACCESSIBLE (self));
  if (context == NULL || g_strcmp0 (G_OBJECT_TYPE_NAME (context), "GtkAtSpiContext") != 0)
    return;
  text = g_strdup_printf ("%s %s", gtk_label_get_text (self->hint), gtk_label_get_text (self->status));
  gtk_accessible_announce (GTK_ACCESSIBLE (self), text, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
}

static void
update_controls (CertificatePinDialog *self)
{
  gboolean enabled = self->ready && !self->busy && !self->submitted && !self->locked && !self->hidden;

  gtk_widget_set_sensitive (GTK_WIDGET (self->entry), enabled);
  gtk_widget_set_sensitive (GTK_WIDGET (self->submit), enabled);
  gtk_widget_set_visible (GTK_WIDGET (self->submit), !self->protected_path || self->warning_id != 0);
  gtk_widget_set_visible (GTK_WIDGET (self->spinner), self->busy || self->submitted);
  gtk_spinner_set_spinning (self->spinner, self->busy || self->submitted);
}

static void
submit_clicked (CertificatePinDialog *self)
{
  XccSecret *secret;
  const char *text;
  size_t capacity, length;
  uint8_t *data;

  if (!self->ready || self->hidden || self->busy || self->submitted || self->locked)
    return;
  if (self->protected_path)
    {
      guint64 warning_id = self->warning_id;

      if (warning_id == 0)
        return;
      self->warning_id = 0;
      self->submitted = TRUE;
      update_controls (self);
      /* Only this explicit Continue action spends the final protected attempt. */
      xcc_warning_reply (self->context, self->id, warning_id, TRUE);
      return;
    }

  /* Borrow directly from GTK's secure entry buffer. No intermediate string,
   * property, signal argument or log may contain the PIN. */
  text = gtk_editable_get_text (GTK_EDITABLE (self->entry));
  length = strlen (text);
  secret = xcc_secret_new ();
  if (secret != NULL)
    {
      data = xcc_secret_data (secret, &capacity);
      if (length < capacity && length <= 511)
        memcpy (data, text, length);
    }
  gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
  if (secret == NULL || !xcc_secret_set_length (secret, length))
    {
      xcc_secret_free (secret);
      gtk_label_set_text (self->status, length == 0 ? _("Enter the PIN for this token.") :
                          length > 511 ? _("PIN exceeds the supported length.") :
                          _("The PIN could not be held in secure memory."));
      return;
    }

  self->submitted = TRUE;
  update_controls (self);
  xcc_pin_submit (self->context, self->id, secret); /* consumes */
}

void
certificate_pin_dialog_hide (CertificatePinDialog *self)
{
  self->hidden = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

static void
cancel_clicked (CertificatePinDialog *self)
{
  if (self->hidden)
    return;
  certificate_pin_dialog_hide (self);
  if (self->warning_id != 0)
    {
      xcc_warning_reply (self->context, self->id, self->warning_id, FALSE);
      self->warning_id = 0;
    }
  xcc_ui_cancel (self->context, self->id);
}

static gboolean
close_request (GtkWindow *window)
{
  cancel_clicked (CERTIFICATE_PIN_DIALOG (window));
  return TRUE;
}

static const char *
translated_status (const XccPinView *view)
{
  if (view->struct_size < offsetof (XccPinView, status_code) + sizeof (view->status_code))
    return view->status;
  switch (view->status_code)
    {
    case XCC_PIN_STATUS_INCORRECT:
      return _("That PIN was not accepted. Try again.");
    case XCC_PIN_STATUS_REQUIRED:
      return _("Enter the PIN for this token.");
    case XCC_PIN_STATUS_TOO_LONG:
      return _("PIN exceeds the supported length.");
    case XCC_PIN_STATUS_MEMORY_UNAVAILABLE:
      return _("The PIN could not be held in secure memory.");
    default:
      return _("The PIN could not be submitted. Try again.");
    }
}

static const char *
translated_purpose (const XccPinView *view)
{
  if (view->struct_size < offsetof (XccPinView, generic_purpose) + sizeof (view->generic_purpose))
    return view->purpose_display ? view->purpose_display : _("Use a certificate");
  if (!view->generic_purpose)
    switch (view->purpose)
      {
      case XCC_PURPOSE_CLIENT_AUTH:
        return _("Authenticate to a server");
      case XCC_PURPOSE_SIGNING:
        return _("Sign a document or data");
      case XCC_PURPOSE_EMAIL:
        return _("Use secure email");
      case XCC_PURPOSE_SSH:
        return _("Authenticate an SSH connection");
      }
  return _("Use a certificate");
}

void
certificate_pin_dialog_update (CertificatePinDialog *self,
                               const XccPinView     *view)
{
  g_autofree char *description = NULL;
  gboolean fresh_input;
  const char *token;

  if (self->hidden)
    return;
  if (view->abi_version != XCC_ABI_VERSION || view->struct_size < XCC_PIN_VIEW_V1_SIZE ||
      (self->ready && self->protected_path != view->protected_path))
    {
      certificate_pin_dialog_hide (self);
      xcc_ui_failed (self->context, self->id, XCC_ERROR_NOT_SUPPORTED);
      return;
    }
  self->protected_path = view->protected_path;
  self->busy = view->busy;
  self->locked = view->locked;
  fresh_input = view->struct_size >= offsetof (XccPinView, status_code) + sizeof (view->status_code) ?
    view->status_code != XCC_PIN_STATUS_NONE : view->status != NULL;
  token = view->struct_size >= offsetof (XccPinView, display_flags) + sizeof (view->display_flags) &&
    (view->display_flags & XCC_DISPLAY_TOKEN_FALLBACK) ? _("Unnamed token") : view->token_label;
  description = g_strdup_printf (_("%s\n%s\nToken: %s\nReader: %s\n%s"),
                                 view->caller_display ? view->caller_display : _("An unidentified application"),
                                 translated_purpose (view),
                                 token ? token : _("Unnamed token"), view->reader ? view->reader : "",
                                 view->context_specific ? _("Authorize this operation") :
                                                          _("Unlock the security token"));
  gtk_label_set_text (self->description, description);
  gtk_label_set_text (self->hint, view->locked ? _("The PIN is locked.") :
                      view->final_try ? _("This is the final PIN attempt. Another incorrect PIN will lock the token.") :
                      view->count_low ? _("The PIN has been entered incorrectly. Only a few attempts may remain.") : "");
  gtk_widget_set_visible (GTK_WIDGET (self->entry), !view->protected_path);
  gtk_widget_set_visible (self->protected_note, view->protected_path);
  gtk_button_set_label (self->submit, view->protected_path ? _("_Continue") : _("_Unlock"));
  if (fresh_input)
    {
      self->submitted = FALSE;
      gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
      gtk_label_set_text (self->status, translated_status (view));
    }
  if (view->busy)
    gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
  update_controls (self);
  if (fresh_input)
    {
      if (!view->protected_path)
        gtk_widget_grab_focus (GTK_WIDGET (self->entry));
      announce_status (self);
    }
}

void
certificate_pin_dialog_warning (CertificatePinDialog *self,
                                const XccWarning     *warning)
{
  if (self->hidden || self->submitted)
    return;
  if (warning->abi_version != XCC_ABI_VERSION || warning->struct_size < sizeof (*warning) ||
      warning->code != XCC_WARNING_FINAL_TRY || !self->protected_path)
    {
      certificate_pin_dialog_hide (self);
      xcc_ui_failed (self->context, self->id, XCC_ERROR_NOT_SUPPORTED);
      return;
    }
  self->warning_id = warning->warning_id;
  gtk_label_set_text (self->hint, _("This is the final PIN attempt. Another incorrect PIN will lock the token."));
  gtk_label_set_text (self->status, _("Continue to enter the PIN on the reader, or Cancel."));
  gtk_widget_add_css_class (GTK_WIDGET (self->submit), "destructive-action");
  update_controls (self);
  gtk_widget_grab_focus (GTK_WIDGET (self->cancel));
  announce_status (self);
  /* Presentation alone never replies to the warning. */
}

static void
mapped (CertificatePinDialog *self)
{
  if (self->ready || self->hidden)
    return;
  self->ready = TRUE;
  update_controls (self);
  gtk_widget_grab_focus (self->protected_path ? GTK_WIDGET (self->cancel) : GTK_WIDGET (self->entry));
  /* For protected paths the core auto-starts NULL-PIN login, or asks for
   * FINAL_TRY confirmation. This acknowledgement also covers typed PINs. */
  xcc_pin_ready (self->context, self->id);
  announce_status (self);
}

static void
certificate_pin_dialog_class_init (CertificatePinDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  GTK_WINDOW_CLASS (klass)->close_request = close_request;
  gtk_widget_class_set_template_from_resource (widget_class,
    "/org/freedesktop/portal/desktop/gnome/certificatepindialog.ui");
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, description);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, hint);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, status);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, protected_note);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, entry);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, submit);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, cancel);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, spinner);
  gtk_widget_class_bind_template_callback (widget_class, submit_clicked);
  gtk_widget_class_bind_template_callback (widget_class, cancel_clicked);
}

static void
certificate_pin_dialog_init (CertificatePinDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
  g_signal_connect_swapped (self, "map", G_CALLBACK (mapped), self);
}

CertificatePinDialog *
certificate_pin_dialog_new (XccContext       *context,
                            XccUiId           id,
                            const XccPinView *view)
{
  CertificatePinDialog *self = g_object_new (CERTIFICATE_TYPE_PIN_DIALOG, NULL);

  self->context = context;
  self->id = id;
  certificate_pin_dialog_update (self, view);
  gtk_window_set_default_widget (GTK_WINDOW (self), GTK_WIDGET (self->submit));
  if (!self->hidden)
    certificate_window_present (GTK_WINDOW (self), view->parent_window, NULL);
  return self;
}
