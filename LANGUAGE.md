# Glide language reference

Targeted at someone who already knows a systems language. For a
learning-oriented walkthrough see `TUTORIAL.md`.

## lexical structure

### keywords

```
let const mut fn struct enum impl trait dyn type extern pub move naked own new
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
*                           dereference / pointer-type prefix
->                          fn return type
=>                          match arm
::                          path
?                           result / option propagation (postfix)
??                          default for a result / option
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
| `i8`–`i64`, `u8`–`u64` | fixed-width integers                                |
| `usize` `isize` | pointer-sized integers                                    |
| `f32` `f64`     | floating point                                            |
| `bool` `char` `string` | primitives. `string` is `const char*`             |
| `*T`            | pointer; the heap value is owned by its binding by default |
| `&T`            | shared borrow (non-null, no lifetime annotation)           |
| `&mut T`        | exclusive mutable borrow (non-null)                        |
| `[]T`           | slice: `{ data: *T, len: usize }`                         |
| `T<U, V>`       | generic instantiation                                      |
| `fn(A, B) -> R` | function pointer                                           |
| `!T`            | result: success carries `T`, error carries a `string`      |
| `?T`            | option: present carries `T`, or absent                     |
| `chan<T>`       | typed channel (bounded MPMC)                               |
| `*dyn Trait`    | fat pointer (vtable + data); runtime dispatch              |

## statements

```glide
let name[: T] = expr;             // immutable; owns the value if it's a heap value
let mut name[: T] = expr;         // mutable

const NAME[: T] = expr;           // file-scope or block-scope constant

return [expr];

expr;                             // expression statement (calls, assignments)

if cond { ... } else { ... }      // statement form: blocks of stmts
while cond { ... }
for init; cond; step { ... }
{ ... }                           // block
match scrutinee { Variant(b) => { ... } _ => { ... } }
match value { 1 => { ... } "s" => { ... } 'c' => { ... } _ => { ... } }
                                  // literal arms compare by value (strings
                                  // by bytes); a `_` arm is required

defer expr;                       // run at fn end / before return (LIFO)
spawn fn_call(args);              // run fn on the M:N coroutine scheduler
spawn_thread fn_call(args);       // run fn on a real OS thread (escape hatch)

import a::b::c;                   // module import: QUALIFIED access only (`c::X`)
import a::b::c::*;                // wildcard: every pub name into bare scope
import a::b::c::{X, Y};           // selective: X and Y into bare scope
import a::b::{c, d::{E}};         // nested group: modules c,d (qualified) + E bare
```

Module semantics are Rust-style: `import a::b::c;` grants only the qualified
form (`c::X`); using a name bare requires importing it by name (selective) or
via a wildcard.

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
    Variant2(i32, string),
}

[pub] trait Name {
    fn required(self: Self) -> i32;
    fn provided(self: Self) -> i32 { return 0; }   // default method
}

impl[<T>] Name[<T>] {
    fn method(self: *Name<T>, arg: i32) -> i32 { ... }
}

impl Trait for Type {
    fn required(self: Self) -> i32 { ... }
}

extern fn libc_function(args) -> ret;          // declare a C function
```

`pub` makes the symbol importable from other Glide files. Top-level visibility
defaults to private.

## memory model

A heap value is owned by default: the binding that creates it owns it, ownership
moves when you transfer it, and it frees itself at the end of its scope. You
never write `malloc` / `free` for the common case, and the compiler catches
use-after-move and double-free at compile time.

### stack values

Plain primitives and pure-data structs (`let p: T = T { ... }`, no pointer) live
on the stack and are copied on assignment / return-by-value.

### owned heap

A constructor that returns a heap value, or a struct literal bound through a
pointer, produces an owned value:

```glide
let v = Vector::new();                  // owned
let p: *Point = Point { x: 1, y: 2 };   // owned
```

The compiler frees it at the end of the enclosing block, by `p.free()` if the
type defines one, else a recursive drop of its owned fields, else a plain free.
An owned binding inside a `for` body frees once per iteration.

### move

Ownership moves on every transfer:

- `return v` hands ownership to the caller, which auto-drops it (move-out).
- `f(v)` where `f` takes a `*T` parameter by value hands ownership to the
  callee. A `&T` parameter borrows instead and leaves the caller owning it.
