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
