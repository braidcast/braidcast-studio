cmake_minimum_required(VERSION 3.28...3.30)

# The vendored SQLite amalgamation gets its own target so every consumer compiles
# it with identical semantics. set_source_files_properties is directory-scoped, so
# per-file definitions set beside the frontend executable would silently not reach
# a test target declared elsewhere -- and the divergence would be invisible rather
# than fatal, which is worse: SQLITE_DEFAULT_FOREIGN_KEYS decides whether
# ON DELETE CASCADE fires at all, so a suite compiled without it would pass while
# testing different behaviour from the product.
add_library(braidcast-sqlite OBJECT)
add_library(OBS::sqlite ALIAS braidcast-sqlite)

target_sources(braidcast-sqlite PRIVATE sqlite/sqlite3.c PUBLIC sqlite/sqlite3.h)

target_include_directories(braidcast-sqlite SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sqlite")

# SQLITE_OMIT_DEPRECATED is the one definition consumers must also see: it gates
# declarations in sqlite3.h, not merely their definitions, so a consumer compiled
# without it would accept a call to a deprecated entry point that was never built
# and fail only at link. The rest steer the amalgamation's own compilation and are
# baked into the object every consumer links, which is what makes them uniform.
#
# SQLITE_DEFAULT_FOREIGN_KEYS=1 because foreign keys are off by default in SQLite
# and the schema leans on ON DELETE CASCADE. SQLITE_DQS=0 makes a mistyped
# identifier an error instead of a silently-accepted string literal.
# SQLITE_THREADSAFE=1 pairs with the SQLITE_OPEN_FULLMUTEX the connection is opened
# with: access is meant to stay on the CEF UI thread, and serialised mode is cheap
# insurance at this call rate should a later caller not honour that.
target_compile_definitions(
  braidcast-sqlite
  PUBLIC SQLITE_OMIT_DEPRECATED
  PRIVATE
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
)

set_target_properties(braidcast-sqlite PROPERTIES FOLDER deps)
