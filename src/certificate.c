/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Wire validation/lifetime rules follow xdg-desktop-portal-certificate's
 * certificate-impl.c and session-impl.c (2026 Stephen J. Trotter).
 * Only installed xdp-certificate SDK declarations are used here.
 * Scoped PIN protection and the unresolved crash-dump policy: certificate.md.
 */
#include "config.h"

#include <stddef.h>
#include <string.h>
#include <gxdp.h>
#include <xdp-certificate/core.h>
#include <xdp-certificate/dispatch-glib.h>
#include <xdp-certificate/ui.h>

#include "certificate.h"
#include "certificate-results.h"
#include "certificatedialog.h"
#include "certificatepindialog.h"
#include "xdg-desktop-portal-dbus.h"
#include "utils.h"

#define FRONTEND_NAME "org.freedesktop.portal.Desktop"
#define XCC_INIT(type) { .struct_size = sizeof (type), .abi_version = XCC_ABI_VERSION }

typedef enum
{
  PIN_PROMPT_AUTO,
  PIN_PROMPT_SYSTEM,
  PIN_PROMPT_DIALOG
} PinPromptMode;

typedef struct
{
  int refs;
  char *path;
  char *app_id;
  XccSession *core;
  XdpImplSession *skeleton;
  gboolean invalidated;
  gboolean identity_present;
  XccIdentityLevel identity;
} CertificateSession;

typedef struct
{
  GDBusMethodInvocation *invocation;
  XdpImplRequest *skeleton;
  CertificateSession *session;
  XccRequest *core;
  const char *cancelled_code;
  gboolean capabilities;
} Transaction;

typedef struct
{
  GDBusConnection *connection;
  XdpImplExperimentalCertificate1 *skeleton;
  XccContext *context;
  GHashTable *sessions;
  GHashTable *windows;
  GPtrArray *transactions;
  char *owner;
  guint64 generation;
  guint watch;
  gboolean resolved;
  gboolean stopping;
  PinPromptMode pin_prompt;
} Certificate;

static Certificate *certificate;

static GVariant *
empty_results (void)
{
  return g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0);
}

static GVariant *
string_variant (const char *value)
{
  /* SDK display strings can be NULL, including reader in original-prefix and
   * partial-extension candidate layouts. Apply this to every result string. */
  return g_variant_new_string (value != NULL ? value : "");
}

static void
reply (GDBusMethodInvocation *invocation,
       guint32                response,
       GVariant              *results)
{
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u@a{sv})", response, results));
}

static CertificateSession *
session_ref (CertificateSession *session)
{
  session->refs++;
  return session;
}

static void
session_unref (gpointer data)
{
  CertificateSession *session = data;

  if (--session->refs != 0)
    return;
  xcc_session_unref (session->core);
  g_clear_object (&session->skeleton);
  g_free (session->path);
  g_free (session->app_id);
  g_free (session);
}

static void
unexport (GDBusInterfaceSkeleton *skeleton)
{
  if (g_dbus_interface_skeleton_get_connection (skeleton) != NULL)
    g_dbus_interface_skeleton_unexport (skeleton);
}

static void
forget_session (CertificateSession *session)
{
  xcc_session_close (session->core);
  unexport (G_DBUS_INTERFACE_SKELETON (session->skeleton));
  g_hash_table_remove (certificate->sessions, session->path);
}

static void
cancel_transactions (CertificateSession *session,
                     const char         *code,
                     XccCancelReason     reason)
{
  for (guint i = 0; i < certificate->transactions->len; i++)
    {
      Transaction *transaction = g_ptr_array_index (certificate->transactions, i);

      if (!transaction->capabilities && (session == NULL || transaction->session == session))
        {
          transaction->cancelled_code = code;
          xcc_request_cancel (transaction->core, reason);
        }
    }
}

static void
set_owner (const char *owner)
{
  GHashTableIter iter;
  gpointer value;

  if (certificate->stopping || g_strcmp0 (certificate->owner, owner) == 0)
    return;
  xcc_context_set_owner_generation (certificate->context, ++certificate->generation);
  cancel_transactions (NULL, "owner_gone", XCC_CANCEL_OWNER_GONE);
  g_hash_table_iter_init (&iter, certificate->sessions);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      CertificateSession *session = value;

      xcc_session_close (session->core);
      unexport (G_DBUS_INTERFACE_SKELETON (session->skeleton));
      g_hash_table_iter_remove (&iter);
    }
  g_free (certificate->owner);
  certificate->owner = g_strdup (owner);
}

