# ARM pthread fixture

`pthread-smoke-armhf` is the stripped output of `guest-tests/pthread-smoke.c`.
It was built inside Linuxemu against the pinned Alpine 3.24 rootfs using GCC
15.2.0 and musl 1.2.6, then stripped with Alpine binutils 2.45.1.

SHA-256:

```text
f8c1d23a5154beb69602abaa8098377a2e8217597f5352f23ff8894668e8558c
```

Install Alpine `build-base` temporarily in the test rootfs and run
`sh scripts/build-pthread-fixture.sh` to rebuild it from source.
