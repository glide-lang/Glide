# Glide tutorial

Walks you from a fresh install through writing, building, and running Glide
programs. Assumes you've installed Glide from the releases page linked in the
README.

```bash
glide
```

Running `glide` with no args prints usage. If it does, you're set.

## hello world

Save this as `hello.glide`:

```glide
fn main() -> i32 {
    println!("hello, glide");
    return 0;
}
```

Run it:

```bash
glide run hello.glide
```

`glide run` compiles to a temp executable, runs it, then deletes it. To keep the
binary, use `glide build hello.glide -o hello`.

## values and bindings

The primitive types are the fixed-width integers `i8`–`i64` and `u8`–`u64`, the
pointer-sized `usize` / `isize`, the floats `f32` / `f64`, and `bool`, `char`,
`string`.

```glide
let x: i32 = 42;            // immutable
let mut y: i32 = 0;         // mutable
y = x + 1;
let pi: f64 = 3.14;
let ok: bool = true;
let name: string = "glide";
```

Type annotations are optional when the compiler can infer:

```glide
let v = Vector::new();      // T inferred from later v.push(...)
v.push(10);                 // T = i32
```

## functions

```glide
fn add(a: i32, b: i32) -> i32 {
    return a + b;
}

fn greet(name: string) {
    println!("hello,", name);
}
```

`fn` declarations stand alone or live inside `impl` blocks for methods.

## structs and methods

```glide
struct Point {
    x: i32,
    y: i32,
}

impl Point {
    fn distance(self: *Point, other: *Point) -> i32 {
        let dx: i32 = self.x - other.x;
        let dy: i32 = self.y - other.y;
        return dx * dx + dy * dy;
    }
}
```

Call it as `p.distance(&q)`. Only the *receiver* is auto-addressed: `p` is a
stack `Point` and the compiler takes its address to match `self: *Point`.
Arguments are not — pass `&q` explicitly (a borrow flows into a `*T`
parameter). An argument that is already a pointer (`let q: *Point = ...`)
passes bare.

## memory

You rarely manage memory by hand. A heap value is **owned by default**, **moves**
when you transfer it, and **frees itself at the end of its scope**.

**stack** — primitives and pure-data structs, automatic and copied by value:

```glide
let p: Point = Point { x: 1, y: 2 };   // lives on the stack
```

**owned heap** — a constructor that returns a heap value hands you ownership;
the binding frees it at scope end with no `free` written:

```glide
fn process() {
    let v = Vector::new();     // owned heap value
    v.push(10);
    v.push(20);
}                              // freed automatically here
```

Ownership **moves** on every transfer: returning it hands ownership to the
caller, passing it to a free function's `*T` parameter hands it to the callee,
and a let-rebind (`let b = a`) moves it to the new name. After a move the old
binding is gone, and reading it is a `use-after-move` compile error. A `&T`
parameter borrows instead of taking ownership, so the caller keeps it. Method
calls are the exception: a `*T` *argument* to a method (`recv.m(v)`) is
borrowed for the call, not moved — the caller still owns `v` afterwards.

**`own` fields** — a struct that owns heap data marks those fields `own`, and
the compiler frees the whole chain on drop:

```glide
struct List {
    head: own *Node,           // owned; freed recursively on drop
}
```

A bare `*T` field is a non-owning reference and is left untouched. `own T` is
shorthand for `own *T`.

**arena** — a bag of allocations with a shared lifetime, freed in one shot:

```glide
let arena: *Arena = Arena::new(4096);
defer arena.free();

let p: *Point = arena.alloc(sizeof(Point)) as *Point;
let q: *Point = arena.alloc(sizeof(Point)) as *Point;
// freed together when arena.free() runs
```

For fully manual, untracked memory (`new T { ... }` is not implemented — the
compiler rejects it), allocate raw and cast:

```glide
extern fn malloc(n: usize) -> *void;
let p: *Point = malloc(sizeof(Point)) as *Point;   // untracked
free(p as *void);                                  // you free it yourself
```

## borrows

`&T` and `&mut T` are non-owning views. They can't be null and can't outlive the
function.

```glide
fn touch(p: &Point) -> i32 {
    return p.x + p.y;
}

fn main() -> i32 {
    let p: Point = Point { x: 3, y: 4 };
    return touch(&p);
}
```

The compiler enforces:

- A value can have many `&T` viewers OR one `&mut T` viewer, not both
- A borrow can't be returned when its source was a local
- Two arguments to the same call can't alias the same variable if any is `&mut`

You never write lifetime annotations.

`&` takes the address of the *binding*, so it's for stack values. On a heap
owner (`let v = Vector::new()`, type `*Vector<i32>`) writing `&v` produces a
`**Vector<i32>` and is a type error against a `&Vector<i32>` parameter — pass
the owner bare (`peek(v)`); a `*T` coerces into a `&T` parameter.

## errors as values

