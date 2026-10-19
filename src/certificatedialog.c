/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "config.h"

#include <glib/gi18n.h>
#include "certificate.h"
#include "certificatedialog.h"

struct _CertificateDialog
{
  AdwWindow parent_instance;
  XccContext *context;
  XccUiId id;
  guint64 revision;
  gboolean hidden;
  gboolean submitted;
  char *announcement;
  gboolean announcement_pending;
  GHashTable *candidate_ids;
  GtkLabel *disclosure;
  GtkLabel *reason;
  AdwStatusPage *empty;
  GtkListBox *candidates;
  GtkButton *accept;
  GtkButton *cancel;
};

G_DEFINE_TYPE (CertificateDialog, certificate_dialog, ADW_TYPE_WINDOW)

static void
row_selected (CertificateDialog *self,
              GtkListBoxRow     *row)
{
  if (self->hidden)
    return;
  gtk_widget_set_sensitive (GTK_WIDGET (self->accept),
                            !self->submitted && row != NULL && gtk_widget_is_sensitive (GTK_WIDGET (row)));
}

static void
accept_clicked (CertificateDialog *self)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (self->candidates);
  guint64 *candidate;

  if (self->hidden || self->submitted || row == NULL || !gtk_widget_is_sensitive (GTK_WIDGET (row)))
    return;

  candidate = g_object_get_data (G_OBJECT (row), "candidate-id");
  /* Keep the window open: a stale revision causes the core to refresh it. */
  self->submitted = TRUE;
  gtk_widget_set_sensitive (GTK_WIDGET (self->accept), FALSE);
  xcc_choose (self->context, self->id, self->revision, *candidate);
}

static void
row_activated (CertificateDialog *self,
               GtkListBoxRow     *row)
{
  accept_clicked (self);
}

