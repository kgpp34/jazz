# Jazz

A C asynchronous memory-transfer engine targeting RoCEv2.

Focus areas include bounded resource usage, explicit memory lifetimes, batched transfers, and fault-aware completion handling.

The new Linux C17 core (`jazz/engine.h`, `libjazz`) currently provides Engine lifecycle,
explicit RDMA device preparation, per-device protection domains, capability snapshots,
and retryable cleanup. It does not transfer data yet. The API is experimental.

The historical synchronous CM/OOB Write/Read prototype is built separately as
`jazz_prototype`; disable it with `-DJAZZ_BUILD_PROTOTYPE=OFF`.

Build with CMake and rdma-core development packages, or use `Dockerfile`:

```sh
docker build -t jazz-dev .
docker run --rm -v "$PWD:/work" jazz-dev sh -c \
  'cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure'
```

The prototype's `jazz_roundtrip` accepts `--mode cm|oob --role server|client --local IPv4 --port PORT`
and `--peer IPv4` on the client. Use an IP backed by an active RoCEv2 device.
Set `JAZZ_TEST_IP` and optionally `JAZZ_TEST_DEVICE` (default `rxe0`) for device tests;
otherwise they report SKIP.
Docker alone does not provide RDMA: its Linux host needs an RNIC or RXE-enabled kernel
and accessible RDMA devices. Soft-RoCE results are functional validation, not hardware benchmarks.

Engine lifecycle calls must be serialized. Background workers, registered regions,
and transfers in the new core are subsequent work; GPU and vLLM support are not implemented.
