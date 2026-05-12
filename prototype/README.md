# NightScript — Python Prototype

This is the Python reference implementation of the NightScript compiler.
It was written to validate language design decisions (syntax, AST structure, type system, C codegen).

**Status: Reference only. Not used in production.**

The production compiler is being rewritten in C at `../compiler/`.

---

## What is implemented

| Module | File | Status |
|---|---|---|
| Token definitions | `compiler/night/token.py` | Complete |
| Lexer | `compiler/night/lexer.py` | Complete |
| Parser | `compiler/night/parser.py` | Complete |
| AST nodes | `compiler/night/ast.py` | Complete |
| Semantic analysis | `compiler/night/sema.py` | Complete |
| Type system | `compiler/night/typesys.py` | Complete |
| Type checker | `compiler/night/typecheck.py` | Complete |
| C code generator | `compiler/night/codegen_c.py` | Complete |
| CLI (`night build/run/check`) | `compiler/night/cli.py` | Complete |
| Error reporting | `compiler/night/errors.py` | Complete |

## What is NOT implemented

- `defer`
- `interface` + `impl Type : Interface`
- `packed struct`
- `Option[T]` / `Result[T, E]` as proper generic types
- `?` error propagation operator
- Generics `[T]`
- UI syntax (`window`, `button`, `label`)
- Kernel mode
- Async / await

## What works end-to-end

```
.afns source  →  Python compiler  →  generated .c  →  clang/gcc  →  native binary
```

Example (`examples/hello.afns`):

```afns
package main;

extern "C" fn puts(s: cstr) -> i32;

fn main() -> i32 {
    puts("Hello NightScript");
    return 0;
}
```

Generates correct C and compiles to a working native binary.