static void
resolve_owner (void)
{
  g_autoptr(GVariant) result = NULL;
  const char *owner = NULL;

  certificate->resolved = TRUE;
  result = g_dbus_connection_call_sync (certificate->connection,
                                        "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus", "GetNameOwner",
                                        g_variant_new ("(s)", FRONTEND_NAME), G_VARIANT_TYPE ("(s)"),
                                        G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
  if (result != NULL)
    g_variant_get (result, "(&s)", &owner);
  set_owner (owner);
}

static gboolean
is_authorized (GDBusMethodInvocation *invocation)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);

  if (!certificate->stopping)
    {
      if (!certificate->resolved)
        resolve_owner ();
      /* Cache may reject, but every acceptance requires a fresh bus snapshot:
       * NameOwnerChanged is unordered against messages from a former owner. */
      if (certificate->owner != NULL && g_strcmp0 (sender, certificate->owner) == 0)
        {
          resolve_owner ();
          if (certificate->owner != NULL && g_strcmp0 (sender, certificate->owner) == 0)
            return TRUE;
        }
    }
  g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                                "Only xdg-desktop-portal may call this interface");
  return FALSE;
}

static void
frontend_appeared (GDBusConnection *connection,
                   const char      *name,
                   const char      *owner,
                   gpointer         data)
{
  set_owner (owner);
  certificate->resolved = TRUE;
}

static void
frontend_vanished (GDBusConnection *connection,
                   const char      *name,
                   gpointer         data)
{
  set_owner (NULL);
  certificate->resolved = TRUE;
}

static gboolean
session_close (XdpImplSession        *object,
               GDBusMethodInvocation *invocation,
               gpointer               data)
{
  /* Authorization can discover owner loss and remove the table reference. */
  CertificateSession *session = session_ref (data);

  if (is_authorized (invocation))
    {
      cancel_transactions (session, "no_such_session", XCC_CANCEL_SESSION_CLOSED);
      forget_session (session);
      xdp_impl_session_complete_close (object, invocation);
    }
  session_unref (session);
  return TRUE;
}

static gboolean
request_close (XdpImplRequest        *object,
               GDBusMethodInvocation *invocation,
               gpointer               data)
{
  Transaction *transaction = data;

  if (is_authorized (invocation))
    {
      xcc_request_cancel (transaction->core, XCC_CANCEL_USER);
      unexport (G_DBUS_INTERFACE_SKELETON (object));
      xdp_impl_request_complete_close (object, invocation);
    }
  return TRUE;
}

static CertificateSession *
lookup_session (const char *path,
                const char *app_id)
{
  CertificateSession *session = g_hash_table_lookup (certificate->sessions, path);

  return session != NULL && !session->invalidated && g_str_equal (session->app_id, app_id) ? session : NULL;
}

/* Also cover CreateSession's short-lived, noninteractive Request export. */
static gboolean
empty_request_close (XdpImplRequest        *object,
                     GDBusMethodInvocation *invocation,
                     gpointer               data)
{
  if (is_authorized (invocation))
    {
      unexport (G_DBUS_INTERFACE_SKELETON (object));
      xdp_impl_request_complete_close (object, invocation);
    }
  return TRUE;
}

static gboolean
create_session (XdpImplExperimentalCertificate1 *object,
                GDBusMethodInvocation           *invocation,
                const char                      *handle,
                const char                      *session_handle,
                const char                      *app_id,
                GVariant                        *options,
                gpointer                         data)
{
  CertificateSession *session;
  g_autoptr(GDBusInterfaceSkeleton) request = NULL;
  XccSessionSpec spec = XCC_INIT (XccSessionSpec);

  if (!is_authorized (invocation))
    return TRUE;
  session = g_hash_table_lookup (certificate->sessions, session_handle);
  if (session != NULL && !session->invalidated)
    {
      reply (invocation, 2, error_results ("invalid_request"));
      return TRUE;
    }
  if (session != NULL)
    forget_session (session);

  /* Noninteractive, but the reference briefly exports this Request too. */
  request = G_DBUS_INTERFACE_SKELETON (xdp_impl_request_skeleton_new ());
  g_signal_connect (request, "handle-close", G_CALLBACK (empty_request_close), NULL);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         certificate->connection, handle, NULL))
    {
      reply (invocation, 2, error_results ("invalid_request"));
      return TRUE;
    }
  unexport (G_DBUS_INTERFACE_SKELETON (request));

  session = g_new0 (CertificateSession, 1);
  session->refs = 1;
  session->path = g_strdup (session_handle);
  session->app_id = g_strdup (app_id);
  session->skeleton = xdp_impl_session_skeleton_new ();
  spec.owner_generation = certificate->generation;
  spec.app_id = app_id;
  spec.log_id = session_handle;
  session->core = xcc_session_new (certificate->context, &spec, NULL);
  if (session->core == NULL)
    {
      session_unref (session);
      reply (invocation, 2, error_results ("no_such_session"));
      return TRUE;
    }
  xdp_impl_session_set_version (session->skeleton, 1);
  g_signal_connect (session->skeleton, "handle-close", G_CALLBACK (session_close), session);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session->skeleton),
                                         certificate->connection, session_handle, NULL))
    {
      xcc_session_close (session->core);
      session_unref (session);
      reply (invocation, 2, error_results ("invalid_request"));
      return TRUE;
    }
  g_hash_table_insert (certificate->sessions, g_strdup (session_handle), session);
  reply (invocation, 0, empty_results ());
  return TRUE;
}

