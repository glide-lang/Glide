# Glide language reference

Targeted at someone who already knows another systems language (Rust, C, Go, Zig). For a learning-oriented walkthrough see `TUTORIAL.md`.

## lexical structure

### keywords

```
let const mut fn struct enum impl trait dyn type extern pub move naked
if else while for return break continue match defer spawn spawn_thread
import use as in
true false null self Self
macro asm volatile c_raw
```

`chan<T>` is a type constructor (parses as a generic type). `Arena`,
`Vector`, `HashMap` are regular types from the prelude / stdlib, not
keywords.

### operators

```
+  -  *  /  %               arithmetic
== != <  <= >  >=           comparison
&& || !                     logical
&  |  ^  ~  << >>           bitwise
=  += -= *= /=              assignment / compound
&  &mut                     borrow / mutable borrow
*                           dereference / pointer-type prefix / auto-drop suffix
->                          fn return type
=>                          match arm
::                          path
?                           result propagation (postfix)
```

### literals

```
42      0xFF    0b1010    1_000_000        integers
3.14    2.5e10                              floats
"text"  "esc\n"                             strings
'a'     '\n'    '\xFF'                      chars
true    false   null                        keywords
```

### comments

```glide
// line
/* block */
```

The lexer currently discards comments; the formatter doesn't preserve them.

## types

| Type            | Meaning                                                   |
| --------------- | --------------------------------------------------------- |
| `int` `uint`    | platform `int` (typically 32-bit) and unsigned variant     |
| `i8`–`i64`, `u8`–`u64` | fixed-width integers                                |
| `usize` `isize` | pointer-sized integers                                    |
| `f32` `f64` `float` | floating point                                       |
| `bool` `char` `string` | primitives. `string` is `const char*`             |
| `*T`            | pointer; non-owning unless declared with auto-drop         |
| `&T`            | shared borrow (non-null, no lifetime annotation)           |
| `&mut T`        | exclusive mutable borrow (non-null)                        |
| `[]T`           | slice: `{ data: *T, len: usize }`                         |
| `T<U, V>`       | generic instantiation                                      |
| `fn(A, B) -> R` | function pointer                                           |
| `!T`            | result: success carries `T`, error carries a `string`      |
| `chan<T>`       | typed channel (bounded MPMC)                               |
| `*dyn Trait`    | fat pointer (vtable + data); runtime dispatch              |

## statements

```glide
let name[: T] = expr;             // immutable
let mut name[: T] = expr;         // mutable
let name*[: T] = expr;            // owned + auto-drop at scope exit

const NAME[: T] = expr;           // file-scope or block-scope constant

return [expr];

expr;                             // expression statement (calls, assignments)

if cond { ... } else { ... }      // statement form: blocks of stmts
while cond { ... }
for init; cond; step { ... }
{ ... }                           // block
match scrutinee { Variant(b) => { ... } _ => { ... } }

defer expr;                       // run at fn end / before return (LIFO)
spawn fn_call(args);              // run fn on the M:N coroutine scheduler
spawn_thread fn_call(args);       // run fn on a real OS thread (escape hatch)

import a::b::c;                   // module path; resolves to a/b/c.glide
import a::b::c::*;                // wildcard
import a::b::c::{X, Y};           // selective
```

`break` and `continue` work in `while` and `for` loops.

## declarations

```glide
[pub] fn name[<T1, T2>](p1: T1, p2: T2) -> R { ... }

[pub] struct Name[<T>] {
    field1: Type1,
    field2: Type2,
}

[pub] enum Name {
    Variant1,
    Variant2(int, string),
}

[pub] trait Name {
    fn required(self: Self) -> int;
    fn provided(self: Self) -> int { return 0; }   // default method
}

impl[<T>] Name[<T>] {
    fn method(self: *Name<T>, arg: int) -> int { ... }
}

impl Trait for Type {
    fn required(self: Self) -> int { ... }
}

extern fn libc_function(args) -> ret;          // declare a C function
```

`pub` makes the symbol importable from other Glide files. Top-level visibility defaults to private.

## memory model

### stack values

Plain primitives and `let p: T = T { ... }` (no `*`) live on the stack and are copied on assignment / return-by-value.

### owned heap (`*T` with auto-drop)

Two equivalent patterns:

```glide
let p: *Point = Point { x: 1, y: 2 };   // implicit: type annotation + struct lit
let p* = Point { x: 1, y: 2 };          // explicit marker
let v* = Vector::new();                  // works for any pointer-returning expr
```

The compiler emits `free(p as *void)` (or `p.free()` if the type defines a `free` method) at the end of the enclosing block. Cleanup is scope-based: an auto-drop binding inside a `for` body fires once per iteration.

### raw heap (`*T`)

Pointers that aren't auto-drop are raw:

```glide
let p: *Point = some_call();    // up to you to free
let q: *Point = new Point { x: 1, y: 2 };   // explicit `new`, raw
```

You're responsible for matching `free(p as *void)` or letting them leak intentionally (e.g. the program owns them until exit).

