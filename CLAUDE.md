# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is PostgreSQL 18.3, an advanced object-relational database management system written in C. The codebase implements a full SQL database engine including query parsing, optimization, execution, transaction management, and storage.

## Build System

PostgreSQL supports two build systems:

### Autoconf/Make (Traditional)
```bash
# Configure build (first time)
./configure --prefix=/usr/local/pgsql --enable-debug --enable-cassert

# Build entire system
make

# Build specific subsystem (faster for development)
make -C src/backend
make -C src/bin/psql

# Install
make install

# Build with all contrib modules
make world
make install-world
```

### Meson (Modern, Faster)
```bash
# Setup build directory
meson setup build --prefix=/usr/local/pgsql -Dcassert=true

# Build
meson compile -C build

# Install
meson install -C build

# Reconfigure options
meson configure build -Dcassert=false
```

**Note:** Cannot mix build systems. Source tree must be clean. Run `make distclean` before switching from autoconf to meson.

## Testing

### Regression Tests
```bash
# Run main regression test suite
make check

# Run specific test
make check TESTS="select join"

# Run all tests (including contrib)
make check-world

# Run tests against installed instance
make installcheck

# Parallel testing
make installcheck-parallel
```

### Individual Test Suites
```bash
# Parser/analyzer tests
make -C src/test/regress check

# Isolation tests (concurrency)
make -C src/test/isolation check

# Recovery/replication tests
make -C src/test/recovery check

# TAP tests (Perl-based)
make -C src/test/perl check

# Specific contrib module
make -C contrib/pgcrypto check
```

### Running Single Test
```bash
cd src/test/regress
./pg_regress --temp-instance=/tmp/test --temp-config=regress.conf select
```

## Architecture Overview

PostgreSQL uses a **multi-process architecture** (not multi-threaded). Each client connection gets a dedicated backend process.

### Query Processing Pipeline

```
SQL Query → Parser → Analyzer → Rewriter → Planner → Executor → Storage
```

1. **Parser** (`src/backend/parser/`): Lexical/syntax analysis, builds raw parse tree
   - `scan.l`: Tokenizer (Flex)
   - `gram.y`: Grammar (Bison)
   - `analyze.c`: Semantic analysis, transforms raw parse tree to Query structure

2. **Rewriter** (`src/backend/rewrite/`): Applies rules system (views, rules)

3. **Optimizer** (`src/backend/optimizer/`): Generates optimal execution plan
   - `/path`: Generates possible access paths (seq scan, index scan, joins)
   - `/plan`: Converts cheapest path to executable plan
   - `/prep`: Preprocessing, subquery handling
   - `/geqo`: Genetic query optimizer for complex joins
   - **Key concept**: Uses cost-based optimization to select among multiple plans

4. **Executor** (`src/backend/executor/`): Executes the plan tree
   - **Pull-based pipeline**: Each node produces tuples on demand
   - Plan tree is read-only; mutable state stored in parallel PlanState tree
   - Handles SELECT, INSERT, UPDATE, DELETE, MERGE via ModifyTable node

### Core Subsystems

**Nodes** (`src/backend/nodes/`)
- All parse trees, plan trees, executor state trees use typed node structures
- First field is always `NodeTag` for type identification
- Auto-generated support functions: copy, equal, serialize (out/read)
- See nodes/README for adding new node types

**Storage Layer** (`src/backend/storage/`)
- `/buffer`: Shared buffer pool management (LRU caching of disk pages)
- `/smgr`: Storage manager interface (abstraction over file I/O)
- `/page`: Page-level operations
- `/lmgr`: Lock manager (table/row locks, deadlock detection)
- `/freespace`: Free space map for finding pages with available space
- **Buffer pinning**: Must pin buffer before access, prevents page eviction

**Access Methods** (`src/backend/access/`)
- `/heap`: Heap table storage (main row storage format)
- `/nbtree`: B-tree index implementation
- `/hash`: Hash index
- `/gin`: Generalized Inverted Index (for arrays, full-text)
- `/gist`: Generalized Search Tree (for geometric data)
- `/brin`: Block Range Index (for large sorted tables)
- `/spgist`: Space-partitioned GiST
- `/transam`: Transaction manager (MVCC, WAL, commit log)

**Postmaster** (`src/backend/postmaster/`)
- Main server process that spawns backend processes
- `postmaster.c`: Connection handling, process lifecycle
- `autovacuum.c`: Auto-vacuum launcher
- `checkpointer.c`: Checkpoint process
- `walwriter.c`: WAL writer process

**Catalog** (`src/backend/catalog/`)
- System catalog management (pg_class, pg_attribute, etc.)
- Schema operations (CREATE TABLE, ALTER, DROP)

