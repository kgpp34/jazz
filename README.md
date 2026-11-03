# Jazz

An experimental Linux C17 memory-transfer library targeting RoCEv2.

The `jz_` API currently provides Engine lifecycle, per-device protection domains,
RDMA capability snapshots, and retryable cleanup. The new core does not transfer
data yet; asynchronous workers, memory registration, GPU support, and inference
engine integration are subsequent work.

Build with CMake and the libibverbs/librdmacm development packages:

```sh
cmake -S . -B build -DJZ_BUILD_PROTOTYPE=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Public headers are under `include/jz`; link against `libjz`.
Code comments are in English. The API is not stable.

The historical synchronous CM/OOB prototype is built separately by default as
`jz_prototype`, with the `jz_roundtrip` example. Set `JZ_TEST_IP` and optionally
`JZ_TEST_DEVICE` (default `rxe0`) to run device tests; otherwise they report SKIP.
Device tests require an RNIC or an RXE-enabled Linux kernel. Docker alone does
not provide RDMA, and Soft-RoCE tests do not establish hardware performance.