- `let b = a` and `a = b` move the value into the new binding.
- a struct literal `T { f: v }` moves `v` into the field.

Reading a binding after its value was moved is a `use-after-move` compile error.
Reassigning the binding revives it. `c = d` where `c` already owned a value frees
the old value first, so reassignment never leaks.

### `own` fields and recursive drop

A struct field marked `own` means the struct owns that heap value and frees it
on drop:

```glide
struct List {
    head: own *Node,        // owned; freed recursively on drop
}
```

A bare `*T` field is a non-owning reference, left untouched. `own T` is shorthand
for `own *T`. A struct that owns heap fields is heap-managed; a pure-data struct
stays a stack value with copy / move semantics.

### raw heap (`new`)

```glide
let p: *Point = new Point { x: 1, y: 2 };   // raw, not auto-dropped
```

`new` allocates a heap value the compiler does not track. You free it yourself
with `free(p as *void)`, or leak it intentionally (e.g. the program owns it
until exit).

### arenas

```glide
let arena: *Arena = Arena::new(4096);
defer arena.free();

let p: *Point = arena.alloc(sizeof(Point)) as *Point;
let raw: *void = arena.alloc(bytes);
```

Use arenas when you have a bag of allocations sharing a lifetime (parser nodes,
request-scoped data). `arena.free()` reclaims everything in `O(1)`;
`arena.reset()` rewinds it for reuse.

### borrows

`&T` and `&mut T` are non-null views with function-scoped lifetimes — and they
are **second-class**: a borrow flows *into* calls (parameters, call args) and
never *out* (returns, struct fields, generics, tuples, channels, stores). That
one rule is why Glide needs no lifetime annotations: a borrow can never outlive
the thing it points at, by construction. The borrow checker enforces:

| Code                     | Description                                                     |
| ------------------------ | --------------------------------------------------------------- |
| `borrow-return-type`     | a fn/method/trait signature can't return `&T` / `&mut T`        |
| `return-borrow`          | `return &x` is always rejected (no param pass-through)          |
| `borrow-in-field`        | `&T` / `&mut T` can't appear in a struct field (use `*T`)       |
| `borrow-in-generic`      | no borrows inside `Vector<…>` / `HashMap<…>` / tuples / `!` `?` |
| `borrow-escape`          | a borrow-typed value or `&mut x` can't be stored or sent        |
| `free-borrow`            | can't free through a borrow — only the owner frees              |
| `borrow-vector-element`  | `&mut v[i]` on a container — indexing copies; use `.get`/`.set` |
| `borrow-alias-in-call`   | same-root mutable aliasing in one call (`f(&mut s.a, &mut s.b)`, `s.m(&mut s)`) |
| `overlap-borrow`         | conflicting `&` / `&mut` in the same scope is rejected          |
| `assign-while-borrowed`  | writing a place while a live borrow views it (NLL: a borrow dies at its last use) |
| `use-while-mut-borrowed` | reading a source while a `&mut` of it is live — go through the borrow |
| `free-while-borrowed`    | freeing an owner while a borrow of it is live (the view would dangle) |
| `use-after-move`         | reading a value after it was moved is rejected (copying a `&mut` moves it) |

You never write lifetime annotations.

**The two-layer safety model.** The safe layer is values (structs are copied),
borrows (second-class, exclusive), and owned heap (`let v* =` auto-drop with
move tracking) — no dangling, no aliased mutation, no use-after-free. The
manual layer is raw `*T` with `malloc`/`free`: the FFI and data-structure
implementation layer, where `&x` used as a plain address-of is legal and the
programmer owns the aliasing. Crossing from a borrow to `*T` in a call
argument is allowed (flow-down); storing one is not.

## if-as-expression

`if` works as both a statement (above) and an expression. As an
expression, both branches must be a single expression of the same
type, and `else` is required:

```glide
let label: string = if x > 3 { "big" } else { "small" };
let r: i32 = (if x > 0 { 10 } else { -10 }) * 2;

// else-if chains compose:
let category: string =
    if x < 0 { "negative" } else if x == 0 { "zero" } else { "positive" };
```