static void
session_invalidated (XccSession *core,
                     const char *reason,
                     void       *data)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, certificate->sessions);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      CertificateSession *session = value;

      if (session->core == core && !session->invalidated)
        {
          session->invalidated = TRUE;
          xdp_impl_experimental_certificate1_emit_session_invalidated (certificate->skeleton,
                                                                       session->path, reason);
          xdp_impl_session_emit_closed (session->skeleton);
          /* Retain the exported tombstone for the frontend's Session.Close. */
          return;
        }
    }
}

static Transaction *
begin_transaction (GDBusMethodInvocation *invocation,
                   const char            *handle,
                   CertificateSession    *session)
{
  Transaction *transaction = g_new0 (Transaction, 1);

  transaction->skeleton = xdp_impl_request_skeleton_new ();
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (transaction->skeleton),
                                         certificate->connection, handle, NULL))
    {
      g_object_unref (transaction->skeleton);
      g_free (transaction);
      reply (invocation, 2, error_results ("invalid_request"));
      return NULL;
    }
  transaction->session = session_ref (session);
  transaction->invocation = g_object_ref (invocation);
  g_signal_connect (transaction->skeleton, "handle-close", G_CALLBACK (request_close), transaction);
  g_ptr_array_add (certificate->transactions, transaction);
  return transaction;
}

static GVariant *
bytes_variant (XccBytes bytes)
{
  return g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, bytes.data, bytes.size, 1);
}

static GVariant *
acquire_results (const XccResult *result)
{
  const XccCandidateInfo *info = xcc_candidate_info (result->candidate);
  GVariantBuilder builder, token;
  const char *operations[] = { result->may_sign ? "sign" : NULL, NULL };
  gboolean metadata = info->struct_size >= offsetof (XccCandidateInfo, token_reader) + sizeof (const char *);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "certificate_der",
                         bytes_variant (xcc_candidate_certificate (result->candidate)));
  g_variant_builder_add (&builder, "{sv}", "chain_der", g_variant_new_array (G_VARIANT_TYPE ("ay"), NULL, 0));
  g_variant_builder_add (&builder, "{sv}", "chain_status", g_variant_new_string ("leaf_only"));
  g_variant_builder_init (&token, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&token, "{sv}", "label", string_variant (metadata ? info->token_label : info->token));
  g_variant_builder_add (&token, "{sv}", "manufacturer", string_variant (metadata ? info->token_manufacturer : ""));
  g_variant_builder_add (&token, "{sv}", "model", string_variant (metadata ? info->token_model : ""));
  g_variant_builder_add (&token, "{sv}", "reader", string_variant (metadata ? info->token_reader : info->reader));
  g_variant_builder_add (&token, "{sv}", "protected_authentication_path", g_variant_new_boolean (info->protected_path));
  g_variant_builder_add (&builder, "{sv}", "token_display", g_variant_builder_end (&token));
  g_variant_builder_add (&builder, "{sv}", "key_type", string_variant (info->key_type));
  g_variant_builder_add (&builder, "{sv}", "key_size", g_variant_new_uint32 (info->key_size));
  if (info->key_curve_present)
    g_variant_builder_add (&builder, "{sv}", "key_curve", string_variant (info->key_curve));
  g_variant_builder_add (&builder, "{sv}", "supported_mechanisms", g_variant_new_strv (info->mechanisms, -1));
  g_variant_builder_add (&builder, "{sv}", "permitted_operations", g_variant_new_strv (operations, -1));
  g_variant_builder_add (&builder, "{sv}", "may_prompt_later", g_variant_new_boolean (TRUE));
  return g_variant_builder_end (&builder);
}

