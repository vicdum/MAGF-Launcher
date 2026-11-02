<p align="center">
  <img alt="MAGF Launcher" src="/program_info/io.github.vicdum.MAGFLauncher.logo.svg" width="40%">
</p>

# MAGF Launcher

A slimmed-down Minecraft launcher, forked from [Prism Launcher](https://github.com/PrismLauncher/PrismLauncher).

MAGF Launcher keeps the core of Prism Launcher — instance management, Microsoft accounts, Java management, mod loaders — and drops online modpack/mod browsing and extra themes.

## Building

See Prism Launcher's [build instructions](https://prismlauncher.org/wiki/development/build-instructions/); the same steps apply. The `Containerfile` in this repository provides a ready-to-use Linux build environment.

## Forking / redistributing

If you redistribute builds, register your own Microsoft (MSA) client ID and set `Launcher_MSA_CLIENT_ID` when configuring CMake.

## License

GPL-3.0, same as Prism Launcher. See [LICENSE](LICENSE) and [COPYING.md](COPYING.md). Original work © Prism Launcher, PolyMC and MultiMC contributors.
