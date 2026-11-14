// hakuX-Boosted bench: one thread-local pointer in a shared library, read
// and written the way the core's "who am I" variables are (a static
// thread_local in a -fPIC library, so every access goes through the TLS
// descriptor of the library). Built twice: once loaded at start-up (the
// loader then gives it a fixed offset and the descriptor call is a few
// instructions) and once loaded with dlopen with a thread-local block too
// large for the loader's spare static space (the descriptor then calls the
// loader's dynamic resolver, which is what bionic does for every dlopen'ed
// library and what the profile shows as tlsdesc_resolver_dynamic).
#include <stddef.h>

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define FN(name) CAT(PRIM_PREFIX, name)

static __thread void* current_thing;
#ifdef PRIM_TLS_PAD
static __thread char pad[PRIM_TLS_PAD];
__attribute__((noinline)) char* FN(_pad)(void) { return pad; }
#endif

__attribute__((noinline)) void* FN(_read)(void) { return current_thing; }
__attribute__((noinline)) void FN(_write)(void* value) {
  current_thing = value;
}
__attribute__((noinline)) void* FN(_address)(void) { return &current_thing; }
