# Third-party notices

Braidcast is GPLv2-or-later (see `COPYING`). This file lists third-party
source vendored directly into the tree (not fetched as a build dependency
and not a git submodule) whose license requires that a copy of its license
text travel with the source. See `frontend/deps/README.md` for the fuller
vendoring rationale, checksums, and versions of everything under
`frontend/deps/`.

| Dependency | Version | Upstream | License | Vendored at | License text |
| --- | --- | --- | --- | --- | --- |
| sqlite_orm | 1.9.1 | https://github.com/fnc12/sqlite_orm | AGPL-3.0 | `frontend/deps/sqlite_orm/sqlite_orm.h` | `frontend/deps/sqlite_orm/LICENSE` |

To add another entry: append one row above with the dependency's name,
vendored version, upstream URL, license identifier, the path to the
vendored copy, and the path to the license file placed alongside it.
