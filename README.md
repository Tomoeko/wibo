# wibo

A minimal, low-fuss wrapper that can run simple command-line 32-bit Windows binaries on Linux and macOS - developed to run Windows compilers faster than Wine.

Download the latest release from [GitHub releases](https://github.com/decompals/wibo/releases) or build from source.

Available builds:

- `wibo-i686`: Linux x86 (static binary)
- `wibo-x86_64`: Linux x86_64 (static binary, experimental)
- `wibo-macos`: macOS x86_64 (experimental, Rosetta 2 supported)

## Building

```sh
cmake --preset debug
cmake --build --preset debug
```

This will produce an x86 (32-bit) debug Linux binary at `build/debug/wibo`.

Available presets:

- `debug`: Debug Linux x86
- `release`: Release Linux x86
- `debug64`: Debug Linux x86_64
- `release64`: Release Linux x86_64
- `debug-macos`: Debug macOS x86_64
- `release-macos`: Release macOS x86_64
- `debug64-macos`: Debug macOS host for x86-64 Windows guests
- `release64-macos`: Release macOS host for x86-64 Windows guests

Build the matching runner for the Windows executable's guest architecture.
Both runners can coexist, so a workflow that uses a mixture of PE32 and PE32+
command-line programs does not need separate source trees:

```sh
cmake --preset release-macos
cmake --build --preset release-macos --parallel 12
cmake --preset release64-macos
cmake --build --preset release64-macos --parallel 12
./build/release/wibo path/to/program32.exe
./build/release64/wibo64 path/to/program64.exe
```

Win64 Wibo embeds PE32+ Wine CRT modules. The build automatically discovers
Wine Stable's `x86_64-windows` directory on macOS; another installation can be
selected without invoking Wine by setting `WIBO_WIN64_CRT_ROOT` during CMake
configuration. The configuration rejects the upstream 32-bit-only CRT assets
for a Win64 guest.

Callers that require bounded execution should launch each Wibo instance in its
own process group. They can then terminate the complete group on timeout,
including any guest-created child processes.

## Usage

```sh
wibo [options] <program.exe> [arguments...]
wibo path [subcommand options] <path> [path...]
```

### General Options

| Option          | Description                       |
| --------------- | --------------------------------- |
| `-h, --help`    | Show usage information and exit   |
| `-V, --version` | Show version information and exit |

### Runtime Options

| Option             | Description                                                                                                      |
| ------------------ | ---------------------------------------------------------------------------------------------------------------- |
| `-C, --chdir DIR`  | Change working directory before launching the program                                                            |
| `-D, --debug`      | Enable debug logging (same as `WIBO_DEBUG=1`)                                                                    |
| `--cmdline STRING` | Use `STRING` as the exact guest command line (must include the program name, e.g. `"test.exe a b c"`)            |
| `--`               | Stop option parsing; following arguments are used verbatim as the guest command line, including the program name |

### Environment

| Variable         | Description |
| ---------------- | ----------- |
| `WIBO_C_DRIVE`   | Host directory exposed as the guest `C:` drive; unset preserves the historical host-root mapping |
| `WIBO_PATH`      | Host path list searched for guest executables and DLLs before compatible fallback paths |
| `WIBO_DEBUG`     | Enable runtime diagnostics when set |
| `WIBO_DEBUG_HEAP` | Enable guest-heap diagnostics when set |
| `WIBO_CRASH_LOG` | Append a minimal fatal-signal record on supported macOS PE32+ builds |

### Subcommands

| Subcommand | Description                                                                       |
| ---------- | --------------------------------------------------------------------------------- |
| `path`     | Convert between host and Windows-style paths (see `wibo path --help` for details) |

### Examples

#### Normal usage

```sh
wibo path/to/test.exe a b c
wibo -C path/to test.exe a b c
```

#### Advanced: full control over the guest command line

```sh
wibo path/to/test.exe -- test.exe a b c
wibo --cmdline 'test.exe a b c' path/to/test.exe
wibo -- test.exe a b c
```

## Tests

Self-checking Windows fixtures run through CTest. They require a 32-bit MinGW cross toolchain (`i686-w64-mingw32-gcc` and `i686-w64-mingw32-windres`).

On macOS: use `brew install mingw-w64`.

```sh
ctest --preset debug
```

This will cross-compile the fixture executables, run them through `wibo`, and fail if any WinAPI expectations are not met.

## Related Projects

- [taviso/loadlibrary](https://github.com/taviso/loadlibrary) - Initial inspiration for this project.
- [evmar/retrowin32](https://github.com/evmar/retrowin32) - A similar project with different goals and architecture.
- [decomp.me](https://decomp.me) - Collaborative decompilation website; uses wibo to run Windows compilers.

## License

wibo is licensed under the MIT License. See [`LICENSE`](LICENSE) for details.

Optionally, wibo embeds custom Wine CRT DLL builds from [encounter/winedll](https://github.com/encounter/winedll). To disable, set `-DWIBO_ENABLE_WINE_DLLS=NO`.

Wine is licensed under the LGPLv2.1+. See [`winedll/LICENSE`](https://github.com/encounter/winedll/blob/main/LICENSE) and [`winedll/COPYING.LIB`](https://github.com/encounter/winedll/blob/main/COPYING.LIB) for details.
