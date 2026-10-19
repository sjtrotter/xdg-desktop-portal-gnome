/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <adwaita.h>
#include <xdp-certificate/ui.h>

#define CERTIFICATE_TYPE_PIN_DIALOG (certificate_pin_dialog_get_type ())
G_DECLARE_FINAL_TYPE (CertificatePinDialog, certificate_pin_dialog, CERTIFICATE, PIN_DIALOG, AdwWindow)

CertificatePinDialog *certificate_pin_dialog_new (XccContext *context, XccUiId id,
                                                  const XccPinView *view);
void certificate_pin_dialog_update (CertificatePinDialog *self, const XccPinView *view);
void certificate_pin_dialog_warning (CertificatePinDialog *self, const XccWarning *warning);
void certificate_pin_dialog_hide (CertificatePinDialog *self);

/* Uses GNOME Shell's session-modal Gcr system prompter instead of presenting
 * this window. Ownership and SDK callbacks match the dialog implementation. */
CertificatePinDialog *certificate_pin_system_new (XccContext *context, XccUiId id,
                                                 const XccPinView *view);
gboolean certificate_pin_system_available (GDBusConnection *connection);