static void
transaction_done (XccRequest      *request,
                  const XccResult *result,
                  void            *data)
{
  Transaction *transaction = data;
  GVariantBuilder builder;
  GVariant *results = NULL;
  guint32 response = 0;

  if (transaction->capabilities)
    {
      const char *purposes[] = { "client_auth", "signing", "email", "ssh", NULL };
      const char *operations[] = { "sign", NULL };

      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&builder, "{sv}", "purposes", g_variant_new_strv (purposes, -1));
      g_variant_builder_add (&builder, "{sv}", "operations", g_variant_new_strv (operations, -1));
      g_variant_builder_add (&builder, "{sv}", "mechanisms", g_variant_new_strv (result->mechanisms, -1));
      g_variant_builder_add (&builder, "{sv}", "protected_authentication_path", g_variant_new_boolean (result->protected_path));
      g_variant_builder_add (&builder, "{sv}", "has_display", g_variant_new_boolean (result->has_display));
      results = g_variant_builder_end (&builder);
    }
  else if (transaction->cancelled_code != NULL)
    {
      response = 2;
      results = error_results (transaction->cancelled_code);
    }
  else if (result->error != XCC_OK)
    {
      response = xcc_error_wire_response (result->error);
      results = result->kind == XCC_RESULT_ACQUIRE && result->error == XCC_ERROR_CANCELLED ?
        empty_results () : error_results (xcc_error_wire_name (result->error));
    }
  else if (result->kind == XCC_RESULT_ACQUIRE)
    {
      const XccCandidateInfo *info = xcc_candidate_info (result->candidate);

      if (info == NULL || info->abi_version != XCC_ABI_VERSION ||
          info->struct_size < offsetof (XccCandidateInfo, protected_path) + sizeof (info->protected_path))
        {
          response = 2;
          results = error_results ("device_error");
        }
      else
        results = acquire_results (result);
    }
  else
    {
      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&builder, "{sv}", "signature", bytes_variant (result->signature));
      results = g_variant_builder_end (&builder);
    }

  /* Result storage is borrowed. Serialize before releasing the request, and
   * unexport before returning its exactly-once terminal method response. */
  if (transaction->skeleton != NULL)
    unexport (G_DBUS_INTERFACE_SKELETON (transaction->skeleton));
  if (transaction->capabilities)
    g_dbus_method_invocation_return_value (transaction->invocation, g_variant_new ("(@a{sv})", results));
  else
    reply (transaction->invocation, response, results);
  g_ptr_array_remove_fast (certificate->transactions, transaction);
  g_clear_object (&transaction->skeleton);
  g_object_unref (transaction->invocation);
  if (transaction->session != NULL)
    session_unref (transaction->session);
  xcc_request_unref (transaction->core);
  g_free (transaction);
}

/* Defaults apply to absent keys only; mistyped constraints must fail. Unknown
 * top-level keys are extensible, but nested security constraints are closed. */
static gboolean
option_string (GVariant   *options,
               const char *key,
               char      **out,
               gboolean   *bad)
{
  g_autoptr(GVariant) value = g_variant_lookup_value (options, key, NULL);

  if (value == NULL)
    return FALSE;
  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
    {
      *bad = TRUE;
      return FALSE;
    }
  *out = g_variant_dup_string (value, NULL);
  return TRUE;
}

static gboolean
option_uint (GVariant   *options,
             const char *key,
             guint32    *out,
             gboolean   *bad)
{
  g_autoptr(GVariant) value = g_variant_lookup_value (options, key, NULL);

  if (value == NULL)
    return FALSE;
  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32))
    {
      *bad = TRUE;
      return FALSE;
    }
  *out = g_variant_get_uint32 (value);
  return TRUE;
}

static GVariant *
option_dict (GVariant   *options,
             const char *key,
             gboolean   *bad)
{
  g_autoptr(GVariant) value = g_variant_lookup_value (options, key, NULL);

  if (value != NULL && !g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT))
    {
      *bad = TRUE;
      return NULL;
    }
  return g_steal_pointer (&value);
}

static gboolean
option_strv (GVariant   *options,
             const char *key,
             char     ***out,
             gboolean   *bad)
{
  g_autoptr(GVariant) value = g_variant_lookup_value (options, key, NULL);

  if (value == NULL)
    return FALSE;
  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_STRING_ARRAY))
    {
      *bad = TRUE;
      return FALSE;
    }
  *out = g_variant_dup_strv (value, NULL);
  return TRUE;
}

static gboolean
known_keys (GVariant          *dict,
            const char *const *known)
{
  GVariantIter iter;
  const char *key;
  GVariant *value;

  g_variant_iter_init (&iter, dict);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      g_variant_unref (value);
      if (!g_strv_contains (known, key))
        return FALSE;
    }
  return TRUE;
}

static gboolean
module_request_handle (const char *handle)
{
  static const char prefix[] = "/org/freedesktop/portal/desktop/request/";
  const char *token;

  if (!g_str_has_prefix (handle, prefix))
    return FALSE;
  token = strchr (handle + strlen (prefix), '/');
  return token != NULL && g_str_has_prefix (token + 1, "pkcs11a");
}

