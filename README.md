# Jazz

An experimental Linux C17 memory-transfer library targeting RoCEv2.

The `jz_` API currently provides Engine lifecycle, per-device protection domains,
RDMA capability snapshots, HOST memory registration, and retryable cleanup.
Registration supports concurrent batches, common MR chunks across prepared
devices, bounded helper threads, optional HOST pre-touch, and NUMA location hints.
Check per-buffer results even when registration fails: cleanup-pending buffers
must stay allocated until unregister or Engine destruction succeeds.

The core does not transfer data yet; asynchronous workers, GPU support, and
inference engine integration are subsequent work.

Build with CMake and the libibverbs/librdmacm development packages:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Public headers are under `include/jz`; link against `libjz`.
Code comments are in English. The API is not stable.

For VS Code on macOS, open this repository folder and run **Dev Containers:
Reopen in Container**. The included configuration builds Linux dependencies and
supplies IntelliSense with CMake's compilation database. Use **Build Jazz** or
**Test Jazz** from Run Task.

Set `JZ_TEST_IP` and optionally `JZ_TEST_DEVICE` (default `rxe0`) to run device
tests; otherwise they report SKIP. Device tests require an RNIC or an RXE-enabled
Linux kernel. Docker alone does not provide RDMA, and Soft-RoCE tests do not
establish hardware performance.

`jz_reg_bench DEVICE cm|oob BYTES ITERATIONS serial|auto|parallel [warm]`
reports registration/unregistration pair latency and process CPU time on
already-touched HOST memory. `cm|oob` selects context ownership; no peer
connection or payload transfer is timed.
