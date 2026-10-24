# The checker

How `src/check.c` resolves names, works out types, lays out structs, makes generic instances and runs comptime.

## What it takes and gives

`checkProgram(Arena *, Program *)` takes the parsed modules and fills in the second half of every `Node`: `type`, `ref`, `isConst` and `cval`, `builtin`, `scope` and `module`. It also lays out every struct and adds the copies of generic code to `Program.instances`. It returns false after errors. The tree it leaves is what the IR writer reads. There is no separate lowered form.

Type checking is a pass of its own, after parsing. A compiler that checks while it parses often meets half a declaration when it needs all of it.

Most compile errors come from here: an unknown name, a wrong type, an error that is not handled, a `match` that misses an enum case. `error(c, node, ...)` reports at a node's place through `errorAt` in `src/diag.c`, which prints the file, line and column, the source line, and a mark under the problem:

```output
main.iris:3:11: error: x is not declared
    print("{x}")
            ^
```

The checker reports every error it finds in one run, and the compiler stops before writing any IR. The type `tyError` fits anywhere, so one mistake doesn't produce a cascade of follow-up errors.

## The passes

`checkProgram` walks all top-level declarations several times, in this order:

1. Every module gets a `Scope` for its names, with `core`'s scope as its parent. Every file gets a scope for its imports, with the module's scope as its parent.
2. `expandWhens` keeps only the chosen branch of each top-level `when` (`os`, `arch` and `mode`, from `gTarget`), so code for other platforms is never checked. `expandTests` turns `test` and `bench` blocks of the program's own module into functions (`F_TESTFN`, `F_BENCH`) when `gWithTests` is set, and drops them otherwise.
3. `declareGlobal` adds each function, constant, global, struct, enum, interface and type alias to its module's scope.
4. `qualify` rewrites `fs.readAll` into one `N_IDENT` named `"fs.readAll"`. It checks that the name exists in the module and is `pub`.
5. Methods are attached to their struct's `Type.methods`, so interfaces can see them. Methods of generic structs go to `genericMethods` and wait for instances.
6. Structs are resolved (`resolveStruct`, which calls `layoutStruct`), enums get their `Type` (`enumType`), and type aliases are resolved.
7. `checkSignature` resolves every function's parameter and return types.
8. Field defaults and then constants and globals are checked (`checkFieldDefault`, `checkConstDecl`, `checkGlobalValue`). Constants are folded by `evalConst`.
9. `checkFunctionBody` checks every non-generic function body.
10. The queue of generic instance bodies is drained. Checking one body can add more instances, so the loop runs until the queue is empty.
11. `main` must exist (except for `iris test` and the comptime helper), take no parameters and return `void`, `i32` or `!void`.

Inside a body, `checkStmt` handles statements and `checkExpr` handles expressions. `checkExpr(c, &slot, expected)` takes a pointer to the slot that holds the expression, because it may replace the node, for example with an `N_CONVERT`. `expected` carries the type the context wants, which literals such as `.red`, `[]` and `{...}` need.

## Scopes and names

A `Scope` is an array of `Sym { name, decl }` with a parent pointer. `lookup` walks up from the current scope. Local scopes are pushed for each block (`pushScope`). `declareLocal` refuses a name that hides another one in the same function. Each top-level declaration remembers its file's scope in `Node.scope`, and `enter(c, decl)` switches the checker to that scope, module and file before it looks at the declaration.

A resolved name points at its declaration through `Node.ref`. The IR writer only follows `ref` and never looks names up again.

## Types

`Type` in `src/iris.h` describes a resolved type. `kind` is a `TypeKind`:

- `TY_INT` (with `bits` and `isSigned`), `TY_FLOAT`, `TY_BOOL`, `TY_VOID`, `TY_NORETURN` (a block that always leaves) and `TY_NULL`.
- `TY_PTR`, `TY_SLICE` and `TY_ARRAY`, with `elem` and, for arrays, `count`. `str` is the slice of `u8`.
- `TY_STRUCT`, with `decl` (the `N_STRUCT`), `methods`, and after layout `size`, `align` and the LLVM `slots` with padding.
- `TY_STRING`, the built-in `String`.
- `TY_ENUM`, with `hasData`, `isFlags` and `payload`, the bytes of room for the largest value's data.
- `TY_RESULT` for `!T`, with `elem` set to `T`, and `TY_ERRVAL` and `TY_ERRCODE` for `Error` and `e.code`.
- `TY_TUPLE`, `TY_DYN`, `TY_FUNC`, `TY_CHAN`, `TY_ATOMIC`, `TY_MUTEX` and `TY_THREAD`.