static gboolean
acquire_credential (XdpImplExperimentalCertificate1 *object,
                    GDBusMethodInvocation           *invocation,
                    const char                      *handle,
                    const char                      *session_handle,
                    const char                      *app_id,
                    const char                      *parent_window,
                    GVariant                        *options,
                    gpointer                         data)
{
  static const char * const purposes[] = { "client_auth", "signing", "email", "ssh", NULL };
  static const char * const policy_keys[] = { "sign", NULL };
  static const char * const filter_keys[] = {
    "issuers", "key_usage", "eku", "key_algorithms", "token_label", "piv_slot", NULL
  };
  static const char * const slots[] = {
    "authentication", "signature", "key_management", "card_authentication", NULL
  };
  XccAcquireSpec spec = XCC_INIT (XccAcquireSpec);
  CertificateSession *session;
  Transaction *transaction;
  const char *purpose;
  g_autofree char *identity = NULL;
  g_autofree char *reason = NULL;
  g_autofree char *activation = NULL;
  g_autofree char *interaction = NULL;
  g_autofree char *token = NULL;
  g_autofree char *slot = NULL;
  g_auto(GStrv) usage = NULL;
  g_auto(GStrv) eku = NULL;
  g_auto(GStrv) algorithms = NULL;
  g_autoptr(GVariant) policy = NULL;
  g_autoptr(GVariant) filter = NULL;
  g_autoptr(GVariant) issuer_array = NULL;
  g_autoptr(GPtrArray) issuer_values = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  g_autofree XccBytes *issuers = NULL;
  gboolean bad = FALSE;

  if (!is_authorized (invocation))
    return TRUE;
  session = lookup_session (session_handle, app_id);
  if (session == NULL)
    {
      reply (invocation, 2, error_results ("no_such_session"));
      return TRUE;
    }
  if (!g_variant_lookup (options, "purpose", "&s", &purpose) || !g_strv_contains (purposes, purpose))
    {
      reply (invocation, 2, error_results ("invalid_purpose"));
      return TRUE;
    }
  for (guint i = 0; i < G_N_ELEMENTS (purposes) - 1; i++)
    if (g_str_equal (purposes[i], purpose))
      spec.purpose = i;
  spec.principal = (XccPrincipal) {
    sizeof (XccPrincipal), XCC_ABI_VERSION, certificate->generation, app_id, XCC_IDENTITY_UNKNOWN, TRUE
  };
  if (option_string (options, "app_identity_level", &identity, &bad))
    {
      if (g_str_equal (identity, "sandboxed"))
        spec.principal.identity_level = XCC_IDENTITY_VERIFIED_SANDBOXED;
      else if (g_str_equal (identity, "host"))
        spec.principal.identity_level = XCC_IDENTITY_DERIVED_HOST;
      else if (!g_str_equal (identity, "unidentified"))
        bad = TRUE;
    }
  option_string (options, "reason", &reason, &bad);
  option_string (options, "activation_token", &activation, &bad);
  if (option_string (options, "interaction_mode", &interaction, &bad) &&
      !g_str_equal (interaction, "forbidden") && !g_str_equal (interaction, "required") &&
      !g_str_equal (interaction, "allowed"))
    bad = TRUE;
  spec.interaction_present = TRUE;
  spec.interaction_forbidden = g_strcmp0 (interaction, "forbidden") == 0;
  spec.lifetime_present = TRUE;
  spec.lifetime_seconds = 300;
  option_uint (options, "lifetime", &spec.lifetime_seconds, &bad);
  bad |= spec.lifetime_seconds == 0;
  spec.lifetime_seconds = MIN (spec.lifetime_seconds, 3600u);
  spec.sign_present = TRUE;
  spec.sign = TRUE;
  policy = option_dict (options, "operation_policy", &bad);
  if (policy != NULL)
    {
      g_autoptr(GVariant) sign = g_variant_lookup_value (policy, "sign", NULL);

      bad |= !known_keys (policy, policy_keys);
      if (sign != NULL)
        {
          if (!g_variant_is_of_type (sign, G_VARIANT_TYPE_BOOLEAN))
            bad = TRUE;
          else
            spec.sign = g_variant_get_boolean (sign);
        }
    }
  if (bad)
    {
      reply (invocation, 2, error_results ("invalid_request"));
      return TRUE;
    }
  /* Mirror the reference's pre-filter assurance latch without the private
   * xcc_session_check_principal symbol. The core also enforces this rule. */
  if (session->identity_present && spec.principal.identity_level < session->identity)
    {
      reply (invocation, 2, error_results ("no_such_session"));
      return TRUE;
    }
  session->identity_present = TRUE;
  session->identity = spec.principal.identity_level;

  filter = option_dict (options, "certificate_filter", &bad);
  if (filter != NULL)
    {
      bad |= !known_keys (filter, filter_keys);
      spec.key_usage_present = option_strv (filter, "key_usage", &usage, &bad);
      spec.eku_oids_present = option_strv (filter, "eku", &eku, &bad);
      spec.key_algorithms_present = option_strv (filter, "key_algorithms", &algorithms, &bad);
      spec.token_label_present = option_string (filter, "token_label", &token, &bad);
      spec.piv_slot_present = option_string (filter, "piv_slot", &slot, &bad);
      if (spec.piv_slot_present && !g_strv_contains (slots, slot))
        bad = TRUE;
      issuer_array = g_variant_lookup_value (filter, "issuers", NULL);
      if (issuer_array != NULL)
        {
          spec.issuers_present = TRUE;
          if (!g_variant_is_of_type (issuer_array, G_VARIANT_TYPE ("aay")))
            bad = TRUE;
          else
            {
              spec.n_issuers = g_variant_n_children (issuer_array);
              issuers = g_new0 (XccBytes, spec.n_issuers);
              for (gsize i = 0; i < spec.n_issuers; i++)
                {
                  GVariant *issuer = g_variant_get_child_value (issuer_array, i);

                  issuers[i] = (XccBytes) XCC_INIT (XccBytes);
                  issuers[i].data = g_variant_get_fixed_array (issuer, &issuers[i].size, 1);
                  g_ptr_array_add (issuer_values, issuer);
                }
            }
        }
    }
  if (bad)
    {
      reply (invocation, 2, error_results ("invalid_filter"));
      return TRUE;
    }
  spec.key_usage = (const char * const *) usage;
  spec.eku_oids = (const char * const *) eku;
  spec.key_algorithms = (const char * const *) algorithms;
  spec.token_label = token;
  spec.piv_slot = slot;
  spec.issuers = issuers;
  spec.parent_window = parent_window;
  spec.activation_token = activation;
  spec.reason = reason;
  spec.generic_purpose_present = TRUE;
  spec.generic_purpose = module_request_handle (handle);
  transaction = begin_transaction (invocation, handle, session);
  if (transaction != NULL)
    transaction->core = xcc_acquire_async (session->core, &spec, transaction_done, transaction);
  return TRUE;
}

