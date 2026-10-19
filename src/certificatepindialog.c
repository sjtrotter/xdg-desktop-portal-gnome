/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Gcr system-prompt lifetime handling adapted from pin-system.c in
 * xdg-desktop-portal-certificate (2026 Stephen J. Trotter).
 */
#include "config.h"

#define GCR_API_SUBJECT_TO_CHANGE 1
#include <gcr/gcr.h>
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
  gboolean system;
  gboolean final_try;
  gboolean count_low;
  gboolean incorrect;
  gboolean round_pending;
  gboolean passive_confirm;
  gboolean prompter_closed;
  guint dismiss_idle;
  gulong close_id;
  GcrPrompt *prompt;
  char *status;
  GtkLabel *heading;
  GtkLabel *token;
  GtkLabel *description;
  GtkLabel *hint;
  GtkLabel *reader_notice;
  GtkPasswordEntry *entry;
  GtkButton *submit;
  GtkButton *cancel;
  GtkSpinner *spinner;
};

G_DEFINE_TYPE (CertificatePinDialog, certificate_pin_dialog, ADW_TYPE_WINDOW)

#define SYSTEM_PROMPTER_NAME "org.gnome.keyring.SystemPrompter"

static void system_release (CertificatePinDialog *self);
static void system_refresh (CertificatePinDialog *self);
static void system_ask (CertificatePinDialog *self);
static void system_confirmed (GObject *source, GAsyncResult *result, gpointer data);
static void cancel_clicked (CertificatePinDialog *self);

static void
set_status (CertificatePinDialog *self,
            const char           *status)
{
  g_free (self->status);
  self->status = g_strdup (status);
  self->incorrect = FALSE;
}

/* Use the same single message for GTK, Gcr and accessibility. A local/status
 * error replaces a token hint; an incorrect PIN combines with its hint. */
static const char *
pin_message (CertificatePinDialog *self)
{
  if (self->locked)
    return _("The PIN is locked. Unlock it with your PUK or ask your administrator.");
  if (self->incorrect)
    return self->final_try ? _("Wrong PIN. Another wrong PIN locks the token.") :
      self->count_low ? _("Wrong PIN. Few attempts may remain.") : _("Wrong PIN. Try again.");
  if (self->status != NULL && *self->status)
    return self->status;
  if (self->final_try)
    return _("Another wrong PIN locks the token.");
  if (self->count_low)
    return _("A wrong PIN was entered before. Few attempts may remain.");
  return "";
}

/* GTK_A11Y=none has a stub context without an announce vfunc. */
static void
announce_status (CertificatePinDialog *self)
{
  g_autoptr(GtkATContext) context = NULL;

  if (!gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;
  context = gtk_accessible_get_at_context (GTK_ACCESSIBLE (self));
  if (context == NULL || g_strcmp0 (G_OBJECT_TYPE_NAME (context), "GtkAtSpiContext") != 0)
    return;
  gtk_accessible_announce (GTK_ACCESSIBLE (self), pin_message (self), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
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
  gtk_label_set_text (self->hint, pin_message (self));
  gtk_widget_remove_css_class (GTK_WIDGET (self->hint), "warning");
  gtk_widget_remove_css_class (GTK_WIDGET (self->hint), "error");
  gtk_widget_add_css_class (GTK_WIDGET (self->hint),
    self->locked || self->final_try || self->incorrect || (self->status != NULL && *self->status) ? "error" : "warning");
  gtk_widget_set_visible (GTK_WIDGET (self->hint), *gtk_label_get_text (self->hint));
  gtk_widget_set_visible (GTK_WIDGET (self->reader_notice), self->protected_path && self->warning_id != 0);
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
      set_status (self, NULL);
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
      set_status (self, length == 0 ? _("Enter your PIN.") :
                  length > 511 ? _("This PIN is too long.") :
                  _("The PIN could not be kept in secure memory."));
      update_controls (self);
      announce_status (self);
      return;
    }

  self->submitted = TRUE;
  set_status (self, NULL);
  update_controls (self);
  xcc_pin_submit (self->context, self->id, secret); /* consumes */
}

void
certificate_pin_dialog_hide (CertificatePinDialog *self)
{
  self->hidden = TRUE;
  if (self->system)
    system_release (self);
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
      return _("Wrong PIN. Try again.");
    case XCC_PIN_STATUS_REQUIRED:
      return _("Enter your PIN.");
    case XCC_PIN_STATUS_TOO_LONG:
      return _("This PIN is too long.");
    case XCC_PIN_STATUS_MEMORY_UNAVAILABLE:
      return _("The PIN could not be kept in secure memory.");
    default:
      return _("The PIN could not be sent to the token. Try again.");
    }
}

