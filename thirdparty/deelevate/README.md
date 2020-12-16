# EleDo - Elevated-Do

[Download EleDo.zip](https://github.com/wez/deelevate/releases/download/Continuous/EleDo.zip)

This repo is the home of EleDo (and NormDo), utilities that allow switching
privilege levels from the command line on Windows 10 and later.

The core privilege shifting code is also available as a Rust crate and a
small C library to make it possible for an console based application to
to detect and adjust its privilege level.

While this repo is named for the privilege elevation aspect, it originally
started life with the goal of reducing privileges to "normal" levels.

## Why Elevate?

In some cases you need a higher level of access than is normal; for example, to
install software or make system configuration.  This is not new; most users
will use the "Run as Administrator" option for a powershell session and run
their commands that way, leaving the privileged session open for convenience,
mixing commands and running most of them with higher privileges than are
strictly required.

Users coming from unix systems generally prefer not to do this and instead use
a utility known as `sudo` (Super-User Do) to run a specific command with
increased privileges.

This repo provides `eledo.exe` as a workalike to `sudo`; it will attempt to
increase privileges using the User Account Control mechanism built in to
Windows to prompt the user to confirm that it should run with increased
privileges.

This repo also provides `normdo.exe` (Normal User Do) that works in the
opposite way, dropping privileges back to normal levels to run a command.
This functionality is important for security-minded code that wishes to
run with lower (if not least!) privilege regardless of the privilege level
of the code that invoked it.

## How do I use the Rust crate?

There are two logical halves to this crate;

* Detecting the privilege level, including both *Elevation* and *High Integrity
  Administrative* privs, so that the embedding application can choose whether
  to surface this as an error, or to continue with the second half of the crate...

```rust
use deelevate::{Token, PrivilegeLevel};

let token = Token::with_current_process()?;
match token.privilege_level()? {
  PrivilegeLevel::NotPrivileged => {
    // No special privs
  }
  PrivilegeLevel::Elevated => {
    // Invoked via runas
  }
  PrivilegeLevel::HighIntegrityAdmin => {
    // Some other kind of admin priv.
    // For example: ssh session to Windows 10 SSH server
  }
}
```

* Re-executing the application with altered privs, while passing the stdio
  streams and process exit status back to the original parent.

```rust
use deelevate::spawn_with_normal_privileges;
use deelevate::spawn_with_elevated_privileges;

// If we have admin privs, this next line will either spawn a version
// of the current process with reduced privs, or yield an error trying
// to do that.
// The spawn_with_elevated_privileges function works similarly, except
// that it will only return when the calling process has elevated
// privs.
spawn_with_normal_privileges()?;

// If we reach this line it is because we don't have any special privs
// and we can therefore continue with our normal operation.
```

The `show` example demonstrates testing for the privilege level.

The `spawn` example demonstrates re-executing the process at a lower priv level.

## Caveats?

There are some privilege levels that are not mapped as privileged from the
perspective of this crate.  The rationale for this is that those levels are
unusual enough that they are probably not humans and probably should not have
this crate adjusting the privilege level.

It may feel like this might be a security concern, but its worth noting that:

* The calling code already has equal or higher privilege (so no escalation is possible)
* This crate is intended for convenience and consistency for human users

## Utilities

This crate provides `normdo.exe` for running a command with normal privileges,
and `eledo.exe` for running a command with elevated privileges.  Unlike other
elevation solutions, both of these utilities are designed to run from inside
a console and to keep the output from the target application in that console.
In addition, these tools use the PTY APIs in order to support running terminal
applications such as pagers and editors (vim.exe!) correctly!

Both of these tools require that the `eledo-pty-bridge.exe` be installed
alongside them, or otherwise be in the PATH.  The bridge process is required
to host the PTY and spawn the program in the alternatively privileged
context.

### `eledo.exe`

*Runs a program with elevated privs*

```
eledo.exe PROGRAM [ARGUMENTS]
```

`eledo.exe` will check to see if the current context has admin privileges;
if it does then it will execute the requested `PROGRAM` directly, returning
its exit status.

Otherwise, `eledo.exe` will arrange to run the program with an elevated PTY
that is bridged to the current terminal session.  Elevation requires that the
current process be able to communicate with the shell in the current desktop
session, and will typically trigger a UAC prompt for that user.

```
> eledo.exe whoami /groups

GROUP INFORMATION
-----------------

Group Name                                                    Type             SID          Attributes
============================================================= ================ ============ ===============================================================
Everyone                                                      Well-known group S-1-1-0      Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\Local account and member of Administrators group Well-known group S-1-5-114    Mandatory group, Enabled by default, Enabled group
BUILTIN\Administrators                                        Alias            S-1-5-32-544 Mandatory group, Enabled by default, Enabled group, Group owner
BUILTIN\Performance Log Users                                 Alias            S-1-5-32-559 Mandatory group, Enabled by default, Enabled group
BUILTIN\Users                                                 Alias            S-1-5-32-545 Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\INTERACTIVE                                      Well-known group S-1-5-4      Mandatory group, Enabled by default, Enabled group
CONSOLE LOGON                                                 Well-known group S-1-2-1      Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\Authenticated Users                              Well-known group S-1-5-11     Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\This Organization                                Well-known group S-1-5-15     Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\Local account                                    Well-known group S-1-5-113    Mandatory group, Enabled by default, Enabled group
LOCAL                                                         Well-known group S-1-2-0      Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\NTLM Authentication                              Well-known group S-1-5-64-10  Mandatory group, Enabled by default, Enabled group
Mandatory Label\High Mandatory Level                          Label            S-1-16-12288
```

### `normdo.exe`

*Runs a program with normal privs*

```
normdo.exe PROGRAM [ARGUMENTS]
```

`normdo.exe` will check to see if the current context has admin privileges;
if it does *not* then it will execute the requested `PROGRAM` directly, returning
its exit status.

Otherwise, `eledo.exe` will arrange to run the program with a Normal user token
with Medium integrity level, dropping/denying the local administrator group
from the current token.  The program will be run in a PTY that is bridged to
the current terminal session.

```
> normdo.exe whoami /groups

GROUP INFORMATION
-----------------

Group Name                                                    Type             SID          Attributes
============================================================= ================ ============ ==================================================
Everyone                                                      Well-known group S-1-1-0      Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\Local account and member of Administrators group Well-known group S-1-5-114    Group used for deny only
BUILTIN\Administrators                                        Alias            S-1-5-32-544 Group used for deny only
BUILTIN\Performance Log Users                                 Alias            S-1-5-32-559 Mandatory group, Enabled by default, Enabled group
BUILTIN\Users                                                 Alias            S-1-5-32-545 Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\INTERACTIVE                                      Well-known group S-1-5-4      Mandatory group, Enabled by default, Enabled group
CONSOLE LOGON                                                 Well-known group S-1-2-1      Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\Authenticated Users                              Well-known group S-1-5-11     Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\This Organization                                Well-known group S-1-5-15     Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\Local account                                    Well-known group S-1-5-113    Mandatory group, Enabled by default, Enabled group
LOCAL                                                         Well-known group S-1-2-0      Mandatory group, Enabled by default, Enabled group
NT AUTHORITY\NTLM Authentication                              Well-known group S-1-5-64-10  Mandatory group, Enabled by default, Enabled group
Mandatory Label\Medium Mandatory Level                        Label            S-1-16-8192
```

## Thanks

The elevator icons embedded into the utilities were made by <a href="https://www.flaticon.com/authors/pixel-perfect" title="Pixel perfect">Pixel perfect</a> from <a href="https://www.flaticon.com/" title="Flaticon"> www.flaticon.com</a>