void
certificate_dialog_hide (CertificateDialog *self)
{
  self->hidden = TRUE;
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

static void
cancel_clicked (CertificateDialog *self)
{
  if (self->hidden)
    return;
  certificate_dialog_hide (self);
  xcc_ui_cancel (self->context, self->id);
}

static gboolean
close_request (GtkWindow *window)
{
  cancel_clicked (CERTIFICATE_DIALOG (window));
  return TRUE;
}

static GtkWidget *
plain_label (const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  return label;
}

static void
announce_state (CertificateDialog *self)
{
  g_autoptr(GtkATContext) context = NULL;

  if (self->hidden || !self->announcement_pending ||
      !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;

  /* GTK_A11Y=none has a stub context without an announce vfunc. */
  context = gtk_accessible_get_at_context (GTK_ACCESSIBLE (self));
  if (context == NULL || g_strcmp0 (G_OBJECT_TYPE_NAME (context), "GtkAtSpiContext") != 0)
    return;

  gtk_accessible_announce (GTK_ACCESSIBLE (self), self->announcement,
                           GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  self->announcement_pending = FALSE;
}

static char *
format_validity (const XccChooserCandidateView *candidate)
{
  g_autoptr(GDateTime) expiry = NULL;
  g_autofree char *date = NULL;

  if (candidate->struct_size < offsetof (XccChooserCandidateView, validity) + sizeof (candidate->validity))
    return g_strdup (candidate->expiry);

  switch (candidate->validity)
    {
    case XCC_VALIDITY_NOT_YET_VALID:
      return g_strdup (_("Not yet valid"));
    case XCC_VALIDITY_VALID:
    case XCC_VALIDITY_EXPIRED:
      if (candidate->not_after > 0)
        expiry = g_date_time_new_from_unix_local (candidate->not_after);
      if (expiry != NULL)
        date = g_date_time_format (expiry, "%x");
      return g_strdup_printf (candidate->validity == XCC_VALIDITY_EXPIRED ?
                              _("Expired on %s") : _("Valid until %s"),
                              date != NULL ? date : _("an unknown date"));
    default:
      return g_strdup (_("Unknown validity date"));
    }
}

static char *
format_key (const XccChooserCandidateView *candidate)
{
  const char *key_type;

  if (candidate->struct_size < offsetof (XccChooserCandidateView, key_size) + sizeof (candidate->key_size))
    return g_strdup (candidate->detail);
  key_type = candidate->key_type;
  if (key_type == NULL || *key_type == '\0')
    key_type = _("Unknown key type");
  else if (g_str_equal (key_type, "unsupported"))
    key_type = _("Unsupported key type");
  return g_strdup_printf (_("%s %u-bit"), key_type, candidate->key_size);
}

void
certificate_dialog_update (CertificateDialog    *self,
                           const XccChooserView *view)
{
  static const char * const purposes[] = {
    N_("Client authentication"), N_("Signing"), N_("Email"), N_("SSH authentication")
  };
  guint64 selected = 0;
  GtkListBoxRow *old = gtk_list_box_get_selected_row (self->candidates);
  GtkWidget *child;
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (self));
  gboolean list_focused = focus != NULL &&
    (focus == GTK_WIDGET (self->candidates) || gtk_widget_is_ancestor (focus, GTK_WIDGET (self->candidates)));
  const char *identity;
  const char *purpose;
  const char *purpose_detail;
  const char *scope;
  g_autofree char *duration = NULL;
  g_autofree char *text = NULL;
  g_autofree char *reason = NULL;
  g_autofree char *announcement = NULL;
  g_autoptr(GHashTable) candidate_ids = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  gboolean new_candidates = FALSE;

  if (self->hidden)
    return;
  if (view->abi_version != XCC_ABI_VERSION || view->struct_size < XCC_CHOOSER_VIEW_V1_SIZE ||
      (unsigned) view->purpose >= G_N_ELEMENTS (purposes))
    {
      certificate_dialog_hide (self);
      xcc_ui_failed (self->context, self->id, XCC_ERROR_NOT_SUPPORTED);
      return;
    }

  if (old != NULL)
    selected = *(guint64 *) g_object_get_data (G_OBJECT (old), "candidate-id");
  self->revision = view->revision;
  self->submitted = FALSE;
  identity = view->identity_level == XCC_IDENTITY_VERIFIED_SANDBOXED ?
    _("Sandboxed application, identity verified by the system") :
    view->identity_level == XCC_IDENTITY_DERIVED_HOST ?
    _("Unsandboxed application. This name is a useful label, not proof of identity.") :
    _("This application could not be identified. Only continue if you know what asked.");
  purpose = _(purposes[view->purpose]);
  switch (view->purpose)
    {
    case XCC_PURPOSE_CLIENT_AUTH:
      purpose_detail = _("The application will be able to authenticate as you until access expires. It cannot read the private key.");
      break;
    case XCC_PURPOSE_SIGNING:
      purpose_detail = _("The application will be able to have data signed with this key until access expires. It cannot read the private key.");
      break;
    case XCC_PURPOSE_EMAIL:
      purpose_detail = _("The application will be able to use this key for secure email until access expires. It cannot read the private key.");
      break;
    case XCC_PURPOSE_SSH:
      purpose_detail = _("The application will be able to authenticate SSH connections as you until access expires. It cannot read the private key.");
      break;
    default:
      g_assert_not_reached ();
    }
  if (view->struct_size < offsetof (XccChooserView, generic_purpose) + sizeof (view->generic_purpose))
    {
      purpose = view->purpose_text;
      purpose_detail = view->purpose_detail;
    }
  else if (view->generic_purpose)
    {
      purpose = _("Use a certificate on your security token");
      purpose_detail = _("The application will be able to use this key until access expires. It cannot read the private key.");
    }
  scope = view->may_sign && view->may_decrypt ? _("It allows signing and decryption.") :
    view->may_decrypt ? _("It allows decryption only.") :
    view->may_sign ? _("It allows signing only.") : _("It allows no operations.");
  duration = g_strdup_printf (ngettext ("Access lasts %u second.", "Access lasts %u seconds.",
                                        view->lifetime_seconds), view->lifetime_seconds);
  text = g_strdup_printf (_("%s\n%s\n%s\nPurpose: %s\n%s\n%s %s\nYou may be asked for your PIN the first time the key is used."),
                          (view->struct_size >= offsetof (XccChooserView, display_flags) + sizeof (view->display_flags) &&
                           (view->display_flags & XCC_DISPLAY_CALLER_FALLBACK)) || !view->caller_name ?
                          _("An unidentified application") : view->caller_name,
                          view->app_id ? view->app_id : "", identity, purpose, purpose_detail, scope, duration);
  gtk_label_set_text (self->disclosure, text);
  reason = g_strdup_printf (_("Reason supplied by the application (untrusted):\n%s"),
                            view->untrusted_reason ? view->untrusted_reason : "");
  gtk_label_set_text (self->reason, reason);
  gtk_widget_set_visible (GTK_WIDGET (self->reason),
                          view->untrusted_reason && *view->untrusted_reason);

  /* Disable the old snapshot before removing it, including its selection. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->accept), FALSE);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->candidates))))
    {
      gtk_widget_set_sensitive (child, FALSE);
      gtk_list_box_remove (self->candidates, child);
    }

  for (size_t i = 0; i < view->n_candidates; i++)
    {
      const XccChooserCandidateView *candidate = xcc_chooser_view_candidate (view, i);
      GtkWidget *row, *box;
      guint64 *id;
      g_autofree char *details = NULL;
      g_autofree char *validity = NULL;
      g_autofree char *key = NULL;
      guint32 display_flags;

      if (candidate == NULL || candidate->abi_version != XCC_ABI_VERSION ||
          candidate->struct_size < XCC_CHOOSER_CANDIDATE_VIEW_V1_SIZE || candidate->id == 0)
        {
          certificate_dialog_hide (self);
          xcc_ui_failed (self->context, self->id, XCC_ERROR_NOT_SUPPORTED);
          return;
        }
      row = gtk_list_box_row_new ();
      box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
      gtk_widget_set_margin_top (box, 12);
      gtk_widget_set_margin_bottom (box, 12);
      gtk_widget_set_margin_start (box, 12);
      gtk_widget_set_margin_end (box, 12);
      validity = format_validity (candidate);
      key = format_key (candidate);
      display_flags = candidate->struct_size >= offsetof (XccChooserCandidateView, display_flags) + sizeof (candidate->display_flags) ?
        candidate->display_flags : 0;
      gtk_box_append (GTK_BOX (box), plain_label (display_flags & XCC_DISPLAY_SUBJECT_FALLBACK ?
                                                 _("Unnamed certificate") : candidate->subject));
      details = g_strdup_printf (_("Issuer: %s\nToken: %s\nReader: %s\n%s\n%s\nFingerprint: %s"),
                                 display_flags & XCC_DISPLAY_ISSUER_FALLBACK ? _("Unnamed issuer") : candidate->issuer,
                                 display_flags & XCC_DISPLAY_TOKEN_FALLBACK ? _("Unnamed token") : candidate->token,
                                 candidate->reader ? candidate->reader : "",
                                 validity, key, candidate->fingerprint);
      gtk_box_append (GTK_BOX (box), plain_label (details));
      if (candidate->invalid)
        {
          GtkWidget *warning = plain_label (_("This certificate is expired or not yet valid"));

          gtk_widget_add_css_class (warning, "warning");
          gtk_box_append (GTK_BOX (box), warning);
        }
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
      id = g_new (guint64, 1);
      *id = candidate->id;
      g_object_set_data_full (G_OBJECT (row), "candidate-id", id, g_free);
      if (self->candidate_ids == NULL || !g_hash_table_contains (self->candidate_ids, id))
        new_candidates = TRUE;
      g_hash_table_add (candidate_ids, g_memdup2 (id, sizeof (*id)));
      /* invalid denotes certificate dates, not a stale token: like the
       * reference UI, retain these certificates with a warning. Removed rows
       * disappear in the core's immediate replacement snapshot. */
      gtk_list_box_append (self->candidates, row);
      if (selected == candidate->id)
        gtk_list_box_select_row (self->candidates, GTK_LIST_BOX_ROW (row));
    }

  gtk_widget_set_visible (GTK_WIDGET (self->empty), view->n_candidates == 0);
  adw_status_page_set_description (self->empty, view->tokens_present ?
    _("No certificates on the inserted tokens match this request.") : NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->candidates), view->n_candidates != 0);
  row_selected (self, gtk_list_box_get_selected_row (self->candidates));
  if (list_focused)
    gtk_widget_grab_focus (view->n_candidates ? GTK_WIDGET (self->candidates) : GTK_WIDGET (self->cancel));

  if (view->n_candidates == 0)
    announcement = g_strdup (view->tokens_present ?
      _("Insert your security token. No certificates on the inserted tokens match this request.") :
      _("Insert your security token"));
  else
    announcement = g_strdup (_("Certificates available on your security tokens"));

  /* IDs catch arrivals even if the candidate count stays the same. Unchanged
   * snapshots and remapping the same state must not repeat announcements. */
  if (g_strcmp0 (self->announcement, announcement) != 0 || new_candidates)
    self->announcement_pending = TRUE;
  g_free (self->announcement);
  self->announcement = g_steal_pointer (&announcement);
  g_clear_pointer (&self->candidate_ids, g_hash_table_unref);
  self->candidate_ids = g_steal_pointer (&candidate_ids);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->candidates),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, self->announcement, -1);
  announce_state (self);
}