static char *
translated_body (const XccPinView *view,
                 const char       *caller)
{
  const char *format = _("%s needs it to use your certificate.");
  g_autofree char *body = NULL;

  if (view->struct_size >= offsetof (XccPinView, generic_purpose) + sizeof (view->generic_purpose) &&
      !view->generic_purpose)
    switch (view->purpose)
      {
      case XCC_PURPOSE_CLIENT_AUTH:
        format = _("%s needs it to identify you to a website.");
        break;
      case XCC_PURPOSE_SIGNING:
        format = _("%s needs it to sign documents and data.");
        break;
      case XCC_PURPOSE_EMAIL:
        format = _("%s needs it to use your email certificate.");
        break;
      case XCC_PURPOSE_SSH:
        format = _("%s needs it to sign in to an SSH server.");
        break;
      }
  body = g_strdup_printf (format, caller);
  return view->context_specific ?
    g_strconcat (body, " ", _("Your security token asks for the PIN before each signature."), NULL) :
    g_steal_pointer (&body);
}

void
certificate_pin_dialog_update (CertificatePinDialog *self,
                               const XccPinView     *view)
{
  g_autofree char *description = NULL;
  gboolean fresh_input;
  gboolean token_fallback;
  gboolean caller_fallback;
  const char *token;
  const char *caller;
  const char *heading;
  g_autofree char *token_line = NULL;

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
  self->final_try = view->final_try;
  self->count_low = view->count_low;
  self->busy = view->busy;
  self->locked = view->locked;
  fresh_input = view->struct_size >= offsetof (XccPinView, status_code) + sizeof (view->status_code) ?
    view->status_code != XCC_PIN_STATUS_NONE : view->status != NULL;
  token_fallback = view->struct_size >= offsetof (XccPinView, display_flags) + sizeof (view->display_flags) &&
    (view->display_flags & XCC_DISPLAY_TOKEN_FALLBACK);
  caller_fallback = view->struct_size >= offsetof (XccPinView, display_flags) + sizeof (view->display_flags) &&
    (view->display_flags & XCC_DISPLAY_CALLER_FALLBACK);
  token = token_fallback || !view->token_label || !*view->token_label ? _("Unnamed token") : view->token_label;
  caller = caller_fallback || !view->caller_display || !*view->caller_display ? _("An unknown app") : view->caller_display;
  heading = view->locked ? _("PIN locked") : view->final_try ?
    (view->protected_path ? _("Last PIN attempt on the reader") : _("Last PIN attempt")) :
    view->protected_path ?
    (view->context_specific ? _("Enter your PIN again on the reader") : _("Enter your PIN on the reader")) :
    (view->context_specific ? _("Enter your PIN again") : _("Enter your PIN"));
  description = translated_body (view, caller);
  token_line = token_fallback && view->reader && *view->reader ?
    g_strdup_printf (_("Security token “%s” in %s"), token, view->reader) :
    g_strdup_printf (_("Security token “%s”"), token);
  gtk_label_set_text (self->heading, heading);
  gtk_label_set_text (self->description, description);
  gtk_label_set_text (self->token, token_line);
  gtk_widget_set_visible (GTK_WIDGET (self->entry), !view->protected_path);
  gtk_button_set_label (self->submit, view->protected_path ? _("_Continue") : _("_Unlock"));
  if (fresh_input)
    {
      self->submitted = FALSE;
      gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
      set_status (self, translated_status (view));
      self->incorrect = view->struct_size >= offsetof (XccPinView, status_code) + sizeof (view->status_code) &&
        view->status_code == XCC_PIN_STATUS_INCORRECT;
    }
  if (view->busy)
    gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
  if (self->system)
    {
      system_refresh (self);
      return;
    }
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
  self->final_try = TRUE;
  gtk_label_set_text (self->heading, self->locked ? _("PIN locked") : _("Last PIN attempt on the reader"));
  if (self->system)
    {
      if (self->prompt == NULL || self->prompter_closed || self->round_pending)
        {
          certificate_pin_dialog_hide (self);
          xcc_ui_failed (self->context, self->id, XCC_ERROR_NO_DISPLAY);
          return;
        }
      system_refresh (self);
      self->round_pending = TRUE;
      gcr_prompt_confirm_async (self->prompt, NULL, system_confirmed, g_object_ref (self));
      return;
    }
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

/* Ported from the standalone pin-system provider. Gcr 4.4 can deliver a
 * completion twice. Claim the result before touching the callback's reference,
 * which may already have been released by the first completion. */
static gboolean
system_round_claim (GAsyncResult *result)
{
  if (g_object_get_data (G_OBJECT (result), "certificate-pin-round-settled"))
    return FALSE;
  g_object_set_data (G_OBJECT (result), "certificate-pin-round-settled", GINT_TO_POINTER (1));
  return TRUE;
}

static void
system_failed (CertificatePinDialog *self,
               XccErrorCode          error)
{
  if (self->hidden)
    return;
  certificate_pin_dialog_hide (self);
  /* Never log Gcr's error text: it is outside our PIN redaction boundary. */
  xcc_ui_failed (self->context, self->id, error);
}

static void
system_confirmed (GObject      *source,
                  GAsyncResult *result,
                  gpointer      data)
{
  CertificatePinDialog *self = data;
  g_autoptr(GError) error = NULL;
  GcrPromptReply reply;
  gboolean passive;
  guint64 warning_id;

  if (!system_round_claim (result))
    return;
  reply = gcr_prompt_confirm_finish (GCR_PROMPT (source), result, &error);
  self->round_pending = FALSE;
  passive = self->passive_confirm;
  self->passive_confirm = FALSE;
  warning_id = self->warning_id;
  if (!self->hidden)
    {
      if (error != NULL)
        system_failed (self, XCC_ERROR_DEVICE);
      else if (reply != GCR_PROMPT_REPLY_CONTINUE)
        cancel_clicked (self);
      else if (!passive && warning_id != 0)
        {
          self->warning_id = 0;
          self->submitted = TRUE;
          set_status (self, NULL);
          xcc_warning_reply (self->context, self->id, warning_id, TRUE);
        }
      /* A passive PIN-pad notice must never submit another login. */
    }
  g_object_unref (self);
}

static void
system_password (GObject      *source,
                 GAsyncResult *result,
                 gpointer      data)
{
  CertificatePinDialog *self = data;
  g_autoptr(GError) error = NULL;
  const char *password;
  XccSecret *secret;
  size_t length, capacity;
  uint8_t *bytes;

  if (!system_round_claim (result))
    return;
  password = gcr_prompt_password_finish (GCR_PROMPT (source), result, &error);
  self->round_pending = FALSE;
  if (self->hidden)
    goto out;
  if (password == NULL)
    {
      if (error != NULL)
        system_failed (self, XCC_ERROR_DEVICE);
      else
        cancel_clicked (self);
      goto out;
    }

  /* Gcr owns secure password storage until the next prompt call. Copy directly
   * into XccSecret, never an ordinary string, property, log or signal argument. */
  length = strlen (password);
  secret = xcc_secret_new ();
  if (secret != NULL)
    {
      bytes = xcc_secret_data (secret, &capacity);
      if (length < capacity && length <= 511)
        memcpy (bytes, password, length);
    }
  if (secret == NULL || !xcc_secret_set_length (secret, length))
    {
      xcc_secret_free (secret);
      set_status (self, length == 0 ? _("Enter your PIN.") :
                  length > 511 ? _("This PIN is too long.") :
                  _("The PIN could not be kept in secure memory."));
      system_refresh (self);
      goto out;
    }
  self->submitted = TRUE;
  set_status (self, NULL);
  xcc_pin_submit (self->context, self->id, secret); /* consumes */
out:
  g_object_unref (self);
}

static void
system_ask (CertificatePinDialog *self)
{
  if (self->hidden || !self->ready || self->prompt == NULL || self->prompter_closed ||
      self->protected_path || self->busy || self->locked || self->submitted || self->round_pending)
    return;
  self->round_pending = TRUE;
  gcr_prompt_password_async (self->prompt, NULL, system_password, g_object_ref (self));
}

static void
system_refresh (CertificatePinDialog *self)
{
  g_autofree char *description = NULL;

  if (self->prompt == NULL || self->prompter_closed || self->hidden)
    return;
  description = g_strdup_printf ("%s\n\n%s%s%s", gtk_label_get_text (self->description), gtk_label_get_text (self->token),
    self->protected_path && self->warning_id != 0 ? "\n\n" : "",
    self->protected_path && self->warning_id != 0 ? gtk_label_get_text (self->reader_notice) : "");
  gcr_prompt_set_title (self->prompt, _("Unlock Security Token"));
  gcr_prompt_set_message (self->prompt, gtk_label_get_text (self->heading));
  gcr_prompt_set_description (self->prompt, description);
  gcr_prompt_set_warning (self->prompt, pin_message (self));
  gcr_prompt_set_continue_label (self->prompt, self->protected_path ? _("_Continue") : _("_Unlock"));
  gcr_prompt_set_cancel_label (self->prompt, _("_Cancel"));
  /* No persistence choice and no caller-window: Shell's prompt is session-modal. */
  gcr_prompt_set_choice_label (self->prompt, NULL);
  gcr_prompt_set_password_new (self->prompt, FALSE);
  system_ask (self);
}

/* Run below Gcr's completion priority so its error/cancel outcome settles first. */
static gboolean
system_dismissed (gpointer data)
{
  CertificatePinDialog *self = data;

  self->dismiss_idle = 0;
  if (!self->hidden)
    cancel_clicked (self);
  return G_SOURCE_REMOVE;
}

static void
system_prompt_closed (GcrPrompt *prompt,
                      gpointer   data)
{
  CertificatePinDialog *self = data;

  if (self->hidden || self->prompter_closed)
    return;
  self->prompter_closed = TRUE;
  self->dismiss_idle = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, system_dismissed,
                                       g_object_ref (self), g_object_unref);
}

