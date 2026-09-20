# tradebot: notes for AI-assisted development

Read `docs/ARCHITECTURE.md` first. It defines the 23-phase plan and the
component boundaries. Work proceeds phase by phase; do not skip ahead.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Before committing, the code must build warning-free under both GCC and Clang
(Debug with sanitizers, Release) and all tests must pass. CI runs exactly
this matrix.

## Conventions

- C++23, namespace `tradebot`, headers and sources together under
  `src/tradebot/<module>/`. Include as `"tradebot/<module>/<file>.hpp"`.
- One doctest file per source file under `tests/<module>/`, registered in
  that directory's `CMakeLists.txt`.
- Money is never `double`. Use `Price`, `Quantity`, `Notional` from
  `core/fixed_point.hpp`. Cross-type products name their rounding mode.
- Time is `Timestamp` (ns since epoch, UTC) and `Duration` from
  `core/time.hpp`. Read "now" only through a `Clock&`; never call
  `std::chrono::system_clock::now()` outside `WallClock`.
- Fallible operations return `Result<T>` (`tl::expected<T, Error>`); throw
  only for programming errors (overflow, logic errors).
- Everything must be deterministic given the same inputs and seed.
- Strategies depend only on the strategy framework interfaces, never on a
  concrete exchange, feed, or clock.
- No third-party dependency without a strong reason; header-only libraries
  are vendored under `third_party/` with their license.
- Never commit market data or run artifacts (`data/`, `runs/` are ignored).

## The board

`board/` is the shared task list and message board for the humans and
agents on this project. If you are asked to "check the board", "check
your tasks", or given a task id like `T-007`, follow the protocol in
`board/README.md` before doing anything else. Board edits are committed
separately from code changes and are never destructive.
