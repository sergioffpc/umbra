# ext

Bridges to third-party libraries that need RTTI or exceptions. Each bridge is
the module `umbra::ext::<dependency>`, with its sources in
`src/ext/<dependency>/` and its public headers in
`include/umbra/ext/<dependency>/`, defined with `umbra_add_ext_module`.

The OpenUSD bridge, `umbra::ext::usd`, initializes OpenUSD and its plugins,
routes Tf's diagnostics into the log, and opens stages for the render
delegate, `umbra::delegate` (`src/delegate/`).

Rules:

- Bridges and the render delegate (`src/delegate/`) are the only targets
  built with `-frtti` and `-fexceptions`.
- Bridges and the render delegate are the only targets with default
  visibility in Release: the library's classes live in its shared objects,
  which hold subclasses outside umbra's LTO unit and the type_info that RTTI
  compares, so LTO must not devirtualize their calls nor umbra keep its own
  copy of their type_info.
- A bridge is the only code that includes the headers of its library,
  except OpenUSD's, which the render delegate includes too.
- Its public headers expose only umbra types, never the library's, except
  the OpenUSD bridge's `stage.h`, which hands the render delegate the stage
  it opens.
- Every exception the library may throw is caught inside the bridge and
  returned as `std::expected` with the core error type.