Basic types are globals: `tyI64`, `tyStr`, `tyString`, `tyBool` and the rest. Compound types are made by `compound`, `funcOf` and `tupleOf`, which reuse an existing type when one matches. Two types are the same when `sameType` says so.

`coerce` is the one place where a value is fitted to a type. It narrows integer constants that fit, turns integer constants into floats, widens numbers without loss, turns a `String` into a `str` and an array into a slice, and inserts `N_CONVERT` nodes. Everything else is an error that names both types.

`layoutStruct` places fields in order with C's padding, honors `packed` and `align(n)`, packs runs of bitfields into one integer of their type, and records each field's `slot`, `offset`, `bitOff` and `bitWidth`. `typeSizeOf`, `typeAlignOf` and `fieldOffset` give the result to the IR writer.

## Calls

`checkCall` works out what a call is and records it in `Node.builtin`:

- `BI_NONE` is a call to an Iris function, with `ref` pointing at it.
- `BI_PRINT`, `BI_FORMAT`, `BI_ALLOC`, `BI_ERROR` and the others are built-ins the IR writer writes inline.
- `BI_DYN_CALL` is a call through a `dyn` value, with `ival` holding the method's place in the interface.
- `BI_INDIRECT` is a call through a function value.

`checkArgs` matches arguments with parameters: named arguments, defaults and a `T...` rest parameter. A method call on `str` or `String` looks the function up in the `str` module (`textMethod`).

Text with values passed straight to a `str` parameter gets `F_TEMPTEXT`, and a `String` returned by a call and passed as a `str` gets `F_TEMPSTR`. The IR writer frees both when the statement ends. `checkTempResult` rejects a call that gets such a temporary and returns a `str`, slice or pointer, because the result could point into the freed memory.

## Generics

A generic function or struct is never checked on its own. Each use with new types makes a copy:

1. `checkGenericCall` takes explicit type arguments or works them out from the arguments (`infer`, which matches a parameter's type with an argument's type).
2. `instantiateFunc` looks for an existing `Instance` with the same generic and the same types (`findInstance`). If there is none, it checks the bounds (`checkBounds`, which calls `conforms` for each `T: Interface`), copies the declaration with `cloneNode` from `src/generic.c`, and names the copy after its types: `max[Version]`.
3. `paramScope` makes a scope where each type parameter is an `N_TYPE_PARAM` whose `type` is the given type. The copy's signature is checked in that scope, and its body is queued with `queueBody`.
4. The copy goes into `Program.instances` (`instOut`) with `F_INSTANCE` and a number of its own in `Node.inst`, so `List[Record]` from two modules with different `Record`s get different IR names.

`instantiateStruct` does the same for `List[i64]`. `instantiateMethods` then copies each method of the generic struct for the instance, checks only its signature and marks it `F_LAZY`. The body is checked when the method is first called (`usedMethod`). Methods like `has` can use `==` without ruling out element types that have no `==`, and methods nobody calls cost nothing. A method with type parameters of its own is copied per call by `instantiateMethod`.

An error inside a copy is reported in the generic code and followed by a line at the use that made the copy: `max[Version], made here, has the errors above`. Instances nested more than 64 deep stop with an error, which catches generic code that calls itself with ever new types.

## comptime

`const X = comptime expr` runs Iris code while compiling. `frontEnd` in `src/main.c` handles it in two rounds:

1. `comptimeHelperSource` in `src/comptime.c` finds every comptime constant at the top level of the program's own files. It writes a file with the same imports and a function `irisComptimeMain` that prints each value as `\x01 N \x02 json.encode(expr)` on a line of its own.
2. `runComptime` loads the program again with that file added (`gExtraPath`, `gExtraText`) and sets `gComptime = 1`, so comptime constants are zero-valued stand-ins and code that uses them still checks. It builds the helper in debug mode for the machine the compiler runs on, runs it, and keeps each line's JSON keyed by the expression's place (`comptimeKey`, `file:line:col`). Other output is passed through with `comptime:` in front.
3. With `gComptime = 2`, `loadProgram` and `checkProgram` run again. `checkComptime` calls `comptimeLiteral`, which reads the JSON into a literal of the expression's type. Numbers, `bool`, text and plain enums become constants. Arrays and structs become read-only globals (`F_READONLY`), written as an LLVM `constant`.

Comptime code is ordinary Iris with the whole library. Nothing has to be interpreted inside the compiler.
