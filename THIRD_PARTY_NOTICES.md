# Third-Party Notices

TLockGUI itself is licensed under the MIT License in `LICENSE`.

The Windows binary package redistributes the following third-party components. They remain subject to their own licenses; inclusion here does not change those licenses.

## drand/tlock `tle.exe`

- Project: https://github.com/drand/tlock
- Copyright: Copyright (c) 2022 drand team
- License: dual-licensed under Apache License 2.0 or MIT License
- This package uses the MIT option. A copy is provided in `licenses/tlock-LICENSE-MIT.txt`.

TLockGUI invokes `tle.exe` as a separate process and does not reimplement or modify its cryptography.

## Qt 6.8.3

- Project: https://www.qt.io/
- Source: https://download.qt.io/official_releases/qt/6.8/6.8.3/submodules/
- Open-source licensing: GNU Lesser General Public License v3 and, depending on the module, GNU General Public License v2/v3

TLockGUI dynamically links to the Qt libraries deployed beside the executable. The Windows binary package includes the Qt license bundle and SPDX software-bill-of-materials files for the deployed Qt modules. You may replace the compatible Qt DLLs in the application directory with your own builds.

## MinGW-w64 / GCC runtime

- Project: https://www.mingw-w64.org/
- Project: https://gcc.gnu.org/

The Windows binary package contains compiler runtime DLLs installed by Qt's `windeployqt`. It includes the applicable GCC Runtime Library Exception, GCC license, MinGW-w64 runtime license, and winpthreads license texts in its `licenses` directory.

This notice is informational and is not legal advice.
