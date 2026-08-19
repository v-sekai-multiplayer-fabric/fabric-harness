# contract-bus

The weft plane harness: a command in, reply bytes out, over iceoryx2 shared memory with no
daemon and no copy. It is the contract a transport layer and an interactor compose against,
so `transport-runpod` and `transport-bus-cli` reach an interactor through the same envelope
and the same two services.

Every plane and every edge links `weft::harness`, and none of them links iceoryx2. The C ABI
is named in `iceoryx2.sigs` and turned into a dlsym dispatch table, so a plane builds on a
machine that has never seen the library, and a missing bus is a start-up failure with a
message rather than a link error.

## State

| payload                          | proof                                   | run on                |
| -------------------------------- | --------------------------------------- | --------------------- |
| `Snapshot`, fixed 40-byte struct | `proof/publisher.cpp`, `subscriber.cpp` | Windows, macOS, Linux |
| byte slice, `run_command_loop`   | `proof/command_*.cpp`                   | Windows, macOS, Linux |

CI runs both proofs on Linux, macOS and Windows, and once more with `WEFT_ICEORYX2_PATH` unset so the names in `bus.hpp` are exercised rather than skipped — every earlier run set the override, which is how a Linux soname survived in the macOS list.

## Build and run

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
export WEFT_ICEORYX2_PATH=/path/to/libiceoryx2_ffi_c.{so,dylib,dll}
./build/weft-harness-command_subscriber 8 &
./build/weft-harness-command_publisher 8
```

iceoryx2 is built separately with `cargo build --release -p iceoryx2-ffi-c`; nothing here
vendors it. `WEFT_ICEORYX2_PATH` is optional where the library sits on the loader's search
path, and running without it is the only thing that exercises the name list at all.

## More

- [docs/runs.md](docs/runs.md) — every proof run, the machine, and the output
- [docs/design.md](docs/design.md) — why nothing links iceoryx2, and why not iceoryx v1
- [docs/plan.md](docs/plan.md) — one harness rather than one per plane, and what is missing