static void
system_opened (GObject      *source,
               GAsyncResult *result,
               gpointer      data)
{
  CertificatePinDialog *self = data;
  g_autoptr(GError) error = NULL;

  if (!system_round_claim (result))
    return;
  self->prompt = gcr_system_prompt_open_finish (result, &error);
  if (self->hidden)
    system_release (self);
  else if (self->prompt == NULL)
    system_failed (self, XCC_ERROR_NO_DISPLAY);
  else
    {
      self->close_id = g_signal_connect (self->prompt, "prompt-close", G_CALLBACK (system_prompt_closed), self);
      system_refresh (self);
      if (self->protected_path && !self->final_try && !self->locked)
        {
          self->passive_confirm = TRUE;
          self->round_pending = TRUE;
          gcr_prompt_confirm_async (self->prompt, NULL, system_confirmed, g_object_ref (self));
        }
      /* Only acknowledge once the prompt can receive FINAL_TRY warnings. */
      self->ready = TRUE;
      xcc_pin_ready (self->context, self->id);
      system_ask (self);
    }
  g_object_unref (self);
}

static void
system_closed (GObject      *source,
               GAsyncResult *result,
               gpointer      data)
{
  gcr_system_prompt_close_finish (GCR_SYSTEM_PROMPT (source), result, NULL);
  g_object_unref (data);
}

