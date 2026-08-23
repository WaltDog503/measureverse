# ShakingMyHead (S.M.H.) - The Measureverse Service

The Measureverse is a high-performance backend measurement-to-fit sizing engine for ShakingMyHead LLC.

## Features
- Calibrated dimensional scaling with logarithmic efficiency.
- In-memory $O(\log n)$ SKU bracket lookup.
- Native multi-threaded REST API via Crow C++ framework.

## Build and Run (Fedora Workstation)
```bash
g++ -O3 -std=c++17 measureverse_server.cpp -lpthread -o measureverse_server
./measureverse_server