static gboolean
sign (XdpImplExperimentalCertificate1 *object,
      GDBusMethodInvocation           *invocation,
      const char                      *handle,
      const char                      *session_handle,
      const char                      *app_id,
      const char                      *parent_window,
      GVariant                        *options,
      gpointer                         data)
{
  static const char * const keys[] = { "hash", "mgf", "salt_length", "signature_encoding", NULL };
  XccSignSpec spec = XCC_INIT (XccSignSpec);
  CertificateSession *session;
  Transaction *transaction;
  gboolean bad = FALSE;
  g_autofree char *operation_id = NULL;
  g_autofree char *mechanism = NULL;
  g_autofree char *hash = NULL;
  g_autofree char *mgf = NULL;
  g_autofree char *encoding = NULL;
  g_autoptr(GVariant) parameters = NULL;
  g_autoptr(GVariant) payload = NULL;

  if (!is_authorized (invocation))
    return TRUE;
  session = lookup_session (session_handle, app_id);
  if (session == NULL)
    {
      reply (invocation, 2, error_results ("no_such_session"));
      return TRUE;
    }
  option_string (options, "operation_id", &operation_id, &bad);
  if (!option_string (options, "mechanism", &mechanism, &bad))
    bad = TRUE;
  parameters = option_dict (options, "parameters", &bad);
  payload = g_variant_lookup_value (options, "data", G_VARIANT_TYPE ("ay"));
  if (bad || payload == NULL)
    {
      reply (invocation, 2, error_results ("invalid_request"));
      return TRUE;
    }
  spec.principal = (XccPrincipal) {
    sizeof (XccPrincipal), XCC_ABI_VERSION, certificate->generation, app_id, XCC_IDENTITY_UNKNOWN, FALSE
  };
  spec.parent_window = parent_window;
  spec.operation_id = operation_id;
  spec.mechanism = mechanism;
  spec.data = (XccBytes) XCC_INIT (XccBytes);
  spec.data.data = g_variant_get_fixed_array (payload, &spec.data.size, 1);
  if (parameters != NULL)
    {
      bad |= !known_keys (parameters, keys);
      spec.hash_present = option_string (parameters, "hash", &hash, &bad);
      spec.hash = hash;
      if (g_str_equal (mechanism, "RSA_PSS"))
        {
          spec.mgf_present = option_string (parameters, "mgf", &mgf, &bad);
          spec.mgf = mgf;
          spec.salt_length_present = option_uint (parameters, "salt_length", &spec.salt_length, &bad);
        }
      spec.encoding_present = option_string (parameters, "signature_encoding", &encoding, &bad);
      spec.signature_encoding = encoding;
    }
  if (bad)
    {
      /* Public SDK equivalent of the reference's private parsed-sign API:
       * invalid encoding always fails after principal/grant checks, before
       * any device work. This preserves nested-parameter error precedence. */
      spec.encoding_present = TRUE;
      spec.signature_encoding = "";
    }
  transaction = begin_transaction (invocation, handle, session);
  if (transaction != NULL)
    transaction->core = xcc_sign_async (session->core, &spec, transaction_done, transaction);
  return TRUE;
}

static gboolean
get_capabilities (XdpImplExperimentalCertificate1 *object,
                  GDBusMethodInvocation           *invocation,
                  const char                      *app_id,
                  GVariant                        *options,
                  gpointer                         data)
{
  Transaction *transaction;

  if (!is_authorized (invocation))
    return TRUE;
  transaction = g_new0 (Transaction, 1);
  transaction->capabilities = TRUE;
  transaction->invocation = g_object_ref (invocation);
  g_ptr_array_add (certificate->transactions, transaction);
  transaction->core = xcc_get_capabilities_async (certificate->context, transaction_done, transaction);
  return TRUE;
}