static void
certificate_dialog_finalize (GObject *object)
{
  CertificateDialog *self = CERTIFICATE_DIALOG (object);

  g_free (self->announcement);
  g_clear_pointer (&self->candidate_ids, g_hash_table_unref);
  G_OBJECT_CLASS (certificate_dialog_parent_class)->finalize (object);
}

static void
certificate_dialog_class_init (CertificateDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = certificate_dialog_finalize;
  GTK_WINDOW_CLASS (klass)->close_request = close_request;
  gtk_widget_class_set_template_from_resource (widget_class,
    "/org/freedesktop/portal/desktop/gnome/certificatedialog.ui");
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, disclosure);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, reason);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, empty);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, candidates);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, accept);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, cancel);
  gtk_widget_class_bind_template_callback (widget_class, cancel_clicked);
  gtk_widget_class_bind_template_callback (widget_class, accept_clicked);
  gtk_widget_class_bind_template_callback (widget_class, row_selected);
  gtk_widget_class_bind_template_callback (widget_class, row_activated);
}

static void
certificate_dialog_init (CertificateDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
  g_signal_connect_data (self, "map", G_CALLBACK (announce_state), self, NULL,
                         G_CONNECT_AFTER | G_CONNECT_SWAPPED);
}

CertificateDialog *
certificate_dialog_new (XccContext           *context,
                        XccUiId               id,
                        const XccChooserView *view)
{
  CertificateDialog *self = g_object_new (CERTIFICATE_TYPE_DIALOG, NULL);

  self->context = context;
  self->id = id;
  certificate_dialog_update (self, view);
  gtk_window_set_default_widget (GTK_WINDOW (self), GTK_WIDGET (self->accept));
  if (!self->hidden)
    {
      certificate_window_present (GTK_WINDOW (self), view->parent_window, view->activation_token);
      gtk_widget_grab_focus (view->n_candidates ? GTK_WIDGET (self->candidates) : GTK_WIDGET (self->cancel));
    }
  return self;
}
