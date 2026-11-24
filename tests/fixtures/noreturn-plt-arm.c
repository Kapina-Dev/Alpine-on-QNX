/* Host fixture for code_map_noreturn (tests/code-map-test.c): calls two
   functions that never return and one that does, through the PLT.
   Built on the laptop:
     arm-linux-gnueabihf-gcc -O2 -mthumb -shared -fPIC -nostdlib \
       -o tests/fixtures/noreturn-plt-arm.so tests/fixtures/noreturn-plt-arm.c
   (-nostdlib: the three are left undefined, so each gets a PLT entry.) */
extern void abort(void) __attribute__((noreturn));
extern void __stack_chk_fail(void) __attribute__((noreturn));
extern int puts(const char *);

int fixture_entry(int x)
{
    if (x == 1) abort();
    if (x == 2) __stack_chk_fail();
    return puts("returns");
}
