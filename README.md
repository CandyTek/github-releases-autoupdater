A C++ library for checking GitHub releases, downloading a Windows installer, and launching it with the Win32 API.
The project is Windows-only and builds as a standard CMake project targeting MSVC + Ninja, with no Qt dependency.

# Usage

1. Create an instance of the updater class. Specify your repository name and a string representation of the current version of the software:
  `_updater("VioletGiraffe/file-commander", "0.9.1")`
2. Specify the class that will receive update notification (via the `CAutoUpdaterGithub::UpdateStatusListener` interface):
  `_updater.setUpdateStatusListener(this);`
3. Call `checkForUpdates()`
4. The `onUpdateAvailable(CAutoUpdaterGithub::ChangeLog changelog)` callback will be called when the GitHub releases request completes. If any updates were found, the `changelog` vector will be non-empty. You can use its items to retrieve the update details. If it's empty, it means no updates are available.
5. Call `downloadAndInstallUpdate()` to download the update and launch it.

# Building

Prerequisites:
* Windows.
* CMake 3.21+.
* Ninja.
* MSVC toolchain.

From a Developer PowerShell / Native Tools prompt:

```powershell
cmake --preset msvc-ninja-debug
cmake --build --preset build-debug
```

Release build:

```powershell
cmake --preset msvc-ninja-release
cmake --build --preset build-release
```

The library links against the Windows SDK only.
