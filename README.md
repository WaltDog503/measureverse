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

| Table | Key columns | Constraints |
|---|---|---|
| `measurement_logs` | `bust, waist, hip, matched_sku, silhouette, created_at` | none |
| `client_profiles` | `fit_token, client_name, client_email, bust, waist, hip, silhouette, matched_sku` | `fit_token` UNIQUE |
| `order_reservations` | `order_token, fit_token, garment_title, garment_price, bust, waist, hip, silhouette` | `order_token` UNIQUE, `fit_token` references `client_profiles` |

Tables are created on startup with `CREATE TABLE IF NOT EXISTS`, so a new database needs no setup.

## Migrating Existing Databases

Databases created before PR #1 store measurements in the old `measurement_logs` layout:
`raw_reading` (bust), `scale_factor` (waist) and `computed_dimension` (hip), with the silhouette
packed into `matched_sku` as `"<sku> [<silhouette>]"`. The new server will fail to log
measurements against that table, so pick one option below.

### Check Whether You Need It

```bash
sqlite3 measureverse.db "PRAGMA table_info(measurement_logs);"
```

If the output lists `raw_reading`, migrate. If it lists `bust`, you are already current.

### Option A: Start Fresh

Use this if the old data doesn't matter (for example, dev or test data).

```bash
mv measureverse.db measureverse.db.bak   # keep a copy just in case
./measureverse_server                    # schema is recreated automatically
```

### Option B: Migrate in Place (Keeps History)

1. Stop the server (Ctrl+C).
2. Back up the database:
   ```bash
   cp measureverse.db measureverse.db.bak
   ```
3. Run the migration. It runs as one transaction, so if anything fails nothing changes:
   ```bash
   sqlite3 measureverse.db < scripts/migrate_measurement_logs.sql
   ```
4. Verify:
   ```bash
   sqlite3 -header measureverse.db "SELECT * FROM measurement_logs LIMIT 5;"
   sqlite3 measureverse.db "PRAGMA integrity_check;"   # should print: ok
   ```
5. Start the server again.

The script splits `"<sku> [<silhouette>]"` back into `matched_sku` and `silhouette`. Rows without
the bracketed suffix keep their full SKU text and get `silhouette = 'Unknown'`. Old rows get a
`created_at` of the migration time, because the old schema had no timestamp column.

### Other Tables

`client_profiles` and `order_reservations` keep their column names, so existing tables keep
working without changes. They only miss the new UNIQUE and foreign-key constraints, because
`CREATE TABLE IF NOT EXISTS` doesn't alter existing tables. To add those constraints, back up the
database, then export, drop and re-import those two tables, or use Option A.

### Rollback

```bash
mv measureverse.db.bak measureverse.db
```

Then check out the commit before PR #1 (`git checkout 2a13fce`).

If you don't have the `sqlite3` CLI, install it with `sudo dnf install sqlite` (Fedora) or
`sudo apt install sqlite3` (Ubuntu/Debian).
