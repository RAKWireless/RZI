# API documentation

Status: normative

Doxygen groups, `@since` / `@version`, and Zephyr toolchain attributes for
`include/rzi/` and production-library sources.

See also: [coding-standards.md](coding-standards.md).

## 1. Scope

Layering, naming, errors, and concurrency are in
[coding-standards.md](coding-standards.md) and
[error-codes.md](../api/error-codes.md). This document applies to every
RZI production-library C source and header under `include/rzi/` and `src/`.
The public C API includes functions, callbacks, structs, enums, constants,
and public types. Adding or changing a public API must update the matching
Doxygen annotations at the same time.

Public headers use Zephyr-style Doxygen and Zephyr toolchain attributes for
compile-time checks. Every production-library `.c` and `.h` file must include
`@file` and `@brief`. Private contracts, cross-file types, and cross-file
functions use Doxygen. Private implementations may use ordinary comments to
explain design reasons; mechanical Doxygen for every `static` function is
not required. `samples/` and `tests/` are not production-library files and
do not require file-level Doxygen.

## 2. Files and API groups

Every production-library C source and header must contain:

```c
/**
 * @file
 * @brief One-line file purpose.
 */
```

Every public header must also join a service group. Each service must define
a Doxygen group and record the first public version and the current API
version:

```c
/**
 * @defgroup rzi_service RZI service API
 * @brief One-line service purpose.
 * @since 0.1.0
 * @version 0.1.0
 * @{
 */
```

Submodules use `@ingroup` to join the owning service. The file must close
the group with `/** @} */`. Private headers and `.c` files must not define a
public group, so internal symbols stay out of the stable API reference.

`tools/ci/check-style.sh` scans `include/rzi/` and `src/`. Any production
library file missing `@file` or `@brief` fails the check.

Generate the HTML manual (public headers plus `doc/*.md`):

```bash
tools/docs/generate-doxygen.sh
```

Output is `doc/doxygen/html/index.html` and is not committed. The Doxyfile
is `doc/Doxyfile`.

## 3. Public functions

Every public function must document:

- `@brief`: one sentence for the operation;
- `@param`: direction, valid range, ownership, and lifetime of each
  parameter;
- `@return` or `@retval`: complete return semantics;
- thread context: whether ISR, callback, or concurrent calls are allowed;
- asynchronous semantics: whether `0` means "done" or "request accepted";
- whether data is copied before return.

Use multiple `@retval` tags when the return set is known and finite. Use
`@return` when the value is a range or cannot be enumerated. Do not repeat
both tags mechanically on the same function.

Example:

```c
/**
 * @brief Start or stop a join sequence using the stored credentials.
 *
 * A return value of zero means accepted; completion is reported through
 * on_event().
 *
 * @param start 1 to join, 0 to cancel retries, or RZI_LORAWAN_JOIN_KEEP.
 * @param auto_join 1 to join after boot, 0 to disable that, or
 *                  RZI_LORAWAN_JOIN_KEEP.
 * @param interval Seconds between attempts, or RZI_LORAWAN_JOIN_KEEP.
 * @param attempts Retries after the first attempt, or RZI_LORAWAN_JOIN_KEEP.
 *
 * @retval 0 Request accepted.
 * @retval -RZI_ERR_INVALID An argument is outside its accepted range.
 * @retval -RZI_ERR_NOT_READY Backend not ready.
 * @retval -RZI_ERR_NOT_SUPPORTED Activation mode unsupported.
 *
 * @note Thread context only; this function must not be called from an ISR.
 */
__must_check int rzi_lorawan_join(int32_t start, int32_t auto_join,
				  int32_t interval, int32_t attempts);
```

## 4. Constraint annotations

- `@note`: normal-use conditions such as thread context, data lifetime, and
  asynchronous behavior;
- `@warning`: conditions that can cause data loss, a security risk, or an
  irreversible result if ignored;
- `@pre`: caller-owned preconditions that the API cannot establish itself.

Do not use `@warning` for ordinary information. Do not write only "returns 0
on success" and omit concrete error paths.

Callbacks must state:

- the execution thread;
- whether an internal lock is held;
- whether re-entering RZI is allowed;
- the lifetime of pointer arguments;
- whether the callback may block.

## 5. Versioning and deprecation

- New public groups must have `@since` and `@version`. Group members inherit
  that `@since` and do not repeat it;