### arenas

```glide
let arena: *Arena = Arena::new(4096);
defer arena.free();

let p: *Point = arena.create(Point);            // sugar for arena.alloc(sizeof(Point)) as *Point
let buf: *int = arena.create(int, 100);         // sugar for an array
let raw: *void = arena.alloc(bytes);            // raw escape hatch
```

Use arenas when you have a bag of allocations sharing a lifetime (parser nodes, request-scoped data). Free is `O(1)` regardless of count.

### borrows

`&T` and `&mut T` are non-null views with function-scoped lifetimes. The borrow checker enforces:

| Code                    | Description                                                    |
| ----------------------- | -------------------------------------------------------------- |
| `borrow-in-field`       | `&T` / `&mut T` can't appear in a struct field (use `*T`)      |
| `borrow-alias-in-call`  | `f(&x, &mut x)` and `f(&mut x, &mut x)` are rejected           |
| `dangling-return`       | `return &local` is rejected; pass-through of borrow params OK   |
| `owned-escape`          | `return owned_p` is rejected (auto-drop would free it)         |
| `owned-move`            | `let q = owned_p` is rejected (double-free risk)               |
| `owned-into-ptr-param`  | `f(owned_p)` where `f` takes `*T` by value is rejected         |
| `overlap-borrow`        | conflicting `&` / `&mut` in the same scope is rejected          |

You never write lifetime annotations.

## if-as-expression

`if` works as both a statement (above) and an expression. As an
expression, both branches must be a single expression of the same
type, and `else` is required:

```glide
let label: string = if x > 3 { "big" } else { "small" };
let r: int = (if x > 0 { 10 } else { -10 }) * 2;

// else-if chains compose:
let category: string =
    if x < 0 { "negative" } else if x == 0 { "zero" } else { "positive" };
```

Codegen lowers it to a C ternary, so there's no extra cost vs writing
the `cond ? a : b` shape by hand. Branches with multiple statements
still need the statement form (`let mut x = default; if cond { x = ... }`).

## errors as values

```glide
fn parse(s: string) -> !int {
    if s.eq("") { return err("empty"); }
    return ok(42);
}

fn pipeline(s: string) -> !int {
    let n: int = parse(s)?;     // propagate err if any
    return ok(n + 1);
}
```

`?` only works inside a function whose return type is `!U` for some `U`. Result conversion happens at the propagation site: an `!T` with err `e` becomes `!U` with err `e`.

`unwrap(r)` returns the inner value or a zero-initialized fallback if `r` is err. Use it at boundaries where you've already checked the error.

## generics

Function and struct type parameters use angle brackets; instantiation is monomorphized.

```glide
struct Vec<T> { data: *T, len: int, cap: int }

impl<T> Vec<T> {
    fn new() -> *Vec<T> { ... }
    fn push(self: *Vec<T>, x: T) { ... }
}

fn first<T>(v: *Vec<T>) -> T { return v.data[0]; }

// Trait bounds — checked at every call site against the impl_set.
fn max<T: Ord>(a: T, b: T) -> T { if a > b { return a; } return b; }
fn dump<T: Display + Clone>(v: T) { println!(v.clone().to_string()); }
```

Inference fires from:

- An explicit return-type hint at the call site (`let v: *Vec<int> = Vec::new()`)
- Argument types passed in (`first(v)` infers `T` from `v`)
- Method calls on a not-yet-bound `let v = Generic::new()` — the first call that uses a type parameter resolves it

There's no turbofish syntax; if inference fails you must annotate the let.

## traits

```glide
trait Render {
    fn render(self: Self) -> string;

    // Default method — any impl that doesn't override gets this body.
    fn label(self: Self) -> string { return "rendered"; }
}

// Supertrait: every Render implementor must also impl Named.
trait Render: Named { ... }

impl Render for Box    { fn render(self: Self) -> string { return "Box"; } }
impl Render for Circle { fn render(self: Self) -> string { return "Circle"; } }

// Static dispatch via generic bound (monomorphized per type).
fn show_all<T: Render>(items: *Vector<T>) { ... }

// Dynamic dispatch via *dyn Trait (fat pointer = vtable + data).
fn show(r: *dyn Render) { println!(r.render()); }

let shapes: *Vector<*dyn Render> = Vector::new();
shapes.push(box_p as *dyn Render);
shapes.push(circle_p as *dyn Render);
```

`Self` inside a trait body refers to the implementor's type. The
default-method walk substitutes `Self` for the concrete type when
copying a default body into an impl that doesn't override it.

## macros

User-defined macro_rules! for AST-level expansion.

```glide
macro bail!($cond:expr, $msg:expr) {
    if $cond { return err($msg); }
}

macro list_each!($($v:expr),*) {        // variadic
    $( println!($v); )*
}

// Type-attached macros: instance form (uses `self`) and static form.
impl<T> Vector<T> {
    macro push_all!($($x:expr),*) { $( self.push($x); )* }
}

// Call sites:
bail!(n < 0, "negative");
list_each!(1, 2, 3);
v.push_all!(10, 20, 30);            // instance: v becomes self
Vector::new().push_all!(7, 8, 9);   // chained
```

