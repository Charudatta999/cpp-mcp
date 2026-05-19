# Benchmarking

The repository includes a benchmark smoke executable named `benchmark_smoke`. It is intentionally small and dependency-free; it is a correctness and trend smoke test, not a full performance lab.

## Planned Benchmarks

- JSON-RPC parse and serialize latency.
- Tool dispatch overhead for empty, small, and large argument payloads.
- Stdio transport message throughput.
- Gateway request construction overhead.
- Concurrent request scheduling once the async runtime exists.

## Smoke Validation

Build and run the current test suite:

```bash
cmake -S . -B build -DMCP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the benchmark smoke target:

```bash
cmake --build build --target benchmark_smoke
./build/benchmark_smoke
```

On multi-config generators, run the executable from the selected configuration directory, for example `build/Debug/benchmark_smoke.exe`.

Record compiler, build type, CPU, operating system, and command line for any published benchmark report. Treat numbers without environment metadata as non-reproducible.
