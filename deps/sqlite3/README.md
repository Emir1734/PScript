# SQLite amalgamation

When system `SQLite3` is not found by CMake, PScript builds the official SQLite amalgamation from `sqlite3.c` / `sqlite3.h` in this directory.

Download from [sqlite.org](https://www.sqlite.org/download.html) (amalgamation zip) and extract here, or install `libsqlite3-dev` / use vcpkg on your platform.
