Experimental Certificate1 portal
===============================

Build with `-Dcertificate=enabled`, gcr-4 and the installed xdp-certificate-core-0 SDK.
The feature defaults to disabled. `-Dcertificate=auto` enables it only when both
dependencies are found. The backend links only the public SDK; card and PIN policy
remain in that library.

Coverage
--------

SoftHSM container runs cover acquisition, RSA and ECDSA signing, regrant,
cancellation, GnuTLS mutual TLS and NSS across expiry. The backend also ran in a
live GNOME 50 session with one PIV card, OpenSC and an SCR 331 reader: test-client
signing, Flathub Firefox sign-in, chooser hotplug and GNOME Shell's system prompt.
On 2026-10-19 its native PIN dialog handled two wrong PINs, showed the final-try
warning, then accepted the correct PIN and signed. PIN-pad and Cancel-on-final-try
checks remain unrun. Full procedures and recorded results are in the certificate
repository's [TESTING.md](https://github.com/sjtrotter/xdg-desktop-portal-certificate/blob/main/docs/TESTING.md).

Tests default to disabled. Enable them with `meson setup build -Dcertificate=enabled -Dtests=true`,
compile with `meson compile -C build -j1`, then run `meson test -C build --print-errorlogs`.
They cover dependency probing, separate dialog window groups, chooser identity and
details, system-prompt callbacks and settings-only startup. The UI wrapper creates
a private session bus before GTK starts. These tests do not establish card behavior.

Configuration
-------------

The experimental interface has no GSettings schema. The backend reads
`/etc/xdg/xdg-desktop-portal-gnome/certificate.conf`, then
`$XDG_CONFIG_HOME/xdg-desktop-portal-gnome/certificate.conf` (normally under
`~/.config`), once at startup. User keys override system keys independently;
restart the backend to apply changes.

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
Impl and public `Sign` report response `2` with `reason=no_display` while the grant
remains current.
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

This backend preserves normal dumpability and core limits for all of its portals.
The standalone xdg-desktop-portal-certificate service disables both.
Crash dumps, crash reports or same-user ptrace can contain transient
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
fields are used for plain-text chooser display. Failures use the string `reason`
key, which the frontend forwards for acquisition and signing. The chooser remains
live while empty and during hotplug refresh. Each dialog has its own window group;
one client's modal dialog does not block another client's dialogs. Identified
callers have an app-id label, host callers have an identity warning, and unknown
callers have a warning asking them to continue only if they started the request.
Sandboxed callers have no explicit sandbox-status label; that remains a gap against
the certificate repository's trusted-dialog requirement.
Certificate details are formatted from structured SDK fields with desktop
translations. The chooser describes permitted operations and grant duration.
PIN policy, retry limits, protected-path login,
final-try confirmation and operation authorization remain in the SDK.

The chooser reads `tokens_reading` through the SDK's size-checked helper. During
refresh with an empty list it shows a spinner and reading message, announces that
state once and disables Allow. Each update adopts the new consent revision while
preserving unchanged rows, selection and open details. Older SDK producers without
the appended field mean no reading state. Certificate initialization is skipped
in settings-only mode. Pending Gcr callbacks retain SDK UI holds until completion.

The standalone GTK4 service remains available. xdg-desktop-portal-gtk uses GTK3
and needs a separate GTK3 UI adapter or routing to the standalone GTK4 service;
these GNOME templates cannot be used by that backend unchanged.
