<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="artwork/droiddeck-banner-dark.svg">
    <img alt="DroidDeck" src="artwork/droiddeck-banner-light.svg" width="100%">
  </picture>
</p>

# DeckDroid-Quest3

A **fork** of [DroidDeck](https://github.com/Droid-Deck/DroidDeck) by [@yomiplush](https://github.com/yomiplush), focused on running Valve's Steam client on the **Meta Quest 3** (Snapdragon 8 Gen 2 / Adreno 740). This repository is **not** the upstream project and is **not** affiliated with the DroidDeck team.

## Changes on top of upstream

- **KGSL compatibility shim** (`app/src/main/cpp/kgsl_compat.c`): Horizon OS denies Turnip's legacy KGSL allocation ioctls. The shim translates `GPUMEM_ALLOC_ID` / `GPUMEM_FREE_ID` into the `GPUOBJ` ABI and emulates the legacy timestamp query through `WAITTIMESTAMP_CTXTID`, so gamescope, Steam and Proton can draw on the Quest 3. It is loaded with `-z global` so the driver's own `ioctl` resolves to it.
- **Quest 3 render profile**: the Quest reports a 4128×2208 panel while its app Surface is much smaller. The guest resolution is capped to a 1920-px long side, and the compositor is kept on a plain linear blit (the shader upscaler and post chain are disabled on this device) to avoid a corrupted/noisy picture.
- **Quest 3 suspend policy**: the Quest reports its panel as non-interactive while DroidDeck is on screen; the session no longer treats that as "screen off" and stops suspending gamescope.

## License

Unchanged **GPL-3.0** — see [LICENSE](LICENSE). Third-party components keep their own licenses: `uruntime` (MIT, `tools/linuxfs/licenses/uruntime-LICENSE`), the DXBC translator (zlib, `app/src/main/cpp/thirdparty/dxbc/LICENSE.md`), adrenotools / linkernsbypass, and the Win-FG notices. LSFG frame generation ships **no** Lossless Scaling shaders (you need your own copy of Lossless Scaling). Steam and Proton belong to Valve Corporation.

## CI

This fork inherits upstream's GitHub Actions workflows, which reference upstream secrets that do not exist here. Actions are therefore **disabled** in this repository; re-enable from Settings → Actions only after adding your own secrets.

---

DroidDeck brings the SteamOS experience to Android: Valve's Steam client in Big Picture on your Adreno handheld, with Windows games through Valve's ARM64 Proton.

<p align="center"><a href="https://discord.gg/JRGAvawjsm"><img src="https://img.shields.io/badge/Discord-Join%20the%20community-5865F2?logo=discord&logoColor=white" alt="Join the DroidDeck Discord"></a></p>

<p align="center"><img src="docs/releases/media/0.2.0/launch-into-steam.gif" width="80%" alt="Tapping DroidDeck on the Android home screen and landing in Steam Big Picture"></p>

> Note: DroidDeck does not have a stand-alone website. Do not click on any download links from websites claiming to be the DroidDeck team. 

## Requirements and install

Use Android 9 or newer on a supported Adreno device (730 or newer, or 8xx). Adreno 6xx is experimental: DirectX 11 uses DXVK 2 and may run, and DirectX 12 games can still crash. Mali, Xclipse, PowerVR, and Adreno 710 are unsupported. No root is required. Allow about 3 GB for the runtime and 1.1 GB more for the desktop and emulators. Install the APK from [Releases](https://github.com/Droid-Deck/DroidDeck/releases), install the Linux runtime, then press **Play** and sign in. Steam downloads on first launch. Install **Desktop & apps** to use the desktop and emulators. The **Store** installs Linux apps and games from Flathub (ARM64 builds) with Flatpak; with additional options to install Appimages and set up scripts. **Stores** (Setup › Stores › "Show Stores in the rail") signs in to GOG, Epic Games and Amazon Games, browses their catalogs and libraries, downloads games into the Games storage and adds each one to the Steam client as a non-Steam game, so it launches through Proton like everything else; the cog on its chip row sets which tab a store opens on and the download speed tier.

Before Steam launches, you must turn off **Restrict child processes** in Developer options. If this option is not available in developer settings (Android 12 and 13 devices), first launch of Steam will present a "Fix it for me" button, which will help automate the setup process.

## Community

Join the [DroidDeck Discord](https://discord.gg/JRGAvawjsm) for help, Preview builds, and device reports. Bug reports go in its **#bug-reports** forum; attach the zip from **Share logs** (Setup, or the session drawer) so the logs come with it.

## Build

Run `tools/build_local.sh` with Docker, Java 17, the Android SDK/NDK, Rust through rustup, GitHub CLI, Python 3, and `zstd` installed. It prepares the same native libraries, pinned components and audio sinks as CI, then checks the APK's assets, JNI exports and dependencies. The APK is at `app/build/outputs/apk/release/app-release.apk`. Set `DROIDDECK_PA13_SOURCE_DIR` to an existing PulseAudio 13.0 source directory to skip downloading it. Set `DROIDDECK_SIGNING_ENV` to a signing environment file to install over a release build; relative keystore paths resolve beside that file. To install on an attached device, run `tools/deploy_local.sh`.

After preparation, ordinary `./gradlew :app:assembleRelease` builds can reuse the verified inputs. If native sources, component pins or staged files change, packaging stops with instructions to rerun the helper. Generated audio stays under `app/build/`; the tracked base bundle is never modified. `./gradlew -PskipRust=true :app:testDebugUnitTest` needs no native preparation. For an APK without store engines, use `DROIDDECK_SKIP_RUST=1 tools/build_local.sh`; it omits cached engine libraries too.

Run the helper and preload regression suite with `bash tools/test_local.sh`. On macOS it runs in Linux through Docker, matching the helpers' runtime and CI instead of compiling Linux preloads against macOS headers.

## Limits

Compatibility and performance vary by device; hardware validation is limited. Desktop compositing uses software rendering. Firefox sandboxing is reduced under proot. Logs are kept app-private (`files/logs/`, the last 30 sessions); **Share logs** packs the newest session, with the stores and tool logs, into one scrubbed zip.

## Credits and licence

GPL-3.0. Runtime, shim, input, and controller work build on WinNative and Bannerlator (maxjivi05). LSFG frame generation is from the work of Camille LaVey and the [Eden](https://eden-emu.dev) emulator project, following [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk), ported to WinNative and DroidDeck by [@maxjivi05](https://github.com/maxjivi05); it needs your own copy of [Lossless Scaling](https://store.steampowered.com/app/993090/) and ships none of its shaders. x86 AppImages, and any whose own runtime cannot unpack them, are unpacked with [uruntime](https://github.com/VHSgunzo/uruntime) by VHSgunzo (MIT), shipped unmodified with its licence. See [LICENSE](LICENSE). Steam and Proton belong to Valve Corporation; this project is not affiliated with Valve.
