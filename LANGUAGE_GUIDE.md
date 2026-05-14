# NightScript Language Guide
### ApexForge NightScript — v0.1 → v0.4

---

## Mündəricat

1. [Giriş](#giriş)
2. [Layihə Quruluşu](#layihə-quruluşu)
3. [v0.1 — Əsas Dil](#v01--əsas-dil)
4. [v0.2 — Tiplər, Enumlar, İnterfeyslər](#v02--tiplər-enumlar-interfeyslər)
5. [v0.3 — Göstəricilər, Dilimler, Defer, Try](#v03--göstəricilər-dilimler-defer-try)
6. [v0.4 — UI Tətbiqləri](#v04--ui-tətbiqləri)
7. [Standart Kitabxana](#standart-kitabxana)
8. [Kompilyator Komandaları](#kompilyator-komandaları)
9. [Tam Nümunələr](#tam-nümunələr)

---

## Giriş

NightScript (`.afns` fayl uzantısı) aşağı səviyyəli, sistem proqramlaması üçün
nəzərdə tutulmuş, UI dəstəkli bir dil.  C-yə transpilyasiya edir.  Sintaksis
Rust-a bənzəyir, lakin daha yüngüldür.

Kompilyator:

```
night check  <fayl.afns>        # yalnız yoxlayır
night build  <fayl.afns>        # ikili fayl yaradır
night run    <fayl.afns>        # dərhal icra edir
night codegen <fayl.afns>       # C kodu çıxarır
night ast    <fayl.afns>        # AST çap edir
night fmt    <fayl.afns>        # kodu formatlar
night init   [qovluq]           # yeni layihə yaradır
night test   [layihə-qovluğu]   # testləri icra edir
```

---

## Layihə Quruluşu

```
myproject/
├── night.toml          ← layihə konfiqürasiyası
└── src/
    └── main.afns
```

**`night.toml` nümunəsi:**

```toml
[package]
name        = "myproject"
version     = "0.1.0"
author      = "Ad Soyad"
description = "Layihənin təsviri"

[target]
mode    = "native"
backend = "c"

[build]
entry        = "src/main.afns"
output       = "myproject"
optimization = "debug"      # "debug" → -g | "release" → -O2

[deps]
# dep_adi = "^1.0"
```

---

## v0.1 — Əsas Dil

### Paket Bəyanatı

Hər fayl `package` ilə başlamalıdır:

```nightscript
package myapp;
```

Nöqtə ilə alt-paketlər yaratmaq mümkündür:

```nightscript
package math.utils;
```

### Dəyişənlər

```nightscript
let x: i32 = 10;        // tip annotasiyası ilə
let y = 20;             // tip çıxarımı (i32)
let name: str = "NightScript";
let flag: bool = true;
```

Dəyişənlər default olaraq dəyişdirilə bilər:

```nightscript
let count: i32 = 0;
count = count + 1;      // icazə verilir
count += 5;             // mürəkkəb mənimsətmə
```

### İbtidai Tiplər

| Tip      | Ölçü    | Təsvir                    |
|----------|---------|---------------------------|
| `i8`     | 1 bayt  | işarəli tam ədəd          |
| `i16`    | 2 bayt  | işarəli tam ədəd          |
| `i32`    | 4 bayt  | işarəli tam ədəd (default)|
| `i64`    | 8 bayt  | işarəli tam ədəd          |
| `isize`  | platform| işarəli pointer ölçüsü   |
| `u8`     | 1 bayt  | işarəsiz tam ədəd         |
| `u16`    | 2 bayt  | işarəsiz tam ədəd         |
| `u32`    | 4 bayt  | işarəsiz tam ədəd         |
| `u64`    | 8 bayt  | işarəsiz tam ədəd         |
| `usize`  | platform| işarəsiz pointer ölçüsü  |
| `f32`    | 4 bayt  | üzən nöqtəli             |
| `f64`    | 8 bayt  | üzən nöqtəli             |
| `bool`   | 1 bayt  | `true` / `false`          |
| `char`   | 1 bayt  | ASCII simvol              |
| `str`    | —       | daxili string görünüşü    |
| `cstr`   | —       | C null-bitişik string     |

### Funksiyalar

```nightscript
fn add(a: i32, b: i32) -> i32 {
    return a + b;
}

fn greet(name: str) -> void {
    puts(name);
}

fn main() -> i32 {
    let result: i32 = add(3, 7);
    return 0;
}
```

### Operatorlar

**Arifmetik:**
```nightscript
let a = 10 + 3;   // 13
let b = 10 - 3;   // 7
let c = 10 * 3;   // 30
let d = 10 / 3;   // 3  (tam bölmə)
let e = 10 % 3;   // 1  (qalıq)
let f = -a;       // mənfi
```

**Müqayisə:**
```nightscript
a == b    // bərabər
a != b    // bərabər deyil
a < b     // kiçik
a > b     // böyük
a <= b    // kiçik və ya bərabər
a >= b    // böyük və ya bərabər
```

**Məntiqi:**
```nightscript
true && false   // VƏ
true || false   // VƏ ya
!true           // İNKAR
```

**Bit əməliyyatları:**
```nightscript
let x: i32 = 10;
let y: i32 = 12;
x & y     // bit VƏ  → 8
x | y     // bit VƏ ya → 14
x ^ y     // bit XOR → 6
~x        // bit İNKAR → -11
x << 2    // sola sürüşmə → 40
x >> 1    // sağa sürüşmə → 5
```

**Mürəkkəb mənimsətmə:**
```nightscript
x += 5;   x -= 2;   x *= 3;
x /= 4;   x %= 7;
x &= y;   x |= y;   x ^= y;
x <<= 1;  x >>= 1;
```

**Tip çevirmə (`as`):**
```nightscript
let i: i32 = 65;
let u: u8  = i as u8;
let f: f64 = i as f64;
let big: i64 = i as i64;
```

### Şərt Operatoru

```nightscript
if x > 0 {
    puts("müsbət");
} else if x < 0 {
    puts("mənfi");
} else {
    puts("sıfır");
}
```

### Dövrələr

**`while`:**
```nightscript
let i: i32 = 0;
while i < 10 {
    i += 1;
}
```

**`for` (C-stilindən ilhamlanan):**
```nightscript
for let i: i32 = 0; i < 10; i += 1 {
    // i = 0..9
}
```

**`break` və `continue`:**
```nightscript
while true {
    if done { break; }
    if skip { continue; }
}
```

### Sabitlər

```nightscript
const MAX: i32 = 100;
const PI: f64 = 3.14159;
```

### Xarici Funksiyalar (C birliyi)

C kitabxanasından funksiyaları idxal etmək:

```nightscript
extern "C" fn puts(s: cstr) -> i32;
extern "C" fn printf(fmt: cstr) -> i32;
extern "C" fn malloc(size: usize) -> *u8;
extern "C" fn free(ptr: *u8) -> void;
```

---

## v0.2 — Tiplər, Enumlar, İnterfeyslər

### Strukturlar

```nightscript
struct Point {
    x: i32;
    y: i32;
}

// yaratmaq:
let p: Point = Point { x: 3, y: 4 };

// sahəyə müraciət:
let xval: i32 = p.x;
```

**Paketlənmiş struct (C `__attribute__((packed))`):**
```nightscript
packed struct Header {
    magic:   u32;
    version: u16;
    flags:   u8;
}
```

### İmplementasiya Blokları

Strukturlara metodlar əlavə etmək:

```nightscript
struct Counter {
    value: i32;
}

impl Counter {
    fn new() -> Counter {
        return Counter { value: 0 };
    }

    fn inc(self: *Counter) -> void {
        self.value = self.value + 1;
    }

    fn get(self: *Counter) -> i32 {
        return self.value;
    }
}

fn main() -> i32 {
    let c: Counter = Counter.new();
    Counter.inc(&c);
    Counter.inc(&c);
    let v: i32 = Counter.get(&c);   // v == 2
    return 0;
}
```

> **Qeyd:** `self` parametri həmişə `*TipAdı` olmalıdır (pointer vasitəsilə).

### Enumlar

**Sadə enum:**
```nightscript
enum Direction {
    North;
    South;
    East;
    West;
}

let d: Direction = Direction.North;
```

**Data daşıyan enum (tagged union):**
```nightscript
enum Shape {
    Circle(radius: i32);
    Rect(w: i32, h: i32);
    None;
}

let s: Shape = Shape.Circle(5);
let r: Shape = Shape.Rect(4, 3);
let n: Shape = Shape.None;
```

### Match İfadəsi

```nightscript
let result: i32 = match direction {
    Direction.North => 1,
    Direction.South => 2,
    Direction.East  => 3,
    Direction.West  => 4,
};

// wildcard:
let val: i32 = match x {
    Direction.North => 10,
    _               => 0,
};
```

**Data binding:**
```nightscript
match shape {
    Shape.Circle(r) => {
        let area: i32 = r * r;
    },
    Shape.Rect(w, h) => {
        let area: i32 = w * h;
    },
    Shape.None => {},
}
```

### İnterfeyslər

```nightscript
interface Drawable {
    fn draw(self: *Self) -> void;
    fn area(self: *Self) -> i32;
}

struct Circle {
    radius: i32;
}

impl Circle : Drawable {
    fn draw(self: *Circle) -> void {
        puts("dairə çəkildi");
    }
    fn area(self: *Circle) -> i32 {
        return self.radius * self.radius;
    }
}
```

> **Qeyd:** `impl Tip : İnterfeys` bütün interfeys metodlarını tələb edir. Əksik metod olduqda kompilyasiya xətası baş verir.

### `pub` Görünürlük

```nightscript
package mylib;

pub fn exported_fn() -> i32 { return 42; }

fn private_fn() -> i32 { return 0; }    // başqa paketdən görünməz

pub struct PublicData {
    pub_field: i32;
}
```

### İdxal / Paketlər

```nightscript
package main;

import mylib;

fn main() -> i32 {
    let v: i32 = mylib.exported_fn();
    return 0;
}
```

### Option[T]

```nightscript
fn safe_div(a: i32, b: i32) -> Option[i32] {
    if b == 0 { return None; }
    return Some(a / b);
}

let result: Option[i32] = safe_div(10, 2);

let val: i32 = match result {
    Some => 1,
    None => 0,
};
```

### Result[T, E]

```nightscript
enum MyError { DivByZero; Overflow; }

fn divide(a: i32, b: i32) -> Result[i32, MyError] {
    if b == 0 { return Err(MyError.DivByZero); }
    return Ok(a / b);
}

let r: Result[i32, MyError] = divide(10, 2);

match r {
    Ok  => { puts("uğurlu"); },
    Err => { puts("xəta");   },
}
```

### Unionlar

```nightscript
union Data {
    as_i32: i32;
    as_f32: f32;
    as_bytes: [4]u8;
}
```

---

## v0.3 — Göstəricilər, Dilimler, Defer, Try

### Göstəricilər

```nightscript
let x: i32 = 10;
let p: *i32 = &x;      // ünvan al
let val: i32 = *p;     // dereference et

// funksiyaya pointer ötür:
fn increment(p: *i32) -> void {
    *p = *p + 1;
}

increment(&x);          // x indi 11-dir
```

**Null pointer:**
```nightscript
let p: *u8 = null;
```

### Massivlər

```nightscript
let arr: [5]i32 = [1, 2, 3, 4, 5];
let first: i32 = arr[0];
let len: usize = arr.len;
```

### Dilimlər (Slices)

```nightscript
let slice: []i32 = arr[1..4];    // [2, 3, 4]
let elem: i32 = slice[0];        // 2
let slen: usize = slice.len;     // 3
```

### String Əməliyyatları

```nightscript
let s: str = "Salam";
let c: u8   = s[0];             // 'S' → 83
let sub: str = s[0..3];         // "Sal"
let len: usize = s.len;
```

### `unsafe` Blokları

Pointer arifmetikası və ham C əməliyyatları üçün:

```nightscript
unsafe {
    let buf: *u8 = malloc(64);
    memset(buf, 0, 64);
    free(buf);
}
```

Funksiya daxilindən `unsafe` blokunda return etmək mümkündür:

```nightscript
fn raw_read(p: *i32) -> i32 {
    unsafe {
        return *p;
    }
}
```

### `defer` İfadəsi

Funksiyadan çıxmazdan əvvəl icra edilir (LIFO sırası):

```nightscript
fn open_and_process() -> i32 {
    let f: *u8 = fopen("fayl.txt", "r");
    defer fclose(f);              // funksiyadan çıxanda icra olunur

    defer puts("üçüncü");
    defer puts("ikinci");
    defer puts("birinci");       // ← ən əvvəl icra olunur

    // ... işlər
    return 0;
}
```

### `?` Try Operatoru

`Result` və ya `Option` dəyərini avtomatik ötürür:

```nightscript
fn step(v: i32) -> Result[i32, MyError] {
    if v < 0 { return Err(MyError.Overflow); }
    return Ok(v + 1);
}

fn chain(v: i32) -> Result[i32, MyError] {
    let a = step(v)?;    // xəta varsa dərhal return Err
    let b = step(a)?;
    return Ok(b);
}
```

### Paketlənmiş Strukturlar

```nightscript
packed struct EthernetHeader {
    dst:  [6]u8;
    src:  [6]u8;
    etype: u16;
}
```

---

## v0.4 — UI Tətbiqləri

UI tətbiqləri SDL2 backend-dən istifadə edir. Kompilyasiya zamanı `-lSDL2` avtomatik əlavə olunur.

### UI App Sintaksisi

```nightscript
package myapp;

ui app MyApp {
    window("Başlıq") {
        width:  800;
        height: 600;

        label("Xoş Gəldiniz!") {}

        button("Tıkla") {
            onClick {
                // tıklama hadisəsi
                let count: i32 = 1;
            }
        }

        row {
            button("Sol")  {}
            button("Sağ")  {}
        }

        input("Adınızı daxil edin") {}
    }
}
```

### UI Elementləri

| Element    | Təsvir                        |
|------------|-------------------------------|
| `window`   | əsas pəncərə                  |
| `button`   | klik edilə bilən düymə        |
| `label`    | mətn etiketi                  |
| `input`    | mətn daxiletmə sahəsi         |
| `row`      | üfüqi düzülüş konteyneri      |
| `column`   | şaquli düzülüş konteyneri     |
| `panel`    | qruplaşdırma konteyneri       |
| `canvas`   | rəsm sahəsi                   |
| `menu`     | menyu konteyneri              |

### Xassələr

```nightscript
button("OK") {
    width:  120;
    height: 40;
    x:      50;       // mütləq mövqe
    y:      100;
}
```

### Hadisə İşləyiciləri

```nightscript
button("Klikle") {
    onClick {
        // düymə tıklandı
        let action: i32 = 1;
    }
    onKey {
        // klaviatura hadisəsi
    }
}

input("Daxil et") {
    onChange {
        // mətn dəyişdi
    }
}
```

---

## Standart Kitabxana

Standart kitabxana `$NIGHT_STDLIB` mühit dəyişəni ilə konfiqurasiya edilir.

### `io.print`

```nightscript
import io.print;

io.print.print("Salam Dünya");
io.print.print_i32(42);
io.print.print_bool(true);
io.print.newline();
```

### `core.math`

```nightscript
import core.math;

let a: i32  = core.math.abs_i32(-5);      // 5
let m: i32  = core.math.min_i32(3, 7);   // 3
let M: i32  = core.math.max_i32(3, 7);   // 7
let c: i32  = core.math.clamp_i32(x, 0, 100);
let p: i32  = core.math.pow_i32(2, 8);   // 256
```

### `core.mem`

```nightscript
import core.mem;

let buf: *u8 = core.mem.alloc(64);        // malloc
core.mem.zero(buf, 64);                   // memset(0)
core.mem.dealloc(buf);                    // free
```

### `alloc.buf`

```nightscript
import alloc.buf;

let b: alloc.buf.Buf = alloc.buf.buf_new();
alloc.buf.buf_push(&b, 65);              // 'A' bayt əlavə et
let len: usize = alloc.buf.buf_len(&b); // 1
alloc.buf.buf_free(&b);
```

---

## Kompilyator Komandaları

### Tək fayl yoxlama

```bash
night check src/main.afns
```

### İkili fayl yaratma

```bash
night build src/main.afns -o program
```

### Dərhal icra

```bash
night run src/main.afns
```

### Layihə ilə işləmək

```bash
night init myproject          # yeni layihə yaradır
cd myproject
night build                   # night.toml-dan oxuyur
night run
night clean
```

### Test icra etmək

```bash
night test                    # tests/ qovluğundakı faylları yoxlayır
```

### Hədəf arxitekturası

```bash
night build src/main.afns --target aarch64
```

### Optimizasiya

`night.toml`-da:
```toml
[build]
optimization = "release"    # -O2 ilə kompilyasiya edir
# optimization = "debug"    # -g ilə (default)
```

---

## Tam Nümunələr

### Nümunə 1: Fibonacci

```nightscript
package fibonacci;

extern "C" fn puts(s: cstr) -> i32;

fn fib(n: i32) -> i32 {
    if n <= 1 { return n; }
    return fib(n - 1) + fib(n - 2);
}

fn main() -> i32 {
    if fib(10) == 55 { puts("ok"); }
    return 0;
}
```

### Nümunə 2: Stack Məlumat Strukturu

```nightscript
package stack;

extern "C" fn puts(s: cstr) -> i32;

const STACK_MAX: i32 = 64;

struct Stack {
    data: [64]i32;
    top:  i32;
}

impl Stack {
    fn new() -> Stack {
        return Stack { data: [0; 64], top: -1 };
    }

    fn push(self: *Stack, val: i32) -> bool {
        if self.top >= STACK_MAX - 1 { return false; }
        self.top = self.top + 1;
        self.data[self.top as usize] = val;
        return true;
    }

    fn pop(self: *Stack) -> Option[i32] {
        if self.top < 0 { return None; }
        let v: i32 = self.data[self.top as usize];
        self.top = self.top - 1;
        return Some(v);
    }

    fn is_empty(self: *Stack) -> bool {
        return self.top < 0;
    }
}

fn main() -> i32 {
    let s: Stack = Stack.new();
    Stack.push(&s, 10);
    Stack.push(&s, 20);
    Stack.push(&s, 30);

    let v: Option[i32] = Stack.pop(&s);
    let ok: i32 = match v {
        Some => 1,
        None => 0,
    };

    if ok == 1 { puts("pop ok"); }
    return 0;
}
```

### Nümunə 3: Şəklə Çəkmə İnterfeysi

```nightscript
package shapes;

extern "C" fn puts(s: cstr) -> i32;

interface Shape {
    fn area(self: *Self)      -> i32;
    fn perimeter(self: *Self) -> i32;
    fn describe(self: *Self)  -> void;
}

struct Rect {
    w: i32;
    h: i32;
}

impl Rect : Shape {
    fn area(self: *Rect) -> i32 {
        return self.w * self.h;
    }
    fn perimeter(self: *Rect) -> i32 {
        return 2 * (self.w + self.h);
    }
    fn describe(self: *Rect) -> void {
        puts("Düzbucaqlı");
    }
}

struct Circle {
    radius: i32;
}

impl Circle : Shape {
    fn area(self: *Circle) -> i32 {
        return self.radius * self.radius;   // təxmini (π olmadan)
    }
    fn perimeter(self: *Circle) -> i32 {
        return 6 * self.radius;             // 2πr ≈ 6r
    }
    fn describe(self: *Circle) -> void {
        puts("Dairə");
    }
}

fn main() -> i32 {
    let r: Rect   = Rect   { w: 4, h: 3 };
    let c: Circle = Circle { radius: 5 };

    Rect.describe(&r);
    if Rect.area(&r) == 12 { puts("alan ok"); }

    Circle.describe(&c);
    if Circle.area(&c) == 25 { puts("alan ok"); }

    return 0;
}
```

### Nümunə 4: Error Zənciri

```nightscript
package errchain;

extern "C" fn puts(s: cstr) -> i32;

enum ParseError {
    Empty;
    InvalidChar;
    Overflow;
}

fn parse_positive(s: str) -> Result[i32, ParseError] {
    if s.len == 0 { return Err(ParseError.Empty); }

    let result: i32 = 0;
    let i: usize = 0;
    while i < s.len {
        let c: u8 = s[i];
        if c < 48 || c > 57 {
            return Err(ParseError.InvalidChar);
        }
        result = result * 10 + (c as i32 - 48);
        i += 1;
    }

    if result < 0 { return Err(ParseError.Overflow); }
    return Ok(result);
}

fn double_parse(s: str) -> Result[i32, ParseError] {
    let v = parse_positive(s)?;    // xəta varsa dərhal geri qayıdır
    return Ok(v * 2);
}

fn main() -> i32 {
    let r1: Result[i32, ParseError] = double_parse("21");
    let r2: Result[i32, ParseError] = double_parse("");
    let r3: Result[i32, ParseError] = double_parse("abc");

    let ok1: i32 = match r1 { Ok => 1, Err => 0 };
    let ok2: i32 = match r2 { Ok => 0, Err => 1 };
    let ok3: i32 = match r3 { Ok => 0, Err => 1 };

    if ok1 == 1 { puts("parse ok"); }
    if ok2 == 1 { puts("empty ok"); }
    if ok3 == 1 { puts("invalid ok"); }

    return 0;
}
```

### Nümunə 5: UI Tətbiqi (v0.4)

```nightscript
package calculator;

ui app Calculator {
    window("Kalkulyator") {
        width:  400;
        height: 500;

        label("Hesablayıcı") {}

        input("Birinci rəqəm") {}

        input("İkinci rəqəm") {}

        row {
            button("Topla") {
                onClick {
                    let result: i32 = 0;
                }
            }
            button("Çıxar") {
                onClick {
                    let result: i32 = 0;
                }
            }
        }

        row {
            button("Vur") {
                onClick {
                    let result: i32 = 0;
                }
            }
            button("Böl") {
                onClick {
                    let result: i32 = 0;
                }
            }
        }

        label("Nəticə: 0") {}
    }
}
```

---

## Tez Keçid Cədvəli

| Xüsusiyyət                | Sintaksis                                      |
|---------------------------|------------------------------------------------|
| Dəyişən                   | `let x: i32 = 5;`                             |
| Sabit                     | `const MAX: i32 = 100;`                       |
| Funksiya                  | `fn foo(a: i32) -> i32 { ... }`               |
| Struct                    | `struct S { x: i32; y: i32; }`                |
| Enum                      | `enum E { A; B(v: i32); }`                    |
| İmplementasiya            | `impl S { fn m(self: *S) -> void { ... } }`   |
| İnterfeys                 | `interface I { fn m(self: *Self) -> void; }`  |
| İnterfeys impl            | `impl S : I { fn m(self: *S) -> void { ... } }` |
| Şərt                      | `if x > 0 { } else { }`                       |
| While                     | `while x < 10 { }`                            |
| For                       | `for let i: i32 = 0; i < 10; i += 1 { }`     |
| Match                     | `match x { E.A => 1, _ => 0 }`                |
| Option                    | `Some(v)` / `None`                            |
| Result                    | `Ok(v)` / `Err(e)`                            |
| Try                       | `let v = expr?;`                              |
| Defer                     | `defer expr;`                                 |
| Unsafe                    | `unsafe { ... }`                              |
| Pointer al                | `&x`                                          |
| Pointer oxu               | `*p`                                          |
| Tip çevirmə               | `x as i64`                                   |
| Paket                     | `package foo;`                                |
| İdxal                     | `import foo.bar;`                             |
| Xarici C                  | `extern "C" fn f(x: i32) -> i32;`            |
| UI App                    | `ui app Name { window("T") { ... } }`        |