- `@since` is the release that introduced the group. It stays put.
  `@version` is that group's current contract version and changes when the
  contract changes. `RZI_VERSION_STRING` is the SDK release reported by AT,
  tags, and SBOMs. Before the first release all three are `0.1.0`;
- After that release, a type or function added in a later version has its
  own `@since`. A group changes `@version` when its contract changes;
- Deprecated APIs use both Doxygen `@deprecated` and compiler
  `__deprecated`;
- `@deprecated` must name the replacement and the major version that will
  remove it;
- A deprecated API stays at least one normal release cycle unless RZI has
  not yet promised a stable ABI.

Example:

```c
/**
 * @deprecated Since 0.3; use rzi_service_init(). Removed in 1.0.
 */
__deprecated int rzi_service_begin(void);
```

## 6. Compiler checks

Public headers take cross-compiler attribute macros from
`<zephyr/toolchain.h>`.

### `__must_check`

Functions that return an error code, a handle, or a result the caller must
consume must be marked `__must_check`:

```c
__must_check int rzi_service_init(void);
```

GCC `warn_unused_result` may still warn after a `(void)` cast. Callers
should handle the error. When an RZI private implementation truly cannot
act on a result, consume it through a named helper so review can see the
intent:

```c
static void ignore_result(int result)
{
    ARG_UNUSED(result);
}

ignore_result(rzi_service_stop());
```

Pure queries with no side effect when the result is ignored do not require
`__must_check`.

### `__printf_like(format_index, first_arg_index)`

Every printf-style variadic API must mark the format argument and the first
variadic argument:

```c
__printf_like(1, 2)
__must_check int rzi_at_publish_event(const char *format, ...);
```

Indexes are 1-based. `va_list` variants use `0` for the second index.

### `__deprecated`

Only public APIs that have entered the formal deprecation process may use
`__deprecated`. Do not use it to mark something as "temporarily
discouraged".

## 7. Do not use `__syscall`

`__syscall` is a Zephyr userspace system-call declaration, not a generic
documentation attribute. It requires `z_impl_*` implementations,
`z_vrfy_*` argument and permission checks, and syscall metadata.

Current RZI public APIs run in supervisor mode and must not add
`__syscall`. Add it later only after `CONFIG_USERSPACE=y` is an explicit
goal, object permissions and user-memory validation are complete, and the
interface has passed a security review.

## 8. Review checklist

Before submitting a public API change, confirm:

- [ ] Every production-library `.c` / `.h` file has `@file` and `@brief`;
- [ ] The public header's service group has `@brief`, `@since`, and
      `@version`;
- [ ] Every parameter documents range, direction, ownership, and lifetime;
- [ ] Synchronous return values are not confused with asynchronous
      completion;
- [ ] Public error codes have `@retval` or a single `@return`;
- [ ] Thread / ISR / callback context is documented;
- [ ] Temporary pointer lifetime is documented;
- [ ] Results that must be handled use `__must_check`;
- [ ] Variadic format APIs use the correct `__printf_like`;
- [ ] Deprecated APIs have both `@deprecated` and `__deprecated`;
- [ ] `__syscall` was not used as a generic attribute.

## Documentation organization

All maintained documentation is English. The documentation home links to
architecture/, api-design/, api/, development/, and compatibility/
references. API design is split into seven focused pages; the complete RUI3 migration
mapping is maintained in compatibility/ rather
than one combined page. Update doc/SUMMARY.md and section indexes when
adding or moving a chapter. Keep metadata and purpose near the top.

Use relative Markdown links between maintained pages. Use repository URLs
for code/configuration outside the documentation tree so links also work
in standalone HTML. Preserve explicit anchors when moving sections.
Diagrams use fenced text; no SVG assets or rendering plugins are required.

Build from the repository root:

```sh
tools/docs/generate-doxygen.sh
tools/docs/generate-mdbook.sh
```

Doxygen output includes a link-resolution pass for chapter fragments.
mdBook stages only the maintained sources, excluding previous Doxygen,
mdBook, and external-checkout output. Its combined print page is disabled.
Use --out-dir PATH with generate-mdbook.sh for a separate preview directory.
Both builders require Python 3; use the CI-pinned Doxygen version and a
compatible mdBook installation. The documentation CI job currently builds
Doxygen; run the mdBook generator locally when changing guide navigation.
