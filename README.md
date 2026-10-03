# SlackBox

SlackBox is an Android application-container project built on the existing BlackBox-derived virtualization core in this repository. It runs isolated secondary application environments in the host application's sandbox; it is not an emulator, hypervisor, or a replacement for Android's multi-user system.

## Current implementation

The core currently provides these implemented subsystems:

- Per-instance (`userId`) package state, application data, device-encrypted data, databases, files, cache, external-app data, and OBB paths.
- APK installation from storage or a host-installed package, virtual package resolution, and component discovery.
- Stub-process based activity, service, receiver, pending-intent, and content-provider dispatch through Binder interfaces.
- Process start/stop and lifecycle bookkeeping through `BProcessManagerService`.
- A persistent virtual-instance registry. Instance names and creation time are stored in the existing user registry; instance metadata has a dedicated `instances/<userId>/metadata` directory. Deleting an instance removes its credential-encrypted, device-encrypted, external, and metadata roots after its installed packages have been stopped and uninstalled.
- A functional host UI for listing virtual environments, selecting an APK, installing, launching, stopping, clearing data, and uninstalling apps.

The host labels and build artifacts are named **SlackBox**. Internal Java package names remain BlackBox-derived while the compatibility migration is in progress; renaming those APIs would break the generated reflection and Binder contracts.

## Android 16 and ARM64

The Gradle build targets Android 16 (API 36) and compiles against API 36. The primary supported ABI is `arm64-v8a`; `armeabi-v7a` remains packaged because the current proxy-process design can host compatible 32-bit guest APKs on devices that still provide a 32-bit runtime.

Android's application sandbox limits what a userspace container can do. In particular, SlackBox cannot provide kernel-level isolation, run arbitrary privileged system services, or guarantee that every app works under hidden-API and background-execution restrictions. Apps that require Play Integrity, hardware attestation, anti-cheat checks, or virtualization detection are not supported targets; SlackBox does not bypass those controls.

Guest activities render through normal Android windows and surfaces in the host's proxy processes. Hardware-accelerated GLES/Vulkan rendering therefore uses the device GPU when the guest and host Android framework allow it. No software renderer or fake GPU path is provided.

## Architecture

| Layer | Responsibility |
| --- | --- |
| `app` | Host UI, APK selection, instance selection, lifecycle commands and diagnostics. |
| `Bcore` | Virtual package manager, process manager, activity/service/provider/receiver dispatch, storage redirects, Binder hooks and compatibility helpers. |
| `black-reflection` | Runtime wrappers for hidden/framework APIs used by the virtualization layer. |
| `compiler` | Annotation processor that generates reflection wrappers. |

The isolation boundary is the virtual `userId`. Package data is rooted under `data/user/<userId>/<package>`, device-encrypted data under `data/user_de/<userId>/<package>`, and virtual external data under the matching virtual external-user root. Package install state is independently tracked per user, so one instance cannot resolve another instance's private data path.

## Build

Install Android SDK Platform 36, Build Tools 36, Android NDK `29.0.13846066`, and a JDK compatible with Android Gradle Plugin 8.13.2. Point `ANDROID_HOME` at that SDK (or create an untracked `local.properties` with `sdk.dir=/absolute/path/to/android-sdk`).

```sh
./gradlew :app:assembleDebug
./gradlew :app:testDebugUnitTest
./gradlew :app:assembleRelease
```

The debug artifact is emitted as `app/build/outputs/apk/**/SlackBox_*.apk`.

## Device validation

Validate on the iQOO Neo 10R (Snapdragon 8s Gen 3, Android 16, 12 GB RAM):

1. Create two virtual instances and install the same APK in both.
2. Verify files, preferences, databases, cache, external-app storage and package state do not cross instance boundaries.
3. Launch an activity, rotate, resize/recreate the surface, use touch, multi-touch, keyboard, Back and volume controls, then return to the host.
4. Start and stop a service, trigger supported in-app broadcasts/providers, force-stop a virtual package, and launch it again.
5. Restart SlackBox and verify both instances and their package state persist.
6. Delete an instance and verify its internal, device-encrypted, external and metadata directories are gone while the other instance still runs.

Use `adb logcat -s BlackBoxCore:B BProcessManager:B BPackageManagerService:B SlackBox:D` for core lifecycle and package diagnostics. Do not include guest application data in shared logs or bug reports.
