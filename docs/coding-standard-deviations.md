# Coding Standard Deviations

Documented deviations from the framework's C++ coding standard. Each entry
records the rule, the deviation, and the rationale.

Every entry below was measured against the tree, not assumed.

| ID | Rule / guideline | Deviation | Rationale |
|----|------------------|-----------|-----------|
| S-1 | Banned feature: exceptions (`throw`/`try`/`catch`), `-fno-exceptions` | The service catches library exceptions at every verb, and raises one of its own (`UpdaterError`) from the adapter | The library's error model is exception-based and is **installed public interface** — see the library's D-1. A service that consumes it must catch them, and `-fno-exceptions` is set by no component in the framework. `UpdaterError` does not extend the reach of the deviation: it exists so that the failure crossing the `IUpdater` boundary is the *interface's* vocabulary rather than the library's, which is what lets a test double express a failure without linking the library. It can be retired only together with the library's exception hierarchy. |
| S-2 | Banned feature: `std::function` | `IUpdater::setProgressCallback` takes one: three sites in `src/`, two more in `tests/` | A per-operation progress callback on an I/O-bound path, not an inner-loop call. The type erasure buys the seam that lets the service be driven from an in-memory double, which is the reason `IUpdater` is pure-abstract at all. A function pointer cannot carry the captured state the call sites need. Same shape as the library's D-2. |
| S-3 | Templates: restricted, and non-type-dependent logic factored out | One function template, `translated()` in the adapter, instantiated once per verb | It is a higher-order try/catch, not one of the categories the standard lists as allowed — it owns no resource, so calling it an RAII wrapper would be wrong. Recorded as a deviation rather than argued into a permitted class. The factoring rule *is* honoured: the classification lives in a plain function (`rethrow_as_updater_error`), so an instantiation carries only the try/catch shell. |
| S-4 | Naming: constants and `constexpr` are `snake_case`, not `ALL_CAPS` | `OBJ_PATH`, `INTERFACE`, `FSUPDATE_INTERFACE_VERSION`, `ERROR_BUSY`, `ERROR_NO_UPDATER` in `src/service.cpp` | Consistent across every file-scope constant in this component, and the standard's own naming section states that consistency with the project convention outranks a default (Core Guidelines NL.8). Renaming a subset would make the component less consistent, not more; renaming all of them is a mechanical change to qualified code, which the standard itself warns can trigger a re-review. Same shape as the library's D-5. |

## Rules this component does meet

Recorded because a register that lists only failures invites the assumption
that the rest was never checked. Measured on the current tree:

- **No `dynamic_cast`, no `typeid`.** The only textual matches are a comment
  explaining why the failure classifier does not need them — it dispatches by
  re-raising inside a catch block.
- **No `std::regex`, no `<iostream>`, no `std::list`, no `printf`/`<cstdio>`.**
  No matches of any kind.
- **No `fork`/`exec`/`system` call.** One textual match, in a comment
  mentioning a possible future `fork()`.
- **No `std::filesystem`.** The single textual match is a comment recording
  that the existence check uses POSIX instead, as the standard requires.
- **No raw `new`, and no `operator delete`.** `delete` does appear four times,
  as `= delete` on the copy and move operations — the opposite of the banned
  use.
- **Members carry the `m_` prefix**, which is the convention the standard
  assigns to this component by name, rather than the unmarked form its own
  table gives as the default. Public fields of the plain data carriers
  (`CheckUpdateInfo`, `PendingEvent`, `UpdaterError`) are unmarked, consistently
  with each other. `tests/` uses trailing underscores, which this measurement
  does not cover.
- **The published bus surface is version-stamped and partly gated.** A contract
  check compares the documented interface against the exported vtable —
  methods, and signals with their signatures — and refuses to run if any
  registration is one it cannot parse. **The error names are not part of that
  gate**: the contract lists them, and nothing checks the list against the
  code. That gap is named here rather than left for the next reader to
  discover.
