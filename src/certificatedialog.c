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
  GtkLabel *heading;
  GtkLabel *app_id;
  GtkLabel *identity;
  GtkLabel *body;
  GtkImage *icon;
  GtkWidget *empty;
  GtkLabel *empty_heading;
  GtkLabel *empty_body;
  GtkLabel *reason;
  GtkListBox *candidates;
  GtkViewport *candidate_viewport;
  GtkButton *accept;
  GtkButton *cancel;
};

G_DEFINE_TYPE (CertificateDialog, certificate_dialog, ADW_TYPE_WINDOW)

static void
candidate_adjustment_changed (CertificateDialog *self,
                              GtkAdjustment     *adjustment)
{
  gboolean scrolls = gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_lower (adjustment) >
    gtk_adjustment_get_page_size (adjustment);

  /* Boxed-list shadows extend beyond the measured rows. Let them paint into
   * the existing margins when all rows fit. Keep the placeholder's clipping. */
  gtk_widget_set_overflow (GTK_WIDGET (self->candidate_viewport),
                           !scrolls && gtk_list_box_get_row_at_index (self->candidates, 0) != NULL ?
                           GTK_OVERFLOW_VISIBLE : GTK_OVERFLOW_HIDDEN);
}

static void
row_selected (CertificateDialog *self,
              GtkListBoxRow     *row)
{
  if (self->hidden)
    return;
  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->candidates));
       child != NULL; child = gtk_widget_get_next_sibling (child))
    {
      GtkCheckButton *radio = g_object_get_data (G_OBJECT (child), "radio");
      if (radio != NULL)
        gtk_check_button_set_active (radio, child == GTK_WIDGET (row));
    }
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
  if (row == gtk_list_box_get_selected_row (self->candidates))
    accept_clicked (self);
}

static gboolean
candidate_is_usable (GtkListBoxRow *row)
{
  return row != NULL && gtk_widget_is_sensitive (GTK_WIDGET (row)) &&
    gtk_widget_get_visible (GTK_WIDGET (row)) && gtk_list_box_row_get_selectable (row);
}

static GtkListBoxRow *
first_usable_candidate (CertificateDialog *self)
{
  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->candidates));
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    if (GTK_IS_LIST_BOX_ROW (child) && candidate_is_usable (GTK_LIST_BOX_ROW (child)))
      return GTK_LIST_BOX_ROW (child);

  return NULL;
}

static GtkListBoxRow *
focused_candidate (CertificateDialog *self)
{
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (self));
  GtkWidget *row = focus != NULL ? gtk_widget_get_ancestor (focus, GTK_TYPE_LIST_BOX_ROW) : NULL;

  return row != NULL && gtk_widget_get_parent (row) == GTK_WIDGET (self->candidates) ?
    GTK_LIST_BOX_ROW (row) : NULL;
}

static void
focus_candidates (CertificateDialog *self,
                  guint64            focused_id)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (self->candidates);

  if (focused_id != 0)
    for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->candidates));
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
      if (GTK_IS_LIST_BOX_ROW (child) && candidate_is_usable (GTK_LIST_BOX_ROW (child)) &&
          *(guint64 *) g_object_get_data (G_OBJECT (child), "candidate-id") == focused_id)
        {
          row = GTK_LIST_BOX_ROW (child);
          break;
        }

  if (!candidate_is_usable (row))
    row = first_usable_candidate (self);

  /* Focusing a row sets GTK's cursor without selecting or activating it. */
  gtk_widget_grab_focus (row != NULL ? GTK_WIDGET (row) : GTK_WIDGET (self->cancel));
}

