# tests/mac: Mac-only test programs (C++11)

Each `*.cpp` here is a standalone program with its own `main()`, built by `make -C tests mac` with
`clang++ -std=c++11` against all core sources and `tests/fs_posix.cpp`, then run; a nonzero exit code fails the
target. These programs may use the C++ standard library (for example `std::regex` as a reference in `rxfuzz.cpp`,
T03). They are never built by VC6 and `tools/lint.py` skips this folder.
