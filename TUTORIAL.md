# Glide tutorial

Walks you from a fresh install through writing, building, and running Glide programs. Assumes you've installed Glide via the one-liner from the README.

```bash
glide
```

Running `glide` with no args prints usage. If it does, you're set.

## hello world

Save this as `hello.glide`:

```glide
fn main() -> int {
    println!("hello, glide");
    return 0;
}
```

Run it:

```bash
glide run hello.glide
```

`glide run` compiles to a temp executable, runs it, then deletes it. To keep the binary, use `glide build hello.glide -o hello`.

## values and bindings

Glide has a small set of primitive types: `int`, `uint`, `i32`, `i64`, `u32`, `u64`, `usize`, `isize`, `f32`, `f64`, `float`, `bool`, `char`, `string`.

```glide
let x: int = 42;            // immutable
let mut y: int = 0;         // mutable
y = x + 1;
let pi: float = 3.14;
let ok: bool = true;
let name: string = "glide";
```

Type annotations are optional when the compiler can infer:

```glide
let v = Vector::new();      // T inferred from later v.push(...)
v.push(10);                 // T = int
```

## functions

```glide
fn add(a: int, b: int) -> int {
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
    x: int,
    y: int,
}

impl Point {
    fn distance(self: *Point, other: *Point) -> int {
        let dx: int = self.x - other.x;
        let dy: int = self.y - other.y;
        return dx * dx + dy * dy;
    }
}
```

Call a method as `p.distance(q)`. The compiler auto-borrows where needed.

## memory

Three categories of values you actually allocate:

**stack** — value types, automatic:

```glide
let p: Point = Point { x: 1, y: 2 };   // lives on the stack
```

**owned heap with auto-drop** — your binding owns it; freed at scope exit:

```glide
fn process() {
    let v* = Vector::new();    // malloc'd
    v.push(10);
    v.push(20);
}                              // automatic v.free() here
```

The `*` after the binding name is the explicit "this is owned, clean up at scope end" marker. Works for any expression that returns a pointer.

**arena** — group of allocations with a shared lifetime:

```glide
let arena: *Arena = Arena::new(4096);
defer arena.free();

let p: *Point = arena.create(Point);
let q: *Point = arena.create(Point);
// freed together when arena.free() runs
```

## borrows

`&T` and `&mut T` are non-owning views. They can't be null and can't outlive the function.

```glide
fn touch(p: &Point) -> int {
    return p.x + p.y;
}

fn main() -> int {
    let p: Point = Point { x: 3, y: 4 };
    return touch(&p);
}
```

The compiler enforces:

- A value can have many `&T` viewers OR one `&mut T` viewer, not both
- A borrow can't be passed back as a function return when its source was a local
- Two arguments to the same call can't alias the same variable if any is `&mut`

You never write lifetime annotations.

## errors as values

`!T` is a result type. `?` propagates errors. `ok` and `err` build them.

```glide
fn parse_pos(n: int) -> !int {
    if n < 0 { return err("negative"); }
    return ok(n * 2);
}

fn pipeline(n: int) -> !int {
    let v: int = parse_pos(n)?;     // if err, return err from pipeline
    return ok(v + 1);
}

fn main() -> int {
    let r: !int = pipeline(5);
    return unwrap(r);                // 11
}
```

## generics

Type parameters use angle brackets. Inference works from arguments and from later uses of the binding.

```glide
fn first<T>(v: *Vector<T>) -> T {
    return v.get(0);
}

let v = Vector::new();           // T deferred
v.push(42);                      // T = int
let n: int = first(v);           // 42
```

`Vector<T>` is auto-injected from `src/builtins/`. `HashMap<V>`,
strings, fs, os, env, io, time, http, net, math, etc. live under
`src/stdlib/` and need an explicit `import`:

```glide
import stdlib::hashmap::*;

fn count(words: *Vector<string>) -> *HashMap<int> {
    let m: *HashMap<int> = HashMap::new();
    for let i: int = 0; i < words.len(); i++ {
        let w: string = words.get(i);
        let mut prev: int = 0;
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

// Static dispatch (one specialization per type, no runtime cost).
fn say_hi<T: Greet>(g: T) { println!(g.greet()); }

// Dynamic dispatch via *dyn Trait — fat pointer, runtime vtable.
let pets: *Vector<*dyn Greet> = Vector::new();
pets.push(cat as *dyn Greet);
pets.push(dog as *dyn Greet);
for let i: int = 0; i < pets.len(); i++ {
    println!(pets.get(i).greet());
}
```

## concurrency

`chan<T>` is a typed bounded channel. `spawn` runs a function on the
M:N coroutine scheduler — coroutines park / unpark on
`recv` / `send` / `sleep_ms` without blocking an OS thread.

```glide
fn worker(c: chan<int>) {
    c.send(42);
    c.close();
}

fn main() -> int {
    let c: chan<int> = make_chan(1);
    spawn worker(c);
    return c.recv();
}
```

Drain a channel until close with `while let`:

```glide
while let v = c.recv() { use(v); }
```

For a real OS thread (e.g. blocking I/O that doesn't go through the
reactor) use `spawn_thread fn_call(args)`. Shared-nothing by design:
data crosses goroutines through channels.

## macros

`macro_rules!`-style expansion runs between parse and typer, so the
typer sees fully-expanded code.

```glide
macro bail!($cond:expr, $msg:expr) {
    if $cond { return err($msg); }
}

fn check(n: int) -> !int {
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
let v: *Vector<int> = Vector::new();
v.push_all!(10, 20, 30);
```

## next steps

- `examples/tour.glide` walks through every feature in one file —
  `glide run examples/tour.glide` exercises auto-drop, borrows,
  arenas, errors-as-values, generics with bounds, traits + `*dyn`,
  channels, spawn, macros, string interpolation, inline `asm`,
  `naked fn`, `@cfg`, `c_raw!`.
- `LANGUAGE.md` is the full reference.
- `glide check foo.glide` runs the type+borrow checker without
  codegen; pair with `--write` to apply formatter changes.
- `glide test [path]` runs `*_test.glide` files (see `TESTING.md`).
- `glide lsp` is the LSP server — install the Zed or VS Code
  extension to get completion, hover, goto, rename in your editor.