static gboolean
candidate_key_pressed (CertificateDialog *self,
                       guint              keyval,
                       guint              keycode G_GNUC_UNUSED,
                       GdkModifierType    state)
{
  GtkListBoxRow *row = focused_candidate (self);
  GtkListBoxRow *selected = gtk_list_box_get_selected_row (self->candidates);
  gboolean backwards;
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (self));

  if (focus != NULL && gtk_widget_get_ancestor (focus, GTK_TYPE_POPOVER) != NULL)
    return GDK_EVENT_PROPAGATE;

  /* Return on the details button belongs to that button, not to consent. */
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && row != NULL &&
      gtk_root_get_focus (GTK_ROOT (self)) != GTK_WIDGET (row))
    return GDK_EVENT_PROPAGATE;

  if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK))
    return GDK_EVENT_PROPAGATE;

  switch (keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      if (row == NULL)
        return GDK_EVENT_PROPAGATE;
      if (row == selected)
        row_activated (self, row);
      return GDK_EVENT_STOP;
    case GDK_KEY_space:
      if (row == NULL || focus != GTK_WIDGET (row))
        return GDK_EVENT_PROPAGATE;
      gtk_list_box_select_row (self->candidates, row);
      return GDK_EVENT_STOP;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      backwards = TRUE;
      break;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      backwards = FALSE;
      break;
    default:
      return GDK_EVENT_PROPAGATE;
    }

  if (!candidate_is_usable (row))
    row = candidate_is_usable (selected) ? selected : first_usable_candidate (self);
  else if (row == selected)
    {
      GtkWidget *next = GTK_WIDGET (row);

      while ((next = backwards ? gtk_widget_get_prev_sibling (next) : gtk_widget_get_next_sibling (next)))
        if (GTK_IS_LIST_BOX_ROW (next) && candidate_is_usable (GTK_LIST_BOX_ROW (next)))
          {
            row = GTK_LIST_BOX_ROW (next);
            break;
          }
    }

  /* The first arrow selects the focused row, including a one-row list.
   * Subsequent arrows stay within the list; Tab retains normal traversal. */
  if (row != NULL)
    {
      gtk_widget_grab_focus (GTK_WIDGET (row));
      gtk_list_box_select_row (self->candidates, row);
    }

  return GDK_EVENT_STOP;
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
        /* Translators: Certificate expiry date, as day, abbreviated month and
         * four-digit year. See g_date_time_format() for format specifiers. */
        date = g_date_time_format (expiry, _("%-d %b %Y"));
      return g_strdup_printf (candidate->validity == XCC_VALIDITY_EXPIRED ?
                              _("Expired on %s") : _("Expires %s"),
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
    return g_strdup (_("Unknown key type"));
  key_type = candidate->key_type;
  if (key_type == NULL || *key_type == '\0')
    key_type = _("Unknown key type");
  else if (g_str_equal (key_type, "unsupported"))
    key_type = _("Unsupported key type");
  return g_strdup_printf (_("%s %u-bit"), key_type, candidate->key_size);
}

/* Keep units exact: a non-round lifetime must never promise longer access. */
static char *
format_duration (guint seconds)
{
  g_autofree char *minutes = NULL;
  g_autofree char *remainder = NULL;

  if (seconds != 0 && seconds % 3600 == 0)
    return g_strdup_printf (ngettext ("%u hour", "%u hours", seconds / 3600), seconds / 3600);
  if (seconds >= 60)
    {
      minutes = g_strdup_printf (ngettext ("%u minute", "%u minutes", seconds / 60), seconds / 60);
      if (seconds % 60 == 0)
        return g_steal_pointer (&minutes);
      remainder = g_strdup_printf (ngettext ("%u second", "%u seconds", seconds % 60), seconds % 60);
      return g_strdup_printf (_("%s and %s"), minutes, remainder);
    }
  return g_strdup_printf (ngettext ("%u second", "%u seconds", seconds), seconds);
}

static char *
format_body (const XccChooserView *view)
{
  g_autofree char *duration = format_duration (view->lifetime_seconds);
  g_autofree char *sentence = NULL;
  const char *format;

  /* Original producers supplied their own generic-purpose disclosure. */
  if (view->struct_size < offsetof (XccChooserView, generic_purpose) + sizeof (view->generic_purpose))
    sentence = g_strstrip (g_strdup_printf (_("%s Access lasts %s."), view->purpose_detail ? view->purpose_detail : "", duration));
  else
    {
      if (!view->may_sign && !view->may_decrypt)
        format = _("Access lasts %s. It cannot sign or decrypt with your certificate.");
      else if (view->generic_purpose)
        format = view->may_sign && view->may_decrypt ? _("It can sign and decrypt data for %s.") :
          view->may_decrypt ? _("It can decrypt data for %s.") : _("It can sign data for %s.");
      else if (view->purpose == XCC_PURPOSE_EMAIL)
        format = view->may_sign && view->may_decrypt ? _("It can sign and decrypt email for %s.") :
          view->may_decrypt ? _("It can decrypt email for %s.") : _("It can sign email for %s.");
      else if (!view->may_sign)
        format = _("It can decrypt data for %s.");
      else if (view->purpose == XCC_PURPOSE_CLIENT_AUTH)
        format = view->may_decrypt ? _("It can prove who you are to websites and servers and decrypt data for %s.") :
          _("It can prove who you are to websites and servers for %s.");
      else if (view->purpose == XCC_PURPOSE_SSH)
        format = view->may_decrypt ? _("It can sign in to SSH servers and decrypt data for %s.") :
          _("It can sign in to SSH servers for %s.");
      else
        format = view->may_decrypt ? _("It can sign documents and data and decrypt data for %s.") :
          _("It can sign documents and data for %s.");
      sentence = g_strdup_printf (format, duration);
    }
  return g_steal_pointer (&sentence);
}

