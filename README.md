# litebtree

A standalone key-value storage engine extracted from SQLite's storage
subsystem — the B-tree layer, the pager, and WAL — with the SQL layer
removed entirely.

## What it is

- **Origin**: derived from SQLite 3.54.0 (public domain). The B-tree, pager,
  and WAL code are reused with only interface-level shims; no algorithmic
  changes.
- **Model**: plain key-value semantics. Keys are byte strings ordered by
  binary comparison (memcmp). Values are arbitrary byte strings.
- **Multiple trees**: one database file can contain many B-trees, each
  identified by its root page number.
- **ACID transactions**: inherited from SQLite's rollback journal and WAL
  crash recovery.
- **Concurrency**: in-process multi-threading plus cross-process file
  locking, inherited from SQLite.
- **File format**: same page/cell layout as SQLite, with a different magic
  string so `sqlite3` CLI tools do not mistake it for a SQL database.

The design and extraction plan is documented in
[docs/kv-extraction-plan.md](docs/kv-extraction-plan.md).

## Status

Design and planning phase; implementation has not started yet.

## License

Derived from SQLite, which is public domain. This project is likewise
released to the public domain.

```
May you do good and not evil.
May you find forgiveness for yourself and forgive others.
May you share freely, never taking more than you give.
```