Codegen lowers it to a C ternary, so there's no extra cost vs writing
the `cond ? a : b` shape by hand. Branches with multiple statements
still need the statement form (`let mut x = default; if cond { x = ... }`).

## errors and options as values

`!T` is a result and `?T` is an option. `ok` / `err` build a result; `some` /
`none` build an option. `?` propagates the failure case; `??` supplies a default.

```glide
fn parse(s: string) -> !i32 {
    if s.eq("") { return err("empty"); }
    return ok(42);
}

fn pipeline(s: string) -> !i32 {
    let n = parse(s)?;          // n is `i32`; the annotation is optional
    return ok(n + 1);
}

let v: i32 = parse("12") ?? 0;  // the value, or 0 on err
```

`?` only works inside a function whose return type is `!U` (or `?U`). Result
conversion happens at the propagation site: an `!T` with err `e` becomes `!U`
with err `e`.

A `?`-bound `let` infers the *unwrapped* type: `let n = parse(s)?` declares `n`
as `i32`, not `!i32`, so `n + 1` and `n.method()` work without an explicit
annotation.

`r.unwrap()` returns the inner value or a zero-initialized fallback if `r` is
the failure case. Use it at boundaries where you've already checked.

## generics

Function and struct type parameters use angle brackets; instantiation is
monomorphized.

```glide
struct Vec<T> { data: *T, len: i32, cap: i32 }

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

- An explicit return-type hint at the call site (`let v: *Vec<i32> = Vec::new()`)
- Argument types passed in (`first(v)` infers `T` from `v`)
- Method calls on a not-yet-bound `let v = Generic::new()` — the first call that
  uses a type parameter resolves it

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

impl Render for Square { fn render(self: Self) -> string { return "Square"; } }
impl Render for Circle { fn render(self: Self) -> string { return "Circle"; } }

// Static dispatch via generic bound (monomorphized per type).
fn show_all<T: Render>(items: *Vector<T>) { ... }

// Dynamic dispatch via *dyn Trait (fat pointer = vtable + data).
fn show(r: *dyn Render) { println!(r.render()); }

let shapes: *Vector<*dyn Render> = Vector::new();
shapes.push(square_p as *dyn Render);
shapes.push(circle_p as *dyn Render);
```

`Self` inside a trait body refers to the implementor's type. The
default-method walk substitutes `Self` for the concrete type when
copying a default body into an impl that doesn't override it.

## operator overloading

Operators dispatch to methods by name (no trait required on a concrete
type — the same convention `println!` uses to find `to_string`):

| Operator            | Method                                | Result            |
| ------------------- | ------------------------------------- | ----------------- |
| `==` `!=`           | `eq(self, other) -> bool`             | `bool` (`!=` negates) |
| `<` `<=` `>` `>=`   | `cmp(self, other) -> i32` (`-`/`0`/`+`) | `bool` (`cmp <op> 0`) |
| `+` `-` `*` `/` `%` | `add`/`sub`/`mul`/`div`/`rem` `-> T`  | the method's `T`  |

`==` and `!=` work out of the box:

- **strings** compare bytes (`"ale".concat("lo") == "alelo"` is `true`);
- **structs** and **tuples** compare field by field automatically, recursing
  into nested structs and string fields. Define your own `eq` to override
  this (e.g. to ignore a cache field). A self-referential field falls back to
  pointer identity so comparison always terminates.

Ordering and arithmetic need a method — a struct without one gets a clear
error telling you which to implement. Strings order via a built-in `cmp`,
and `+` on strings concatenates.

```glide
struct Vec2 { x: i32, y: i32 }
impl Vec2 {
    fn add(self: *Vec2, o: *Vec2) -> *Vec2 { let r: *Vec2 = Vec2 { x: self.x + o.x, y: self.y + o.y }; return r; }
    fn cmp(self: *Vec2, o: *Vec2) -> i32 { return (self.x + self.y) - (o.x + o.y); }
}
let c: *Vec2 = a + b;   // Vec2::add
if a < b { ... }        // a.cmp(b) < 0
```

