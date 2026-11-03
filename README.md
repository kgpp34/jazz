# Jazz

A C asynchronous memory-transfer engine targeting RoCEv2.

Focus areas include bounded resource usage, explicit memory lifetimes, batched transfers, and fault-aware completion handling.

Experimental Linux C17 library with RDMA_CM and TCP out-of-band RC connection setup,
registered host memory, and synchronous RDMA Write/Read. The asynchronous batch engine
is planned. The API is not stable.

Build with CMake and rdma-core development packages, or use `Dockerfile`:

```sh
docker build -t jazz-dev .
docker run --rm -v "$PWD:/work" jazz-dev sh -c \
  'cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure'
```

`jazz_roundtrip` accepts `--mode cm|oob --role server|client --local IPv4 --port PORT`
and `--peer IPv4` on the client. Use an IP backed by an active RoCEv2 device.
Set `JAZZ_TEST_IP` to run the real RDMA integration suite; otherwise it reports SKIP.
Docker alone does not provide RDMA: its Linux host needs an RNIC or RXE-enabled kernel
and accessible RDMA devices. Soft-RoCE results are functional validation, not hardware benchmarks.

Single-owner connections, numeric IPv4, one signaled WR per transfer, bounded control
messages, and explicit deadlines are supported. Remotely accessible regions must be
quiesced before release. Automatic replay, GPU memory, and vLLM integration are future work.
