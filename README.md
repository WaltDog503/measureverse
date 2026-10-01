# ShakingMyHead (S.M.H.) - The Measureverse Service

Backend measurement-to-fit sizing engine for ShakingMyHead LLC: a standalone C++17 POSIX HTTP
server with SQLite persistence and a single-page atelier dashboard (`index.html`).

## Layout

```
src/sizing.hpp|.cpp        Calibration, O(log n) bracket matching, silhouette classification
src/json_util.hpp          Strict flat-JSON parser + output escaping
src/measureverse_server.cpp HTTP server, routing, SQLite layer (schema auto-created)
tests/test_measureverse.cpp Unit tests + 10k-bracket benchmark against production code
index.html                 Dashboard served at /
```

## Build

Dependencies: a C++17 compiler, CMake >= 3.16, SQLite3 dev headers.

```bash
sudo dnf install gcc-c++ cmake sqlite-devel       # Fedora
sudo apt install g++ cmake libsqlite3-dev         # Ubuntu/Debian

cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
cd build && ./measureverse_server                 # http://localhost:8080
```

Without CMake:

```bash
g++ -std=c++17 -Wall -Wextra -O2 -Isrc src/measureverse_server.cpp src/sizing.cpp -lsqlite3 -o measureverse_server
g++ -std=c++17 -Wall -Wextra -O2 -Isrc tests/test_measureverse.cpp src/sizing.cpp -o test_measureverse
```

VS Code: install the CMake Tools extension, open the folder, pick a kit, then Build / Run Tests from
the status bar. `compile_commands.json` is exported for IntelliSense.

Configuration: port via `./measureverse_server 9090` or `MEASUREVERSE_PORT`; database path via
`MEASUREVERSE_DB` (default `measureverse.db`).

## API

All POST bodies are flat JSON objects. Errors return `{"status":"error","message":"..."}` with
400/404/405/413/500.

| Method | Path | Body | Returns |
|---|---|---|---|
| POST | `/api/v1/measure` | `bust, waist, hip` (0-120], `scale_factor` [0.1, 10], default 1 | calibrated values, `matched_sku`, `silhouette` |
| POST | `/api/v1/vault/save` | `client_name, client_email, bust, waist, hip, silhouette, matched_sku` | `fit_token` |
| POST | `/api/v1/orders/reserve` | `fit_token` (optional, must exist), `garment_title, garment_price, bust, waist, hip, silhouette` | `order_token` |

## Sizing rules

Brackets are sorted, and each one runs from its `min` up to the next bracket's `min`. There are
no floating-point gaps, so 33.55 now resolves to `SMH-00` instead of `BESPOKE`. Values outside the
full range return `BESPOKE`.

## Schema

`measurement_logs`, `client_profiles` (`fit_token` UNIQUE) and `order_reservations` (`order_token`
UNIQUE, `fit_token` references `client_profiles`). Created on startup with `CREATE TABLE IF NOT
EXISTS`. Databases from older builds that used the `raw_reading`/`scale_factor` columns in
`measurement_logs` need that table dropped or migrated.
