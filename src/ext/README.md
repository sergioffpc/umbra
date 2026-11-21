# ext

Bridges to third-party libraries that need RTTI or exceptions. Each bridge is
the module `umbra::ext::<dependency>`, with its sources in
`src/ext/<dependency>/` and its public headers in
`include/umbra/ext/<dependency>/`, defined with `umbra_add_ext_module`.

Rules:

- Bridges are the only targets built with `-frtti` and `-fexceptions`.
- A bridge is the only code that includes the headers of its library.
- Its public headers expose only umbra types, never the library's.
- Every exception the library may throw is caught inside the bridge and
  returned as `std::expected` with the core error type.