void
certificate_window_present (GtkWindow  *window,
                            const char *parent_window,
                            const char *activation_token)
{
  GxdpExternalWindow *parent = NULL;

  gtk_widget_realize (GTK_WIDGET (window));
  if (parent_window != NULL && *parent_window != '\0')
    parent = gxdp_external_window_new_from_handle (parent_window);
  if (parent != NULL)
    {
      gxdp_external_window_set_parent_of (parent, gtk_native_get_surface (GTK_NATIVE (window)));
      g_object_set_data_full (G_OBJECT (window), "certificate-parent", parent, g_object_unref);
    }
  /* Consume only this presentation's token; PIN views deliberately have none.
   * Failed/unsupported parent imports fall back to an unparented window. */
  if (activation_token != NULL && *activation_token != '\0')
    gtk_window_set_startup_id (window, activation_token);
  gtk_window_present (window);
}

static void
add_window (XccUiId   id,
            GtkWindow *window)
{
  guint64 *key = g_new (guint64, 1);

  *key = id;
  g_hash_table_insert (certificate->windows, key, g_object_ref_sink (window));
}

static void
chooser_show (void                 *data,
              XccUiId               id,
              const XccChooserView *view)
{
  add_window (id, GTK_WINDOW (certificate_dialog_new (certificate->context, id, view)));
}

static void
chooser_update (void                 *data,
                XccUiId               id,
                const XccChooserView *view)
{
  CertificateDialog *window = g_hash_table_lookup (certificate->windows, &id);

  if (window != NULL && CERTIFICATE_IS_DIALOG (window))
    certificate_dialog_update (window, view);
}

static void
pin_show (void             *data,
          XccUiId           id,
          const XccPinView *view)
{
  gboolean available = certificate->pin_prompt != PIN_PROMPT_DIALOG &&
    certificate_pin_system_available (certificate->connection);

  if (certificate->pin_prompt == PIN_PROMPT_SYSTEM && !available)
    {
      xcc_ui_failed (certificate->context, id, XCC_ERROR_NO_DISPLAY);
      return;
    }
  add_window (id, GTK_WINDOW (available ?
    certificate_pin_system_new (certificate->context, id, view) :
    certificate_pin_dialog_new (certificate->context, id, view)));
}

static void
pin_update (void             *data,
            XccUiId           id,
            const XccPinView *view)
{
  CertificatePinDialog *window = g_hash_table_lookup (certificate->windows, &id);

  if (window != NULL && CERTIFICATE_IS_PIN_DIALOG (window))
    certificate_pin_dialog_update (window, view);
}

static void
warning (void             *data,
         XccUiId           id,
         const XccWarning *view)
{
  CertificatePinDialog *window = g_hash_table_lookup (certificate->windows, &id);

  if (window != NULL && CERTIFICATE_IS_PIN_DIALOG (window))
    certificate_pin_dialog_warning (window, view);
}

static void
hide (void   *data,
      XccUiId id)
{
  GtkWindow *window = g_hash_table_lookup (certificate->windows, &id);

  if (window == NULL)
    return;
  if (CERTIFICATE_IS_PIN_DIALOG (window))
    certificate_pin_dialog_hide (CERTIFICATE_PIN_DIALOG (window));
  else
    certificate_dialog_hide (CERTIFICATE_DIALOG (window));
}

static void
close_ui (void   *data,
          XccUiId id)
{
  GtkWindow *window = g_hash_table_lookup (certificate->windows, &id);

  if (window == NULL)
    return;
  hide (data, id);
  gtk_window_destroy (window);
  g_hash_table_remove (certificate->windows, &id);
}

static gboolean
read_config_file (const char    *path,
                  char        ***modules,
                  gboolean      *allow_software,
                  PinPromptMode *pin_prompt,
                  GError       **error)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autoptr(GError) local_error = NULL;

  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, &local_error))
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        return TRUE;
      /* Do not echo configuration contents, module names or parser errors. */
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Could not read Certificate configuration");
      return FALSE;
    }
  if (g_key_file_has_key (file, "Certificate", "Modules", NULL))
    {
      g_auto(GStrv) values = g_key_file_get_string_list (file, "Certificate", "Modules", NULL, &local_error);

      if (local_error != NULL)
        goto invalid;
      g_strfreev (*modules);
      *modules = g_steal_pointer (&values);
      if (*modules != NULL && (*modules)[0] == NULL)
        g_clear_pointer (modules, g_strfreev);
    }
  if (g_key_file_has_key (file, "Certificate", "PinPrompt", NULL))
    {
      g_autofree char *value = g_key_file_get_string (file, "Certificate", "PinPrompt", &local_error);

      if (local_error != NULL)
        goto invalid;
      if (g_strcmp0 (value, "auto") == 0)
        *pin_prompt = PIN_PROMPT_AUTO;
      else if (g_strcmp0 (value, "system") == 0)
        *pin_prompt = PIN_PROMPT_SYSTEM;
      else if (g_strcmp0 (value, "dialog") == 0)
        *pin_prompt = PIN_PROMPT_DIALOG;
      else
        goto invalid;
    }
  if (g_key_file_has_key (file, "Certificate", "AllowSoftwareTokens", NULL))
    {
      *allow_software = g_key_file_get_boolean (file, "Certificate", "AllowSoftwareTokens", &local_error);
      if (local_error != NULL)
        goto invalid;
    }
  return TRUE;