static void
radio_toggled (GtkCheckButton *radio,
               GtkListBoxRow  *row)
{
  GtkWidget *list = gtk_widget_get_parent (GTK_WIDGET (row));

  if (gtk_check_button_get_active (radio) && GTK_IS_LIST_BOX (list))
    gtk_list_box_select_row (GTK_LIST_BOX (list), row);
}

static void
add_detail (GtkGrid    *grid,
            int         position,
            const char *name,
            const char *value,
            gboolean    selectable)
{
  GtkWidget *label = gtk_label_new (name);
  GtkWidget *text = gtk_label_new (value ? value : "");

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_set_valign (label, GTK_ALIGN_START);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (text), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars (GTK_LABEL (text), 32);
  gtk_label_set_selectable (GTK_LABEL (text), selectable);
  gtk_grid_attach (grid, label, 0, position, 1, 1);
  gtk_grid_attach (grid, text, 1, position, 1, 1);
}

static GtkWidget *
certificate_details (const XccChooserCandidateView *candidate,
                     guint32                        flags,
                     const char                    *key)
{
  GtkWidget *button = gtk_menu_button_new ();
  GtkWidget *popover = gtk_popover_new ();
  GtkWidget *grid = gtk_grid_new ();
  g_autofree char *validity = format_validity (candidate);

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (button), "help-about-symbolic");
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (button, _("Certificate Details"));
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_widget_set_margin_top (grid, 12);
  gtk_widget_set_margin_bottom (grid, 12);
  gtk_widget_set_margin_start (grid, 12);
  gtk_widget_set_margin_end (grid, 12);
  add_detail (GTK_GRID (grid), 0, _("Subject"), flags & XCC_DISPLAY_SUBJECT_FALLBACK ? _("Unnamed certificate") : candidate->subject, TRUE);
  add_detail (GTK_GRID (grid), 1, _("Issuer"), flags & XCC_DISPLAY_ISSUER_FALLBACK ? _("Unnamed issuer") : candidate->issuer, TRUE);
  add_detail (GTK_GRID (grid), 2, _("Security token"), flags & XCC_DISPLAY_TOKEN_FALLBACK ? _("Unnamed token") : candidate->token, FALSE);
  add_detail (GTK_GRID (grid), 3, _("Reader"), candidate->reader, FALSE);
  add_detail (GTK_GRID (grid), 4, _("Key"), key, FALSE);
  add_detail (GTK_GRID (grid), 5, _("Validity"), validity, FALSE);
  add_detail (GTK_GRID (grid), 6, _("Fingerprint"), candidate->fingerprint, TRUE);
  gtk_popover_set_child (GTK_POPOVER (popover), grid);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), popover);
  return button;
}

