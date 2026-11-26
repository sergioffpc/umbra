// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Safety annotation macros. They give clang the information its lifetime,
// nullability, thread-safety and buffer-usage analyses need; the warnings
// those analyses emit are errors (see cmake/UmbraSafety.cmake).

#ifndef UMBRA_CORE_SAFETY_H_
#define UMBRA_CORE_SAFETY_H_

// Lifetimes.

// Marks a parameter (or, after a member function's parameter list, `this`)
// whose referent the return value may refer to. Required on every function
// that returns a reference, pointer, span or view derived from `this` or a
// parameter.
#define UMBRA_LIFETIMEBOUND [[clang::lifetimebound]]

// Marks a parameter whose referent is stored in `entity` (a parameter name,
// or `global`), so it must outlive it.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): wraps an attribute.
#define UMBRA_LIFETIME_CAPTURE_BY(entity) [[clang::lifetime_capture_by(entity)]]

// Marks a parameter of a member function whose referent the object stores,
// so it must outlive the object.
#define UMBRA_LIFETIME_CAPTURE_BY_THIS [[clang::lifetime_capture_by_this]]

// Marks a class that owns the data it refers to (a container).
#define UMBRA_OWNER [[gsl::Owner]]

// Marks a class that refers to data owned elsewhere (a view or iterator).
#define UMBRA_POINTER [[gsl::Pointer]]

// Nullability.

// Pointers declared between these macros are non-null unless marked
// UMBRA_NULLABLE. Every header wraps its declarations in them.
#define UMBRA_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define UMBRA_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")

// Marks a pointer that may be null: `int* UMBRA_NULLABLE p`.
#define UMBRA_NULLABLE _Nullable

// Thread safety.

// Marks a class as a capability (a lock) named `name` in diagnostics.
#define UMBRA_CAPABILITY(name) __attribute__((capability(name)))

// Marks an RAII class that holds a capability for its lifetime.
#define UMBRA_SCOPED_CAPABILITY __attribute__((scoped_lockable))

// Marks data that may be accessed only while holding `capability`.
#define UMBRA_GUARDED_BY(capability) __attribute__((guarded_by(capability)))

// Marks a pointer whose pointee may be accessed only while holding
// `capability`.
#define UMBRA_PT_GUARDED_BY(capability) \
  __attribute__((pt_guarded_by(capability)))

// Marks a function that must be called while holding the capabilities.
#define UMBRA_REQUIRES(...) __attribute__((requires_capability(__VA_ARGS__)))

// Marks a function that must be called without holding the capabilities.
#define UMBRA_EXCLUDES(...) __attribute__((locks_excluded(__VA_ARGS__)))

// Marks a function that acquires the capabilities (or `this`).
#define UMBRA_ACQUIRE(...) __attribute__((acquire_capability(__VA_ARGS__)))

// Marks a function that releases the capabilities (or `this`).
#define UMBRA_RELEASE(...) __attribute__((release_capability(__VA_ARGS__)))

// Unsafe buffer usage.

// Pointer arithmetic and raw indexing are allowed only between these macros,
// immediately preceded by a `// SAFETY:` comment explaining why the access is
// sound.
#define UMBRA_UNSAFE_BUFFER_BEGIN _Pragma("clang unsafe_buffer_usage begin")
#define UMBRA_UNSAFE_BUFFER_END _Pragma("clang unsafe_buffer_usage end")

#endif  // UMBRA_CORE_SAFETY_H_
