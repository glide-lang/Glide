<p align="center">
  <img src="assets/glide-horizontal.svg" alt="Glide" width="320">
</p>

**Glide gives you memory safety, real concurrency, and the speed of C, without lifetime annotations, callbacks, or a garbage collector.**

A small systems language. Small enough to learn in an afternoon.

## the problems Glide solves

### "I want safety, but `'a, 'b, 'q: 'a` makes me cry."

Borrows in Glide are function-scoped. The compiler catches dangling references, double-free, aliased mutation, and owned values trying to escape their scope. You never type a single lifetime parameter.

```glide
fn dangle() -> &int {
    let x: int = 5;
    return &x;          // rejected at compile time
}
```

### "I want heap allocation without `free()` everywhere."

Declare a `*T` from a struct literal and Glide pairs the allocation with automatic cleanup at scope exit. When you have a pile of objects that all live and die together, reach for an arena.

```glide
fn process() {
    let p: *Point = Point { x: 1, y: 2 };   // allocates
    use(p);
}                                            // freed automatically
```

```glide
let arena: *Arena = Arena::new(4096);
defer arena.free();
let n1: *Node = arena.create(Node);
let n2: *Node = arena.create(Node);
// freed together when the arena drops
```

### "I want real concurrency, not callback soup."

Channels and `spawn`. Shared-nothing by design. No `Arc<Mutex<...>>`, no async coloring, no executor to configure.

```glide
fn worker(c: chan<int>) {
    send(c, 42);
}

fn main() -> int {
    let c: chan<int> = make_chan(1);
    spawn worker(c);
    return recv(c);
}
```

### "I want errors as values, not exceptions or `Result<T, Box<dyn Error>>`."

`!T` is the result type. `?` propagates errors. That's the whole story.

```glide
fn parse(n: int) -> !int {
    if n < 0 { return err("negative"); }
    return ok(n * 2);
}

fn double(n: int) -> !int {
    let v: int = parse(n)?;
    return ok(v + 1);
}
```

### "I want generics that aren't a PhD thesis."

Monomorphized, inferred from arguments and return-type hints. No `::<T>` turbofish needed in the common case.

```glide
let v: *Vector<int> = Vector::new();
v.push(10);
v.push(20);

let m: *HashMap<int> = HashMap::new();
m.insert("answer", 42);
```

### "I want interfaces without object-orientation."

`trait` declares a contract, `impl Trait for Type` provides it. Default methods + supertraits work. For heterogeneous collections, `*dyn Trait` does runtime dispatch via a small vtable.

```glide
trait Render {
    fn render(self: Self) -> string;
}

impl Render for Box    { fn render(self: Self) -> string { return "Box"; } }
impl Render for Circle { fn render(self: Self) -> string { return "Circle"; } }

fn show(r: *dyn Render) { println!(r.render()); }
```

### "I need to drop down to assembly without leaving the file."

Inline `asm { … }` blocks accept GCC-style operand constraints. `naked fn` lets you write your own calling convention. `@cfg("posix"|"windows")` gates definitions per platform. `c_raw! { … }` injects raw C verbatim.

```glide
fn read_tsc() -> u64 {
    let lo: u32 = 0;
    let hi: u32 = 0;
    asm volatile { "rdtsc" : "=a"(lo), "=d"(hi) }
    return ((hi as u64) << 32) | (lo as u64);
}
```

## install

Glide ships as a single archive that contains the compiler, a bundled C toolchain (Zig), and the stdlib. No system gcc, clang, or Rust required.

**Linux / macOS:**

```bash
curl -fsSL https://github.com/glide-lang/Glide/releases/latest/download/install.sh | bash
```

**Windows (PowerShell):**

```powershell
iwr https://github.com/glide-lang/Glide/releases/latest/download/install.ps1 -UseB | iex
```

Both install into a per-user directory (`~/.local/share/glide` on Linux/macOS, `%LOCALAPPDATA%\Programs\Glide` on Windows) and add a `glide` command to your PATH. No admin / sudo needed. Open a new terminal after install.

If you'd rather install from a downloaded archive, grab the right `.zip` / `.tar.gz` from the [releases page](https://github.com/glide-lang/Glide/releases) and run:

```bash
bash tools/install.sh --archive ./glide-linux-x86_64-0.1.0.tar.gz
```

```powershell
.\tools\install.ps1 -Archive .\glide-windows-x86_64-0.1.0.zip
```

## use

```bash
glide run hello.glide
glide build hello.glide -o hello
glide build hello.glide --target=x86_64-linux-gnu     # cross-compile
glide check hello.glide
glide emit hello.glide                                 # show generated C
```

Cross-compile targets are anything Zig supports: `x86_64-linux-{gnu,musl}`, `aarch64-linux-{gnu,musl}`, `x86_64-windows-{gnu,msvc}`, `aarch64-macos`, `x86_64-macos`, `riscv64-linux-musl`, etc.

## build from source

The compiler is written in Glide. To build it from scratch you only need a C compiler:

```bash
git clone <repo> && cd glide

# 1. Seed. -lws2_32 is needed on Windows for the TCP socket builtins;
#    POSIX skips it.
cc bootstrap/seed/bootstrap.c -o glide_seed -O2 -lpthread -lm -lws2_32   # Windows
cc bootstrap/seed/bootstrap.c -o glide_seed -O2 -lpthread -lm            # POSIX

bash tools/install_zig.sh                                          # 2. fetch Zig
./glide_seed build bootstrap/main.glide -o glide                   # 3. self-host
bash tools/build_release.sh                                        # 4. (optional) make a release archive
```

## tour

A single program covering every language feature lives in `examples/tour.glide`:

```bash
glide run examples/tour.glide
```

It exercises auto-drop, borrows, arenas, errors-as-values, generics with bounds, traits with `*dyn` dispatch, channels, spawn, sleep_ms, while-let-recv, closures, macros (variadic + non-variadic), string interpolation, qualified imports, inline `asm`, `naked fn`, and `@cfg` gates.

## benchmarks

Glide vs Go 1.26 on Windows 11, median of 5 runs. See `bench/RESULTS.md` for the full breakdown.

| bench                       | Glide     | Go     | result                  |
|-----------------------------|-----------|--------|-------------------------|
| Spawn + drain 100K          | 6 ms      | 10 ms  | Glide 1.7× faster       |
| Spawn + drain 1M            | 85 ms     | 94 ms  | Glide 1.1× faster       |
| Pure chan 1M (cap = 1024)   | 24 ms     | 48 ms  | Glide 2.0× faster       |
| RAM idle 100K parked        | 448 MB    | 903 MB | Glide 2.0× lighter      |
| Throughput spawn+chan 100K  | 486 ms    | 436 ms | Glide 1.1× off          |

## editor support

- **Zed**: install the `zed-extension/` folder as a dev extension.
- **VS Code**: load `vscode-extension/` via Extensions → Install from VSIX.
- **Tree-sitter grammar** for any other editor: `glide-grammar/` (highlights, indents, outline).
- The compiler ships an LSP server (`glide lsp`) with hover, definition, references, completion, document symbol, document highlight, rename + prepareRename, and formatting.

## license

MIT.
