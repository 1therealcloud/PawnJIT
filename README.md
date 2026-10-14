# PawnJIT

PawnJIT is a just-in-time (JIT) compiler for the Pawn scripting language, designed for open.mp servers.

It compiles Pawn bytecode into native machine code at runtime, reducing interpreter overhead and significantly improving script execution performance without requiring changes to existing scripts.

PawnJIT is approximately 5x faster than the standard Pawn VM, achieving an average speedup of 4.67x in benchmarks. Actual performance gains depend on the workload.

## Performance

Benchmark results comparing the standard Pawn VM against PawnJIT on the same machine.

| Benchmark | Pawn VM | PawnJIT | Speedup |
|---|---:|---:|---:|
| Arithmetic | 363.28 µs | 62.26 µs | **5.84x** |
| Loops | 262.70 µs | 41.99 µs | **6.26x** |
| Function Calls | 375.00 µs | 86.43 µs | **4.34x** |
| Control Flow | 244.14 µs | 78.13 µs | **3.13x** |
| Raw Calls | 44.43 µs | 11.05 µs | **4.02x** |
| Parity Stress | 337.89 µs | 73.73 µs | **4.58x** |
| Differential Fuzz | 91.31 µs | 17.27 µs | **5.29x** |

## Build

```bash
git clone --recurse-submodules https://github.com/1therealcloud/PawnJIT.git
cd PawnJIT

# Windows
cmake -S . -B build -A Win32
cmake --build build --config Release

# Linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j$(nproc)
```

The DLL ends up in `build/Release/PawnJIT.dll|.so`.
Use `Debug` for development or `RelWithDebInfo` for an optimized build with debug information.

## License

The plugin code is licensed under [MPL 2.0](LICENSE).
Dependencies and borrowed SDK code keep their own licenses.