Matchers: `$x:expr`, variadic `$($x:expr),*` with `,` or `;`
separators. Expansion runs between parse and typer; the typer sees
already-expanded AST.

`println!`, `print!`, `format!`, `panic!`, `printf` are codegen
builtins — they don't go through the macro_rules expander.

## string interpolation

```glide
let name: string = "world";
let s: string = "hello, ${name}!";
let n: int = 7;
let label: string = "count: ${n}, double: ${n * 2}";
```

`"...${expr}..."` is sugar — the parser lowers it to a `format!` call
with `{}` placeholders.

## inline asm and FFI escape hatches

```glide
// GCC operand syntax with output / input / clobber lists.
fn read_tsc() -> u64 {
    let lo: u32 = 0;
    let hi: u32 = 0;
    asm volatile { "rdtsc" : "=a"(lo), "=d"(hi) }
    return ((hi as u64) << 32) | (lo as u64);
}

// Naked: no prologue / epilogue, body must be only `asm`.
@cfg("posix")
naked fn add_raw(a: int, b: int) -> int {
    asm { "lea (%rdi,%rsi,1), %rax" : : : }
    asm { "ret" : : : }
}

// Per-platform gates.
@cfg("posix")  fn now_ms() -> i64 { ... }
@cfg("windows") fn now_ms() -> i64 { ... }

// Drop arbitrary C verbatim into the output. Useful for runtime
// helpers, intrinsics, or to call platform APIs that aren't in
// stdlib yet. Values from outer scope are interpolated.
c_raw! {
    int dup = _dup(_fileno(stdout));
    fflush(stdout);
}
```

## concurrency

```glide
fn worker(c: chan<int>) {
    c.send(42);
    c.close();
}

fn main() -> int {
    let c: chan<int> = make_chan(4);     // buffered, capacity 4
    spawn worker(c);                      // M:N coroutine
    let v: int = c.recv();                // parks the caller, frees worker
    return v;
}

// Drain until close.
while let v = c.recv() { use(v); }

// `sleep_ms` parks the coro on the timer thread; the worker is free
// to pick up another ready task immediately.
sleep_ms(100);

// `spawn_thread` is the escape hatch that runs on a real OS thread.
spawn_thread heavy_blocking_io();
```

Channels are bounded MPMC built on a Vyukov ring with cache-padded
cells. Blocking ops (`recv`, `send` to full chan, `sleep_ms`) park
the coroutine on a tiny park/unpark primitive without consuming a
worker thread. `c.recv()` on a closed empty channel returns a
zero-initialized `T`; `while let v = c.recv()` exits naturally on
close.

## FFI

```glide
extern fn printf(fmt: string, ...) -> int;     // C-style variadic
extern fn malloc(n: usize) -> *void;
extern fn free(p: *void);
```

`extern fn` declares a C function the runtime / system libc provides;
no body, no Glide-level checks beyond signature. Variadic is declared
with a trailing `...`. The compiler emits a forward declaration in the
generated C; the linker pulls the symbol from libc / pthread / the
emitted runtime.

For `#include`, system-specific glue, calling platform APIs, or
defining new C helpers, drop into `c_raw! { ... }` at top level (any
header introduced before its first use becomes available to later
emitted code):

```glide
c_raw! {
    #ifdef _WIN32
    #include <windows.h>
    static int now_ticks(void) { return GetTickCount(); }
    #else
    #include <time.h>
    static int now_ticks(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
    #endif
}

extern fn now_ticks() -> int;
```

## memory layout and ABI

Glide compiles to C99 + pthread. Structs follow the C ABI: same layout as the equivalent C struct. Generic struct instantiations are mangled with their type arguments (e.g. `Vector<int>` becomes `Vector__int`). Function pointers fit in a `void*` slot and cast at call sites.

## what's intentionally not in the language

- **Lifetimes / generic over lifetimes** — borrows are function-scoped
  only.
- **Reflection** — none. Generics + traits cover the structural cases;
  macros cover the syntactic ones.
- **Garbage collection** — explicitly excluded.
- **Nullable type (`?T`)** — pointers are nullable; borrows are non-null
  by typer enforcement. `?T` is on the roadmap.

## deferred (planned, not in 0.1.0)

- **`async fn` / `await`** — coroutines + `chan` + `sleep_ms` cover the
  cases an executor would; `async fn` syntax is on the roadmap once
  the reactor + parker work is exposed at the language level.
- **Stack growth** — coroutines have fixed-size stacks today. Lazy
  growth via mmap-backed regions is planned.
- **`Mutex<T>` / `select!`** — channels are the recommended sync
  primitive. A typed mutex and `select!` over multiple chans are
  planned.
- **Non-lexical lifetimes** — borrow lifetimes are block-scoped.
- **`move` returns** — owned values can't escape their declaring fn.