void
certificate_dialog_update (CertificateDialog    *self,
                           const XccChooserView *view)
{
  static const char * const headings[] = {
    N_("%s wants to identify you with a certificate"), N_("%s wants to sign with your certificate"),
    N_("%s wants to use your email certificate"), N_("%s wants to use your certificate for SSH")
  };
  guint64 selected = 0;
  guint64 focused_id = 0;
  GtkListBoxRow *old = gtk_list_box_get_selected_row (self->candidates);
  GtkListBoxRow *focused_row = focused_candidate (self);
  GtkWidget *child;
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (self));
  gboolean list_focused = focus != NULL &&
    (focus == GTK_WIDGET (self->candidates) || gtk_widget_is_ancestor (focus, GTK_WIDGET (self->candidates)));
  gboolean had_usable_candidates = first_usable_candidate (self) != NULL;
  const char *caller;
  gboolean unknown;
  g_autofree char *body = NULL;
  g_autofree char *reason = NULL;
  GtkCheckButton *group = NULL;
  g_autofree char *text = NULL;
  g_autofree char *announcement = NULL;
  g_autoptr(GHashTable) candidate_ids = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  gboolean new_candidates = FALSE;

  if (self->hidden)
    return;
  if (view->abi_version != XCC_ABI_VERSION || view->struct_size < XCC_CHOOSER_VIEW_V1_SIZE ||
      (unsigned) view->purpose >= G_N_ELEMENTS (headings))
    {
      certificate_dialog_hide (self);
      xcc_ui_failed (self->context, self->id, XCC_ERROR_NOT_SUPPORTED);
      return;
    }

  if (old != NULL)
    selected = *(guint64 *) g_object_get_data (G_OBJECT (old), "candidate-id");
  if (focused_row != NULL)
    focused_id = *(guint64 *) g_object_get_data (G_OBJECT (focused_row), "candidate-id");
  self->revision = view->revision;
  self->submitted = FALSE;
  unknown = view->identity_level == XCC_IDENTITY_UNKNOWN || !view->caller_name || !*view->caller_name ||
    (view->struct_size >= offsetof (XccChooserView, display_flags) + sizeof (view->display_flags) &&
     (view->display_flags & XCC_DISPLAY_CALLER_FALLBACK));
  caller = unknown ? _("An unknown app") : view->caller_name;
  text = g_strdup_printf (view->struct_size >= offsetof (XccChooserView, generic_purpose) + sizeof (view->generic_purpose) && view->generic_purpose ?
                          _("%s wants to use a certificate") : _(headings[view->purpose]), caller);
  body = format_body (view);
  gtk_label_set_text (self->heading, text);
  gtk_label_set_text (self->app_id, !unknown && view->app_id != NULL ? view->app_id : "");
  gtk_widget_set_visible (GTK_WIDGET (self->app_id), *gtk_label_get_text (self->app_id));
  gtk_label_set_text (self->body, body);
  gtk_label_set_text (self->identity, unknown ?
                      _("This app could not be identified. Continue only if you started this.") :
                      view->identity_level == XCC_IDENTITY_DERIVED_HOST ? _("The app name is reported by the app.") : "");
  gtk_widget_set_visible (GTK_WIDGET (self->identity), *gtk_label_get_text (self->identity));
  if (unknown)
    gtk_widget_add_css_class (GTK_WIDGET (self->identity), "warning");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self->identity), "warning");
  if (view->untrusted_reason && *view->untrusted_reason)
    reason = unknown ? g_strdup_printf (_("The app says “%s”"), view->untrusted_reason) :
      g_strdup_printf (_("%s says “%s”"), caller, view->untrusted_reason);
  gtk_label_set_text (self->reason, reason ? reason : "");
  gtk_widget_set_visible (GTK_WIDGET (self->reason), reason != NULL);

  /* Disable the old snapshot before removing it, including its selection. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->accept), FALSE);
  child = gtk_widget_get_first_child (GTK_WIDGET (self->candidates));
  while (child != NULL)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      if (GTK_IS_LIST_BOX_ROW (child))
        {
          gtk_widget_set_sensitive (child, FALSE);
          gtk_list_box_remove (self->candidates, child);
        }
      child = next;
    }

  for (size_t i = 0; i < view->n_candidates; i++)
    {
      const XccChooserCandidateView *candidate = xcc_chooser_view_candidate (view, i);
      GtkWidget *row;
      GtkWidget *radio;
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
      row = adw_action_row_new ();
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_action_row_set_title_lines (ADW_ACTION_ROW (row), 2);
      adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 1);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), TRUE);
      validity = format_validity (candidate);
      key = format_key (candidate);
      display_flags = candidate->struct_size >= offsetof (XccChooserCandidateView, display_flags) + sizeof (candidate->display_flags) ?
        candidate->display_flags : 0;
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), display_flags & XCC_DISPLAY_SUBJECT_FALLBACK ?
                                     _("Unnamed certificate") : candidate->subject);
      details = g_strdup_printf (_("%s · %s"),
                                 display_flags & XCC_DISPLAY_ISSUER_FALLBACK ? _("Unnamed issuer") : candidate->issuer,
                                 validity);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), details);
      radio = gtk_check_button_new ();
      gtk_widget_set_focusable (radio, FALSE);
      if (group != NULL)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (radio), group);
      else
        group = GTK_CHECK_BUTTON (radio);
      g_object_set_data (G_OBJECT (row), "radio", radio);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), radio);
      adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), radio);
      g_signal_connect (radio, "toggled", G_CALLBACK (radio_toggled), row);
      if (candidate->invalid)
        {
          gboolean has_validity = candidate->struct_size >= offsetof (XccChooserCandidateView, validity) + sizeof (candidate->validity);
          GtkWidget *warning = gtk_label_new (has_validity && candidate->validity == XCC_VALIDITY_NOT_YET_VALID ?
            _("Not yet valid") : has_validity && candidate->validity == XCC_VALIDITY_EXPIRED ? _("Expired") :
            _("Check certificate dates"));

          gtk_widget_add_css_class (warning, "error");
          gtk_widget_add_css_class (warning, "caption");
          adw_action_row_add_suffix (ADW_ACTION_ROW (row), warning);
        }
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), certificate_details (candidate, display_flags, key));
      id = g_new (guint64, 1);
      *id = candidate->id;
      g_object_set_data_full (G_OBJECT (row), "candidate-id", id, g_free);
      if (self->candidate_ids == NULL || !g_hash_table_contains (self->candidate_ids, id))
        new_candidates = TRUE;
      g_hash_table_add (candidate_ids, g_memdup2 (id, sizeof (*id)));
      /* Certificates with invalid dates remain selectable and show a warning. */
      gtk_list_box_append (self->candidates, row);
      if (selected == candidate->id)
        gtk_list_box_select_row (self->candidates, GTK_LIST_BOX_ROW (row));
    }

  candidate_adjustment_changed (self, gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (self->candidate_viewport)));
  gtk_label_set_text (self->empty_heading, view->tokens_present ?
                      _("No usable certificates on this token") : _("Insert your security token"));
  gtk_label_set_text (self->empty_body, view->tokens_present ?
                      _("Try a different security token.") : _("Certificates on it will appear here."));
  row_selected (self, gtk_list_box_get_selected_row (self->candidates));
  if (list_focused || (!had_usable_candidates && first_usable_candidate (self) != NULL))
    focus_candidates (self, focused_id);

  if (view->n_candidates == 0)
    announcement = g_strdup (view->tokens_present ?
      _("No usable certificates on this token. Try a different security token.") :
      _("Insert your security token. Certificates on it will appear here."));
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
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, heading);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, app_id);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, identity);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, body);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, icon);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, reason);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, candidates);
  gtk_widget_class_bind_template_child (widget_class, CertificateDialog, candidate_viewport);
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
  GtkEventController *keys;
  GtkWidget *empty_icon;
  GtkAdjustment *adjustment;

  gtk_widget_init_template (GTK_WIDGET (self));
  adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (self->candidate_viewport));
  g_signal_connect_object (adjustment, "changed", G_CALLBACK (candidate_adjustment_changed),
                           self, G_CONNECT_SWAPPED);
  candidate_adjustment_changed (self, adjustment);
  if (!gtk_icon_theme_has_icon (gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (self))), "auth-smartcard-symbolic"))
    gtk_image_set_from_icon_name (self->icon, "dialog-password-symbolic");
  self->empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top (self->empty, 24);
  gtk_widget_set_margin_bottom (self->empty, 24);
  gtk_widget_set_size_request (self->empty, -1, 96);
  gtk_widget_set_valign (self->empty, GTK_ALIGN_CENTER);
  empty_icon = gtk_image_new_from_icon_name ("auth-smartcard-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (empty_icon), 32);
  gtk_widget_add_css_class (empty_icon, "dim-label");
  if (!gtk_icon_theme_has_icon (gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (self))), "auth-smartcard-symbolic"))
    gtk_image_set_from_icon_name (GTK_IMAGE (empty_icon), "dialog-password-symbolic");
  self->empty_heading = GTK_LABEL (gtk_label_new (NULL));
  self->empty_body = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->empty_heading, TRUE);
  gtk_label_set_justify (self->empty_heading, GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap (self->empty_body, TRUE);
  gtk_label_set_justify (self->empty_body, GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (self->empty_heading), "heading");
  gtk_widget_add_css_class (GTK_WIDGET (self->empty_body), "dim-label");
  gtk_box_append (GTK_BOX (self->empty), empty_icon);
  gtk_box_append (GTK_BOX (self->empty), GTK_WIDGET (self->empty_heading));
  gtk_box_append (GTK_BOX (self->empty), GTK_WIDGET (self->empty_body));
  gtk_list_box_set_placeholder (self->candidates, self->empty);
  keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect_swapped (keys, "key-pressed", G_CALLBACK (candidate_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
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
      focus_candidates (self, 0);
      certificate_window_present (GTK_WINDOW (self), view->parent_window, view->activation_token);
    }
  return self;
}