`!T` is a result type and `?T` is an option type. `?` propagates errors. `ok`
and `err` build a result; `some` and `none` build an option.

```glide
fn parse_pos(n: i32) -> !i32 {
    if n < 0 { return err("negative"); }
    return ok(n * 2);
}

fn pipeline(n: i32) -> !i32 {
    let v: i32 = parse_pos(n)?;      // if err, return err from pipeline
    return ok(v + 1);
}

fn main() -> i32 {
    let r: !i32 = pipeline(5);
    return r.unwrap();               // 11
}
```

A `?`-bound `let` infers the unwrapped type, so `parse_pos(n)?` gives an `i32`,
not an `!i32`.

`unwrap()` panics (aborts the process) if the value is the failure case — use
it only after you've checked, and reach for `expr ?? fallback` when you want a
default value instead.

## generics

Type parameters use angle brackets. Inference works from arguments and from
later uses of the binding.

```glide
fn first<T>(v: *Vector<T>) -> T {
    return v.get(0);
}

let v = Vector::new();           // T deferred
v.push(42);                      // T = i32
let n: i32 = first(v);           // 42
```

`Vector<T>` is built in. `HashMap<V>`, strings, fs, os, env, io, time, http,
net, and math live under `src/stdlib/` and need an explicit `import`:

```glide
import stdlib::hashmap::*;

fn count(words: *Vector<string>) -> *HashMap<i32> {
    let m: *HashMap<i32> = HashMap::new();
    for let i: i32 = 0; i < words.len(); i++ {
        let w: string = words.get(i);
        let mut prev: i32 = 0;
        if m.contains(w) { prev = m.get(w); }
        m.insert(w, prev + 1);
    }
    return m;
}
```

Bounds on type parameters are checked at every call site:

```glide
fn max<T: Ord>(a: T, b: T) -> T { if a > b { return a; } return b; }
```

## traits

```glide
trait Greet {
    fn greet(self: Self) -> string;
    fn shout(self: Self) -> string { return self.greet(); }   // default
}

impl Greet for Cat { fn greet(self: Self) -> string { return "meow"; } }
impl Greet for Dog { fn greet(self: Self) -> string { return "woof"; } }

// Static dispatch: one specialization per type, no runtime cost.
fn say_hi<T: Greet>(g: T) { println!(g.greet()); }

// Dynamic dispatch via *dyn Trait: a fat pointer with a runtime vtable.
let pets: *Vector<*dyn Greet> = Vector::new();
pets.push(cat as *dyn Greet);
pets.push(dog as *dyn Greet);
for let i: i32 = 0; i < pets.len(); i++ {
    println!(pets.get(i).greet());
}
```

## concurrency

`chan<T>` is a typed bounded channel. `spawn` runs a function on the M:N
coroutine scheduler. Coroutines park and unpark on `recv` / `send` / `sleep_ms`
without blocking an OS thread.

```glide
fn worker(c: chan<i32>) {
    c.send(42);
    c.close();
}

fn main() -> i32 {
    let c: chan<i32> = make_chan(1);
    spawn worker(c);
    return c.recv();
}
```

Drain a channel until close with `while let`:

```glide
while let v = c.recv() { use(v); }
```

For a real OS thread, for example blocking I/O that doesn't go through the
reactor, use `spawn_thread fn_call(args)`. Data crosses coroutines through
channels.

## macros

`macro_rules!`-style expansion runs between parse and typer, so the typer sees
fully-expanded code.

```glide
macro bail!($cond:expr, $msg:expr) {
    if $cond { return err($msg); }
}

fn check(n: i32) -> !i32 {
    bail!(n < 0, "negative");
    return ok(n);
}

// Variadic.
macro list_each!($($v:expr),*) {
    $( println!($v); )*
}
list_each!(1, 2, 3);

// Type-attached.
impl<T> Vector<T> {
    macro push_all!($($x:expr),*) { $( self.push($x); )* }
}
let v: *Vector<i32> = Vector::new();
v.push_all!(10, 20, 30);
```

A macro whose body ends in `return <expr>;` produces a value when called in
expression position, so it works in a `let`, as an argument, or in a larger
expression. Macro locals are hygienic: renamed per expansion so they never clash
with caller variables.

```glide
macro ints!($($x:expr),*) {
    let v: *Vector<i32> = Vector::new();
    $( v.push($x); )*
    return v;
}
let nums = ints!(1, 2, 3);     // a real *Vector<i32>
```

The `vec_of!` builtin does exactly this for vectors, no import needed:

```glide
let v: *Vector<i32> = vec_of!(10, 20, 30);
```

## next steps

- `LANGUAGE.md` is the full reference.
- `glide check foo.glide` runs the type and borrow checker without codegen;
  `glide fmt foo.glide --write` applies the formatter.
- `glide test [path]` runs `*_test.glide` files (see `TESTING.md`).
- `glide lsp` is the language server; install the Zed or VS Code extension to
  get completion, hover, goto, and rename in your editor.