**Commands** (`src/backend/commands/`)
- DDL command execution (CREATE, DROP, ALTER for various objects)
- Utility commands (VACUUM, ANALYZE, EXPLAIN)

**Utils** (`src/backend/utils/`)
- `/adt`: Abstract data type implementations (int4, text, date, etc.)
- `/cache`: System catalog cache
- `/mmgr`: Memory management (memory contexts)
- `/sort`: External sort/merge
- `/fmgr`: Function manager

### Client Interface

**libpq** (`src/interfaces/libpq/`)
- C client library for connecting to PostgreSQL
- Protocol implementation (message passing)

**Binaries** (`src/bin/`)
- `psql/`: Interactive terminal
- `pg_dump/`: Backup utility
- `pg_ctl/`: Server control utility
- `initdb/`: Database cluster initialization
- `pg_basebackup/`: Streaming backup
- `pgbench/`: Benchmarking tool

### Header Files

**src/include/** mirrors backend structure:
- `nodes/`: Node type definitions (parsenodes.h, plannodes.h, execnodes.h)
- `access/`: Access method interfaces
- `storage/`: Storage subsystem interfaces
- `catalog/`: System catalog definitions
- `postgres.h`: Core PostgreSQL types and macros
- `c.h`: Portability macros, basic types

## Code Patterns

### Memory Management
- PostgreSQL uses **memory contexts** (not malloc/free directly)
- Each query execution uses temporary contexts that are bulk-freed at end
- Prevents memory leaks even if errors occur mid-query

### Transaction System
- Three-layer architecture:
  - Low level: StartTransaction/CommitTransaction/AbortTransaction
  - Mid level: StartTransactionCommand/CommitTransactionCommand
  - Top level: BeginTransactionBlock/EndTransactionBlock (user SQL)
- MVCC for concurrency: multiple versions of rows coexist
- WAL (Write-Ahead Logging) for crash recovery

### Error Handling
- Uses setjmp/longjmp for error recovery (ereport/elog macros)
- Errors automatically clean up resources via memory contexts and callbacks
- Never return error codes - always use ereport() to throw errors

### Catalog Access
- System catalogs are regular tables with indexes
- Syscache provides fast lookup by common keys
- Must use appropriate locks when modifying catalogs

## Development Workflow

### Making Changes

1. **Parser/Analyzer**: When adding SQL syntax
   - Modify `gram.y` (add grammar rules)
   - Modify `keywords.c` if adding keywords
   - Update `analyze.c` to build Query structures
   - Add node types in `parsenodes.h` if needed

2. **Planner**: When adding optimization rules
   - Path generation in `optimizer/path/`
   - Cost estimation in `optimizer/path/costsize.c`
   - Plan creation in `optimizer/plan/`

3. **Executor**: When adding execution logic
   - Implement node execution in `executor/nodeXXX.c`
   - Update `executor/execProcnode.c` dispatch tables

4. **Storage**: When modifying data layout
   - Page format in `storage/page/`
   - Buffer management in `storage/buffer/`
   - Must maintain WAL logging

### Testing Changes

Always add regression tests for new features:
```bash
# Add SQL test file
echo "SELECT new_feature();" > src/test/regress/sql/new_feature.sql

# Generate expected output
make check  # Creates src/test/regress/expected/new_feature.out

# Update parallel schedule
# Edit src/test/regress/parallel_schedule to include new test
```

### Debugging

```bash
# Build with debug symbols and assertions
./configure --enable-debug --enable-cassert
make

# GDB attach to backend
ps aux | grep postgres
gdb -p <backend_pid>

# Useful debug functions (call from gdb)
(gdb) call pprint(node)     # Pretty-print parse/plan tree
(gdb) call print_expr(expr) # Print expression
```

## Key Constraints

- **No threading**: Each backend is single-threaded. Shared state uses IPC.
- **WAL-first**: All data modifications must be logged to WAL before page changes
- **Buffer discipline**: Must pin buffers, acquire appropriate locks
- **No longjmp across functions**: Error handling relies on cleanup callbacks
- **Catalog consistency**: System catalog changes must be transactional

## Additional Resources

Each major subsystem has detailed README files:
- `src/backend/optimizer/README` - Query optimization details
- `src/backend/executor/README` - Executor architecture
- `src/backend/storage/buffer/README` - Buffer management rules
- `src/backend/access/transam/README` - Transaction system
- `src/backend/nodes/README` - Node type system
- `src/backend/parser/README` - Parser structure

Official documentation: https://www.postgresql.org/docs/18/
