<div align="center">
 <img src="website/static/img/logo.png" width="20%" height="20%" alt="watchman-logo">
 <h1>Watchman</h1>
 <h3>A file watching service.</h3>
</div>

## Purpose

Watchman exists to watch files and record when they actually change. It can
also trigger actions (such as rebuilding assets) when matching files change.

## Documentation

Head on over to https://facebook.github.io/watchman/

## installation
 System Requirements:

* Linux systems with `inotify`
* macOS (uses `FSEvents` on 10.7+, `kqueue(2)` on earlier versions)
* Windows 10 (64-bit) and up. Windows 7 support is provided by community patches.


Prebuilt Binaries: 

* Download and extract the windows release from the latest release here: https://github.com/facebook/watchman/releases/tag/v2024.02.12.00
* It will be named something like watchman-vYYYY.MM.DD.00-windows.zip
* It contains a bin folder. Move that somewhere appropriate and update your PATH environment to reference that location.

Installing via Chocolatey:

Watchman is available via the Chocolatey Windows package manager. Installation is as simple as:

PS C:\> choco install watchman

for more information about Chocolatey:

here: https://chocolatey.org/install

for linux and MacOS goes to the next link.

link: https://facebook.github.io/watchman/docs/install.

## License

Watchman is made available under the terms of the MIT License. See the
LICENSE file that accompanies this distribution for the full text of the
license.

## Support

Watchman is primarily maintained by the source control team at Meta Platforms, Inc. We support:

* Windows and macOS builds
* Linux builds on recent Ubuntu and Fedora releases
* Watchman's [compatibility commitment](https://facebook.github.io/watchman/docs/compatibility.html)
* Python, Rust, and JavaScript clients

Support for additional operating systems, release packaging, and language bindings is community-maintained:

* Homebrew
* FreeBSD
* Solaris

Please submit a [GitHub issue](https://github.com/facebook/watchman/issues/) to report any troubles.

## Contributing

Please see the [contributing guide](https://facebook.github.io/watchman/contributing.html).