invalid:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Invalid Certificate configuration");
  return FALSE;
}

static void
shutdown_done (XccContext *context,
               void       *data)
{
  g_main_loop_quit (data);
}

void
certificate_shutdown (void)
{
  GHashTableIter iter;
  gpointer value;
  g_autoptr(GMainLoop) drain = NULL;

  if (certificate == NULL)
    return;
  certificate->stopping = TRUE;
  if (certificate->watch != 0)
    g_bus_unwatch_name (certificate->watch);
  if (certificate->context != NULL)
    {
      cancel_transactions (NULL, "backend_gone", XCC_CANCEL_BACKEND_GONE);
      /* Keep UI state, bus exports and dispatcher alive through all workers,
       * completions and invalidations. Never unload the core DSO. */
      drain = g_main_loop_new (NULL, FALSE);
      xcc_context_shutdown_async (certificate->context, shutdown_done, drain);
      g_main_loop_run (drain);
      g_hash_table_iter_init (&iter, certificate->sessions);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        {
          CertificateSession *session = value;

          xcc_session_close (session->core);
          unexport (G_DBUS_INTERFACE_SKELETON (session->skeleton));
        }
      g_hash_table_remove_all (certificate->sessions);
      xcc_context_unref (certificate->context);
    }
  if (certificate->skeleton != NULL)
    unexport (G_DBUS_INTERFACE_SKELETON (certificate->skeleton));
  g_clear_object (&certificate->skeleton);
  g_hash_table_unref (certificate->sessions);
  g_hash_table_unref (certificate->windows);
  g_ptr_array_unref (certificate->transactions);
  g_object_unref (certificate->connection);
  g_free (certificate->owner);
  g_clear_pointer (&certificate, g_free);
}

gboolean
certificate_init (GDBusConnection *connection,
                  GError         **error)
{
  XccConfig config = XCC_INIT (XccConfig);
  XccUiV1 ui = {
    .struct_size = sizeof (XccUiV1),
    .abi_version = XCC_ABI_VERSION,
    .capabilities = XCC_UI_NEEDS_DISPLAY,
    .chooser_show = chooser_show,
    .chooser_update = chooser_update,
    .pin_show = pin_show,
    .pin_update = pin_update,
    .warning = warning,
    .hide = hide,
    .close = close_ui,
  };
  XccDispatch dispatch = xcc_dispatch_glib (g_main_context_default ());
  g_auto(GStrv) modules = NULL;
  g_autofree char *user_config = NULL;
  gboolean allow_software = FALSE;
  PinPromptMode pin_prompt = PIN_PROMPT_AUTO;

  if (certificate != NULL)
    return TRUE;
  /* This experimental interface has no GSettings schema. Snapshot a keyfile
   * at startup for now; GNOME reviewers may prefer GSettings before merging.
   * Module and software-token policy never come from environment variables. */
  user_config = g_build_filename (g_get_user_config_dir (), "xdg-desktop-portal-gnome", "certificate.conf", NULL);
  if (!read_config_file ("/etc/xdg/xdg-desktop-portal-gnome/certificate.conf", &modules, &allow_software, &pin_prompt, error) ||
      !read_config_file (user_config, &modules, &allow_software, &pin_prompt, error))
    return FALSE;

  certificate = g_new0 (Certificate, 1);
  certificate->connection = g_object_ref (connection);
  certificate->pin_prompt = pin_prompt;
  certificate->sessions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, session_unref);
  certificate->windows = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_object_unref);
  certificate->transactions = g_ptr_array_new ();
  config.module_paths = (const char * const *) modules;
  config.allow_software = allow_software;
  config.has_display = gdk_display_get_default () != NULL;
  config.session_invalidated = session_invalidated;
  certificate->context = xcc_context_new (&config, &ui, &dispatch, NULL);
  if (certificate->context == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create Certificate context");
      certificate_shutdown ();
      return FALSE;
    }
  certificate->skeleton = xdp_impl_experimental_certificate1_skeleton_new ();
  xdp_impl_experimental_certificate1_set_version (certificate->skeleton, 1);
  g_signal_connect (certificate->skeleton, "handle-create-session", G_CALLBACK (create_session), NULL);
  g_signal_connect (certificate->skeleton, "handle-acquire-credential", G_CALLBACK (acquire_credential), NULL);
  g_signal_connect (certificate->skeleton, "handle-sign", G_CALLBACK (sign), NULL);
  g_signal_connect (certificate->skeleton, "handle-get-capabilities", G_CALLBACK (get_capabilities), NULL);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (certificate->skeleton),
                                         connection, DESKTOP_PORTAL_OBJECT_PATH, error))
    {
      certificate_shutdown ();
      return FALSE;
    }
  certificate->watch = g_bus_watch_name_on_connection (connection, FRONTEND_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       frontend_appeared, frontend_vanished, NULL, NULL);
  g_debug ("providing org.freedesktop.impl.portal.experimental.Certificate1");
  return TRUE;
}