The `Eq`, `Ord`, `Add`, `Sub`, `Mul`, `Div`, `Rem` marker traits (auto-injected,
like `Display`) let generic code bound on the capability: `fn max<T: Ord>(...)`.

## macros

User-defined `macro_rules!` for AST-level expansion.

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

### macros that return a value

When a macro body ends with `return <expr>;`, a call in **expression
position** expands to a block-expression that yields that value (the
same `{ ...; return v }` rule as a literal block). So the same macro
splices statements at statement position and produces a value when used
as one:

```glide
macro doubled!($x:expr) {
    let d: i32 = $x * 2;
    return d;
}

macro ints!($($x:expr),*) {
    let v: *Vector<i32> = Vector::new();
    $( v.push($x); )*
    return v;
}

let a = doubled!(21);                  // 42
let b = doubled!(5) + doubled!(10);    // 30 — inside an expression
let v = ints!(1, 2, 3);                // a real *Vector<i32>
```

Works in every call shape — bare `name!`, receiver `recv.name!`, and
`Type::name!` — and in any expression slot (let-init, argument, operand,
`return`). Macro-introduced locals are **hygienic**: they are renamed per
expansion, so `let tmp = $x` can't capture a caller variable named `tmp`.
A macro defined in terms of itself stops at a recursion limit with a
diagnostic rather than hanging.

### builtin macros

`println!`, `print!`, `format!`, `panic!`, `printf`, `dbg!`, and
`vec_of!` are codegen builtins — they don't go through the `macro_rules`
expander. `vec_of!(a, b, c)` builds a `*Vector<T>` (T = the element type)
and is available everywhere without an import:

```glide
let v: *Vector<i32> = vec_of!(10, 20, 30);
let names = vec_of!("alice", "bob");   // *Vector<string>
```

## string interpolation

```glide
let name: string = "world";
let s: string = "hello, ${name}!";
let n: i32 = 7;
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
naked fn add_raw(a: i32, b: i32) -> i32 {
    asm { "lea (%rdi,%rsi,1), %rax" : : : }
    asm { "ret" : : : }
}

// Per-platform gates.
@cfg("posix")  fn now_ms() -> i64 { ... }
@cfg("windows") fn now_ms() -> i64 { ... }

// `@cfg` also gates a single statement or `{ ... }` block inside a fn,
// so a function can keep one body and branch only the target-specific
// parts (each guarded stmt emits its own #ifdef / #ifndef _WIN32).
fn print_msg(msg: string) {
    @cfg("windows") asm! volatile {
        "subq $40, %%rsp" "movq %0, %%rcx" "call puts" "addq $40, %%rsp"
        : : "r"(msg) : "rcx", "rax", "memory"
    }
    @cfg("posix") asm! volatile {
        "movq %0, %%rdi" "call puts" : : "r"(msg) : "rdi", "rax", "memory"
    }
}

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
fn worker(c: chan<i32>) {
    c.send(42);
    c.close();
}

fn main() -> i32 {
    let c: chan<i32> = make_chan(4);     // buffered, capacity 4
    spawn worker(c);                      // M:N coroutine
    let v: i32 = c.recv();                // parks the caller, frees worker
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
close. `stdlib::sync` adds `Mutex<T>`, atomics, and a `WaitGroup`, and
`select!` waits on several channels at once.

## FFI

```glide
extern fn printf(fmt: string, ...) -> i32;     // C-style variadic
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

extern fn now_ticks() -> i32;
```

## memory layout and ABI

Glide compiles to C99 + pthread. Structs follow the C ABI: same layout as the
equivalent C struct. Generic struct instantiations are mangled with their type
arguments (e.g. `Vector<i32>` becomes `Vector__i32`). Function pointers fit in a
`void*` slot and cast at call sites.

## what's intentionally not in the language

- **Lifetimes / generic over lifetimes** — borrows are function-scoped only,
  and second-class: they cannot be returned or stored, so there is nothing a
  lifetime annotation would need to say.
- **Reflection** — none. Generics + traits cover the structural cases; macros
  cover the syntactic ones.
- **Garbage collection** — explicitly excluded; ownership and scope-based drop
  handle reclamation.
- **`async` / `await`** — the concurrency model is coroutines plus channels, so
  there is no separate async function colour.
