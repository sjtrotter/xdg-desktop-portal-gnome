/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <adwaita.h>
#include <xdp-certificate/ui.h>

#define CERTIFICATE_TYPE_DIALOG (certificate_dialog_get_type ())
G_DECLARE_FINAL_TYPE (CertificateDialog, certificate_dialog, CERTIFICATE, DIALOG, AdwWindow)

CertificateDialog *certificate_dialog_new (XccContext *context, XccUiId id,
                                           const XccChooserView *view);
void certificate_dialog_update (CertificateDialog *self, const XccChooserView *view);
void certificate_dialog_hide (CertificateDialog *self);
