# Prebuilt tools of the deps image

The deps image is the development environment. Its best HDR viewer for
comparing renders, tev, follows none of the image's rules for what it
builds. It is GPL-3.0-only from version 2, off the submodules' permissive
allow-list. It is not in Ubuntu, and its Linux release is an AppImage built
with GCC, so it needs libstdc++ and libgcc_s, which nothing else under
`/opt/umbra` may need. Building it from source against libc++ means
building and patching the two dozen libraries it vendors. This decision
takes such a tool prebuilt instead, and keeps it apart from everything
umbra uses.

## The rule

The deps image may carry a tool as its upstream release binary, outside the
submodules and the dependency build, when all of these hold:

- it is a program that a developer runs in the deps image, and umbra
  neither links it nor loads it nor runs it;
- `docker/deps.Dockerfile` downloads it from the upstream release, pinned by
  version and SHA-256, into `/opt/<tool>`, never under `/opt/umbra`;
- it is never part of a release image, which carries only umbra and what
  umbra loads (see `0005-releases-are-a-chiseled-image.md`).

Such a tool may then have a copyleft license and may need a GCC runtime.
`third_party/versions.md` lists it under **Prebuilt tools** with its version,
source and license, and its system libraries in the Ubuntu packages table.
The toolchain image already carries one tool of this kind: cuda-gdb,
GPL-3.0-or-later.

## Why it is safe

umbra's license stays Apache-2.0 because no copyleft code reaches a program
built from umbra. The tool is a separate program, and only the developer
runs it. The release image is assembled from what umbra loads, so a tool
cannot reach it: a library there that the table of components does not
cover stops the install.

umbra stays free of the GCC runtime too. The tool lives outside
`/opt/umbra`, which `forge no-gcc-runtime` checks in the deps image. umbra,
its build trees and the dependency build never link anything from the
tool's directory. libstdc++ and libgcc_s are in the image anyway, because
Ubuntu's own packages need them; the rule keeps them out of what umbra
builds, not out of the image.

## Consequences

- Choosing a tool no longer depends on its license or its C++ runtime, only
  on where it runs.
- The image does not rebuild the tool. A new version is a new pinned
  version and SHA-256 in `docker/deps.Dockerfile`, which changes the deps
  image's tag; `forge bump-submodules` does not see it.
- The tool's source is the upstream tag its release was built from, which
  `third_party/versions.md` names; the image does not carry it.
- A library is still never taken this way: what umbra links, the
  dependency build builds from a submodule with a permissive license.
