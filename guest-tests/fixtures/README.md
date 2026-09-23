# ARM pthread fixture

`pthread-smoke-armhf` is the stripped output of `guest-tests/pthread-smoke.c`.
It was built inside Linuxemu against the pinned Alpine 3.24 rootfs using GCC
15.2.0 and musl 1.2.6, then stripped with Alpine binutils 2.45.1.

SHA-256:

```text
5771a8ac2fd3b642468b69a20a848df0662620d8a6a54da4a77d2edd7a4157c8
```

Install Alpine `build-base` temporarily in the test rootfs and run
`sh scripts/build-pthread-fixture.sh` to rebuild it from source.
The fixture also cancels and joins a thread blocked in a musl condition wait.

# Phase 9 loader/process fixtures

These stripped ARM binaries were built from `dlopen-module.c`,
`dlopen-stress.c`, and `spawn-thread-stress.c` inside Linuxemu with the same
Alpine GCC and musl versions. Rebuild them with
`sh scripts/build-phase9-fixtures.sh` while `build-base` is installed.

SHA-256:

```text
4012ee50629a46c19c5ff69e976163e88b678773b8c48b700ee9737f88e63c35  phase9-module-armhf.so
ba17c3a023aa8d8fe398a2b116b1bed2ead5583b85f32f95f6c5673e0f93227a  dlopen-stress-armhf
1ac31c7cd968969bf92911c339b913a06629bffa09e9a0570f8efa08512f07f3  spawn-thread-stress-armhf
```
