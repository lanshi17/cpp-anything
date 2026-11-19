# legacy.cpp

C-style C++: raw loops, `char*`, `malloc`/`free`, `printf`, C casts.

```sh
cpp-anything analyze . --lang cpp
cpp-anything bridge . legacy.cpp --lang cpp --out cpp/legacy-cpp
cpp-anything record cpp/legacy-cpp
# rewrite src/port.cpp as modern C++20/23, then:
cpp-anything diff cpp/legacy-cpp
cpp-anything gate cpp/legacy-cpp
```
