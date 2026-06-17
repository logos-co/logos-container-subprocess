# logos-container-subprocess

The **subprocess container** for the Logos module runtime: a
`LogosCore::ModuleContainer` implementation that isolates each module in its own
child process, with a hardened parent↔child auth-token handoff over a Unix
domain socket.

It implements the contract defined in
[`logos-container`](https://github.com/logos-co/logos-container) and depends
only on that contract — not on `logos-liblogos`. Swapping in a different
isolation mechanism (Docker, in-process, WASM) means writing a sibling repo that
implements the same `ModuleContainer` interface; the core is unaffected.

```
logos-container                 (ModuleContainer interface + descriptor types)
   └─ logos-container-subprocess <-- this repo
         consumed by logos-liblogos
```

## What's here

A single Qt-free static library, `logos_container_subprocess`:

| Source | Role |
|--------|------|
| `subprocess_container.{h,cpp}` | `SubprocessContainer` — the `ModuleContainer` implementation. |
| `subprocess_container_factory.cpp` | Defines `LogosCore::makeContainer()` (the logos-container factory seam) → a `SubprocessContainer`, so a consumer selects this container at link time without naming it. |

`SubprocessContainer` launches and supervises module processes via Boost.Process
v2, relays their stdout/stderr line-by-line (with a bounded buffer so a module
can't OOM the host), detects crashes, and delivers the auth token to the child
over the child's **inherited stdin pipe** (the parent writes the token and closes
it). There is no child-side library and no socket: the child just reads its token
from stdin (handled by `logos_host`'s `TokenSource`), so there is no predictable
filesystem path to squat and no peer-credential dance.

### Consuming it

This package installs a generic CMake config so a consumer selects "the container
implementation" without naming this one:

```cmake
find_package(LogosContainerImpl REQUIRED)   # provided by this package
target_link_libraries(your_target PRIVATE LogosContainerImpl::impl)
```

`LogosContainerImpl::impl` carries the static library plus its own deps
(Boost.Process, spdlog, nlohmann_json). A different container implementation that
ships the same `LogosContainerImpl` config is a drop-in replacement.

## Build & test

```bash
nix build .#logos-container-subprocess          # both libs + headers
nix build .#checks.aarch64-linux.tests -L       # run the container tests
```

