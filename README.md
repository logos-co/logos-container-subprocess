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

Two static libraries, split by dependency surface:

| Library | Side | Source | Depends on |
|---------|------|--------|-----------|
| `logos_container_subprocess` | parent | `subprocess_container.{h,cpp}` | Boost.Process, spdlog, logos-container. |

The parent (`SubprocessContainer`) launches and supervises module processes via
Boost.Process v2, relays their stdout/stderr line-by-line (with a bounded
buffer so a module can't OOM the host), detects crashes, and delivers the auth
token to the child. The child (`SubprocessTokenReceiver`) receives that token
over a `QLocalServer` socket. Both sides authenticate the peer by uid (and pid
where available) so a same-host attacker cannot intercept or inject the token.

Supporting headers (`peer_credentials.h`, `unix_socket_path.h`,
`path_safety.h`) are the shared handoff helpers.

## Build & test

```bash
nix build .#logos-container-subprocess          # both libs + headers
nix build .#checks.aarch64-linux.tests -L       # run the container tests
```