static void
system_release (CertificatePinDialog *self)
{
  if (self->dismiss_idle != 0)
    {
      g_source_remove (self->dismiss_idle);
      self->dismiss_idle = 0;
    }
  if (self->prompt == NULL)
    return;
  if (self->close_id != 0)
    {
      g_signal_handler_disconnect (self->prompt, self->close_id);
      self->close_id = 0;
    }
  if (!self->prompter_closed)
    {
      self->prompter_closed = TRUE;
      /* Close rounds instead of cancelling them: Gcr 4.4 can complete twice. */
      gcr_prompt_close (self->prompt);
    }
  /* Synchronous close leaks a nested GMainContext per prompt on Gcr 4.4. */
  gcr_system_prompt_close_async (GCR_SYSTEM_PROMPT (self->prompt), NULL,
                                 system_closed, g_object_ref (self));
  g_clear_object (&self->prompt);
}

gboolean
certificate_pin_system_available (GDBusConnection *connection)
{
  g_autoptr(GVariant) reply = NULL;
  gboolean owned = FALSE;

  /* Auto uses an existing owner only; do not activate a fallback prompter. */
  reply = g_dbus_connection_call_sync (connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", "NameHasOwner", g_variant_new ("(s)", SYSTEM_PROMPTER_NAME),
    G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL);
  if (reply != NULL)
    g_variant_get (reply, "(b)", &owned);
  return owned;
}

static void
certificate_pin_dialog_finalize (GObject *object)
{
  CertificatePinDialog *self = CERTIFICATE_PIN_DIALOG (object);

  g_free (self->status);
  if (self->system)
    xcc_context_unref (self->context);
  G_OBJECT_CLASS (certificate_pin_dialog_parent_class)->finalize (object);
}

static void
certificate_pin_dialog_class_init (CertificatePinDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = certificate_pin_dialog_finalize;
  GTK_WINDOW_CLASS (klass)->close_request = close_request;
  gtk_widget_class_set_template_from_resource (widget_class,
    "/org/freedesktop/portal/desktop/gnome/certificatepindialog.ui");
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, heading);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, token);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, description);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, hint);
  gtk_widget_class_bind_template_child (widget_class, CertificatePinDialog, reader_notice);
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

CertificatePinDialog *
certificate_pin_system_new (XccContext       *context,
                           XccUiId           id,
                           const XccPinView *view)
{
  CertificatePinDialog *self = g_object_new (CERTIFICATE_TYPE_PIN_DIALOG, NULL);

  self->system = TRUE;
  /* Async open/round/close callbacks can outlive the SDK's close notification. */
  self->context = xcc_context_ref (context);
  self->id = id;
  certificate_pin_dialog_update (self, view);
  if (!self->hidden)
    gcr_system_prompt_open_for_prompter_async (SYSTEM_PROMPTER_NAME, 60, NULL,
                                              system_opened, g_object_ref (self));
  return self;
}
