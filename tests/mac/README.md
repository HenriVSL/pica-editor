# tests/mac: Mac-only test programs (C++11)

Each `*.cpp` here is a standalone program with its own `main()`, built by `make -C tests mac` with
`clang++ -std=c++11` against all core sources and `tests/fs_posix.cpp`, then run; a nonzero exit code fails the
target. These programs may use the C++ standard library (for example `std::regex` as a reference in `rxfuzz.cpp`,
T03). They are never built by VC6 and `tools/lint.py` skips this folder.

`pfuzz.cpp` (T16): mutation fuzzer for the core parsers (INI + config + session, `.lng`, hex patterns and go-to
expressions, regex compile + search + expand, backup headers, detection + Document lines). `make -C tests mac` runs
it for 3 s without sanitizers. For a real pass build it with ASan/UBSan and run it for minutes:

    cd tests && clang++ -std=c++11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DE_DEBUG \
      -I../src/core -I. -o build/pfuzz mac/pfuzz.cpp ../src/core/*.cpp fs_posix.cpp
    build/pfuzz 1200 <seed> tmp        # seconds, seed, folder for crashing inputs
    build/pfuzz -replay <target> <file>

A crashing input is written to `<folder>/pfuzz-<target>-<seed>.bin`; regressions go to
`tests/data/fuzz/<target>-<n>.bin` (targets ini lng hex rx bak doc), which every run replays first.
