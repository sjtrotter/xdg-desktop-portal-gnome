Experimental Certificate1 portal
===============================

Build with `-Dcertificate=enabled` and the installed xdp-certificate-core-0 SDK.
The feature defaults to disabled. Only the public SDK is used; no PKCS#11 policy
or middleware implementation is copied into this backend.

Configuration
-------------

The experimental interface has no GSettings schema. Pending GNOME review, read
`/etc/xdg/xdg-desktop-portal-gnome/certificate.conf`, then
`$XDG_CONFIG_HOME/xdg-desktop-portal-gnome/certificate.conf` (normally under
`~/.config`), once at startup. User keys override system keys independently;
restart the backend to apply changes. GNOME reviewers may prefer GSettings.

```ini
[Certificate]
Modules=/usr/lib64/pkcs11/libsofthsm2.so;
AllowSoftwareTokens=true
PinPrompt=auto
```

`Modules` is a GLib keyfile string list (semicolon-separated, keyfile escaping).
Absent or empty uses p11-kit's registered modules. `AllowSoftwareTokens` defaults
to false. Invalid configuration prevents Certificate initialization. Neither
setting is read from environment variables. Module paths load code in the
backend and have the same trust level as the standalone service's `--module`.
As in that service, explicitly selected modules may expose software tokens even
with `AllowSoftwareTokens=false`; the SDK's module policy is unchanged.

`PinPrompt` accepts `auto` (the default), `system`, or `dialog`. Selection is
made for each PIN request. `auto` uses Gcr's system prompter when
`org.gnome.keyring.SystemPrompter` has an owner on the session bus; otherwise it
uses this backend's GTK dialog. `system` requires an existing prompter owner.
When unavailable, the backend reports `XCC_ERROR_NO_DISPLAY` to the SDK.
Public `Sign` reports failure with response 2.
`dialog` always uses the GTK dialog. A system prompt that fails to open or
disappears settles the request; it never silently switches to the GTK dialog.
Invalid values prevent Certificate initialization. This setting is not read
from the environment.

In a GNOME session the system prompter is GNOME Shell. Its prompt is
session-modal and is not attached to the caller's window. PIN-pad readers show
a notice instead of a password field; the final attempt requires Continue.
The `gcr-4` build dependency is required only when Certificate support is enabled.

PIN protection (open GNOME review question)
------------------------------------------

The embedded GNOME backend uses scoped PIN protection. With the system prompt,
the PIN is typed into gnome-shell and transferred to the backend over Gcr's
secret exchange. The returned password is copied straight from Gcr's secure
storage into XccSecret, without intermediate strings, properties, logs or signal
arguments in certificate code. Shell and Gcr input-handling copies are outside
this backend's control.

With `PinPrompt=dialog`, GtkPasswordEntry uses GTK's non-pageable secure buffer, has no peek icon or undo, and is cleared
immediately after copying directly to XccSecret, on busy, hide and destruction.
Retained PIN bytes live in the SDK's XccSecret: best-effort mlock, page-aligned
storage, MADV_DONTDUMP where supported, and full-capacity wiping on release.
The SDK logs lock failure. Certificate code never stores PIN text in ordinary
strings, properties, signal arguments or logs.

Unlike the standalone xdg-desktop-portal-certificate service (non-dumpable, no
cores), this backend preserves normal dumpability and core limits for all of
its portals. Crash dumps, crash reports or same-user ptrace can contain transient
PIN copies from GTK (dialog mode), Gcr, input methods and the input stack despite
secure entry/secret storage. Scoped protection does not prevent ptrace from reading
live PINs. Clearing widgets cannot prove erasure of input-handling copies.

Open question for GNOME reviewers: accept this residual risk, apply process-wide
hardening, or collect PINs in a separate helper. Process-wide hardening affects
all portals' crash diagnostics and portal-backed host integrations; a zero hard
RLIMIT_CORE is also inherited by launched children, including printing commands.
Actual dump behavior depends on core_pattern. A separate PIN helper would keep
those restrictions outside this desktop-wide backend; it is not implemented.

Wire and UI contract
--------------------

Current frontend ownership is checked on every method and both Close calls.
Owner changes advance the core generation, cancel transactions and remove old
sessions. Invalidated sessions emit SessionInvalidated and Session.Closed but
remain exported for the frontend's Close. The adapter tracks the reference's
pre-filter identity latch and uses an invalid public signature encoding to
preserve deferred malformed-parameter errors without private SDK symbols, as
in the KDE adapter. Original token_* metadata supplies token_display; sanitized
fields are used for plain-text chooser display. The chooser remains live while
empty and during hotplug refresh. PIN policy, retry limits, protected-path login,
final-try confirmation and operation authorization remain in the SDK.

The standalone GTK4 service remains available. xdg-desktop-portal-gtk uses GTK3
and needs a separate GTK3 UI adapter or routing to the standalone GTK4 service;
these GNOME templates cannot be used by that backend unchanged.
