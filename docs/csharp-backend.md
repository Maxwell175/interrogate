# Interrogate and the C# Backend

This document explains two things:

1. how Interrogate works as a binding generator, and
2. the design decisions that were necessary to make the C# backend work well.

It is intentionally architectural.  It is not a user tutorial and it is not a
complete CLI reference.

## What Interrogate is

Interrogate is Panda3D's binding generator.  At a high level it does three
jobs:

1. parse C++ headers,
2. build an intermediate database that describes the published API, and
3. use that database to generate language bindings.

The important internal pieces are:

- `cppparser`: parses C++ and produces a C++ AST-like model.
- `interrogate`: walks that parsed model and builds the published interface.
- `interrogatedb`: stores that interface in memory and serializes it to `.in`
  database files.
- `InterfaceMaker*` backends: generate concrete outputs from the interface.

Historically the most important output was Python bindings, but the same core
pipeline can drive any FFI-oriented backend.

## How Interrogate sees C++

Interrogate does not expose arbitrary C++ just because it parsed it.  It only
builds binding data for declarations that survive its filtering rules.

The most important filters are:

- **published visibility**: Panda3D uses `PUBLISHED` and related macros to mark
  the API surface intended for bindings.
- **source ownership**: a target's interrogate run distinguishes between files
  that are local to the current source directory and files that merely arrived
  through includes.
- **backend legality**: even if a declaration is published, a particular backend
  may still reject it if it cannot be represented safely in that target
  language.

This distinction matters because a parsed C++ type can exist in memory without
being owned by the current target's output.

## The normal Interrogate pipeline

For a single target, the classic pipeline is:

1. parse headers and sources for that target,
2. create `InterrogateType`, `InterrogateFunction`, `InterrogateElement`, and
   related database entries,
3. generate native wrapper code if a backend needs it,
4. serialize the target's interface database to a `.in` file.

The main executable for that work is `interrogate`.

In Panda3D, this runs once per library-sized target such as `p3display`,
`p3gobj`, `p3chan`, and so on.

## Why C# was different

The original inline C# generation approach tried to emit the managed bindings
during each per-target `interrogate` run.

That worked poorly for a fundamental reason: C# needs a more global view of the
 type graph than the native wrapper pass does.

In particular, the C# API design depends on:

- interface-based representation of secondary C++ bases,
- transitive multiple-inheritance relationships,
- cross-library type references using a single managed namespace,
- consistent ownership information for native pointers,
- consistent wrapper names for P/Invoke,
- and enough metadata to emit managed code without reparsing C++ later.

Per-target inline generation meant the C# backend often ran before sibling
libraries had finished generating their `.in` files.  The result was unstable:

- missing secondary-base interface members,
- duplicate `NativeMethods` declarations,
- missing type emission in some runs,
- runtime entry-point mismatches,
- and native supplemental/destructor support drifting out of sync with managed
  output.

The solution was not a local patch.  The solution was to change the shape of the
pipeline.

## The C# architecture that finally worked

The C# backend now uses a real **two-pass design**.

### Pass 1: native wrapper production and database generation

Pass 1 is still driven by `interrogate` and still happens per target.

For each target it now produces:

- `*_igate_base.cxx`: the normal native wrapper code,
- `*.in`: the serialized interrogate database,
- `*_csharp_supplemental.cxx`: native support code required only by the C#
  backend,
- and finally a merged `*_igate.cxx` that combines the base wrapper output and
  the C# supplemental native stubs.

The supplemental native pass is intentionally parser-backed.  This matters
because some native support code still depends on source-context information,
especially destructor support and related helper stubs.

### Pass 2: managed code generation

Pass 2 is driven by the standalone `interrogate_csharp` executable.

It does **not** parse C++ again.  Instead it:

1. loads the complete set of `.in` files for the module,
2. builds a complete in-memory type graph from the serialized database, and
3. emits the managed `.cs` files.

In the Panda3D integration, this pass is used for managed output only.  The
tool also has an `--ocxx` path, but the production architecture described here
keeps parser-dependent native supplemental code in the parser-backed first pass.

This is what makes the C# generation deterministic.  The backend no longer has
to guess at cross-target relationships based on whichever sibling happened to be
done first.

## Why the second pass is database-driven

The whole point of the second pass is that C# generation should happen with a
complete, stable view of the API graph.

That only works if the `.in` database carries enough information to drive the
managed emitter.  Several pieces of metadata had to be added or surfaced to make
that practical.

The important additions were:

- abstract-type information,
- ref-counted return ownership information,
- explicit nullability information for returns and parameters,
- explicit-self metadata,
- forced-void-return metadata,
- and a C#-extension flag on wrappers.

Without those, the second pass would still have needed to depend on live C++ AST
objects, which would have defeated the whole design.

## Nullability in the C# API

One late design problem was nullability.

The first nullable-aware version of the backend used a very blunt rule: most
reference-like returns were emitted as nullable simply because they were returns.
That was technically safe in many places, but it created a bad developer
experience because everything downstream needed `!` to recover the more likely
non-null contract.

The final design is:

- **non-null by default** for the generated C# API,
- `[[in::nullable]]` on a C++ function declaration means the **return value** is
  nullable on the C# side,
- `[[in::nullable]]` on a C++ parameter means that **parameter** is nullable on
  the C# side.

The important implementation detail is that this is **wrapper metadata**, not
type metadata.  Nullability is a property of a particular use of a type, not of
the type globally.

That is why the data lives on:

- `FunctionRemap` during pass 1,
- `InterrogateFunctionWrapper` in the serialized database,
- and then the pass-2 C# emitter consumes those wrapper flags to decide whether a
  generated parameter or return should get `?`.

This keeps the second pass purely database-driven while still allowing precise
contracts.

### Why explicit nullability metadata matters

The default-non-null approach only works if the genuinely-nullable Panda3D APIs
are explicitly marked.  In practice, the important cases are:

- APIs that return `nullptr` / `NULL` on lookup failure,
- APIs that use `nassertr(..., nullptr)` or similar assertion-return-null paths,
- optional pointer parameters with defaults like `= nullptr`,
- and stream/file/open helpers that may legitimately fail and return no object.

The practical rule is simple:

- if the C++ contract allows null, add `[[in::nullable]]`,
- otherwise let the C# side stay non-null.

For non-null returns, the generated managed bindings now fail loudly if the
native side still returns null unexpectedly, instead of silently weakening the
API contract.

## Why interfaces are everywhere in the C# API

The user-facing rule for the C# backend is:

> public methods should return and accept interfaces for class-like types,
> not raw concrete wrappers.

That rule exists to make C++ multiple inheritance usable from C#.

### The C++ problem

C++ allows one class to inherit from multiple concrete bases.  C# does not.

If a Panda3D class has:

- one primary C++ base, and
- one or more secondary C++ bases,

then the natural managed shape is:

- the primary base becomes C# class inheritance,
- the secondary bases become C# interfaces,
- and the concrete generated class emits forwarding methods for those interface
  members.

### Why concrete return types are a bad fit

If public APIs returned concrete wrapper classes everywhere, the secondary-base
relationships would constantly leak through as explicit casts or helper methods.

Using interfaces instead means:

- natural assignment compatibility,
- better pattern matching,
- cleaner multiple-inheritance representation,
- and a public API that does not force the user to care about which wrapper type
  physically owns a native handle.

## Why automatic upcasts matter for multiple inheritance

Representing secondary C++ bases as C# interfaces is only correct if calls into
those inherited methods use the **right native subobject pointer**.

For example, if `TextNode` inherits `TextEncoder` as a secondary base, then a
`TextNode *` is not interchangeable with a `TextEncoder *` at the ABI level.
The pointer may need adjustment before calling a `TextEncoder` method.

An earlier version of the backend got this wrong: it generated inherited
secondary-base methods like `TextNode.SetText()` by passing `NativeHandle`
directly into the `TextEncoder_*` wrappers.  That compiled, but it was using the
wrong native `this` pointer and could crash at runtime.

The final design fixes this in the generator itself:

- the backend resolves the **declaring class** of the inherited method,
- searches the available interrogate upcast information for a chain from the
  concrete derived type to that declaring class,
- and emits the appropriate upcast helper calls before invoking the target
  native wrapper.

This is why inherited secondary-base methods now work directly from the public
generated API and no longer need handwritten helper shims.

## Why raw `IntPtr` is not part of the public API

The design goal was explicit:

> the end user should never have to deal with raw `IntPtr`.

That is why the backend uses the `NativeObject` / `INativeObject` model.

The managed layer hides raw native handles behind:

- `NativeObject` for concrete wrappers,
- `INativeObject` for interface-typed API boundaries,
- `NativeOwnership` to model borrow/own/refcount semantics,
- `NativeList<T>` and `NativeReadOnlyList<T>` for collection wrappers
  (extending `NativeObject`, implementing `IList<T>` / `IReadOnlyList<T>`),
- and factory methods such as `__CreateFromNative()` internally.

Users interact with managed wrappers and interfaces.  Native handles remain an
implementation detail.

## Why `CastTo<T>()` is available on interfaces

Once the public API returns interfaces pervasively, downcasting needs to work on
interface-typed values too.

Originally `CastTo<T>()` only existed as an instance method on `NativeObject`.
That forced callers into awkward code like:

```csharp
window = (output as NativeObject)?.CastTo<GraphicsWindow>();
```

That shape leaked implementation detail back into user code.  The final cleanup
was to expose `CastTo<T>()` on `INativeObject` via an extension method, so the
natural interface-typed version works directly:

```csharp
IGraphicsOutput output = engine.MakeOutput(...);
GraphicsWindow window = output.CastTo<GraphicsWindow>()
  ?? throw new InvalidOperationException("Output is not a GraphicsWindow.");
```

This keeps the API aligned with the “interfaces everywhere” model while still
using borrowed wrappers under the hood.

## Ownership, GC, and reference counting

The C# backend had to coexist with Panda3D's C++ ownership rules.

There are three distinct ownership cases:

- **Borrowed**: managed code must not destroy the object.
- **Owned**: managed code is responsible for destroying the object.
- **RefCounted**: the underlying C++ object uses reference counting semantics.

Those are represented by `NativeOwnership` in managed code.

The reason this matters is that GC and C++ lifetime are not the same system.
If the generated bindings do not preserve that distinction, one side will either
leak or free objects it does not own.

To make this work reliably, the backend had to:

- carry ownership metadata through wrapper generation,
- generate destructor-support stubs where direct wrapper destruction was not
  otherwise available,
- and ensure that the final native library actually exports the destructor
  entry points referenced by the managed bindings.

One of the most important late fixes in this work was making sure the native
`_inCSDestr_*` support stubs were generated, linked into `libpanda`, and then
confirmed at runtime with explicit disposal tests.

## Why there is C# supplemental native code at all

Most native wrappers are generated by the ordinary C backend.

The C# backend only needs extra native code for cases the normal C wrapper pass
does not already cover, such as:

- destructor support for ownership-sensitive managed disposal,
- collection helper stubs for native-backed list types (see the next section),
- and other C#-specific native helpers that should not be mixed into the public
  managed emitter.

These are emitted in the supplemental native pass and merged into the final
target `_igate.cxx`.

That keeps one native wrapper translation unit per target while still allowing
the managed backend to add the small amount of native support it actually needs.

## Native collections

C++ APIs frequently pass collections such as `std::vector<T>`, `pvector<T>`,
and `PointerToArray<T>`.  These need a clean, honest, and performant managed
representation.

### Design principles

1. **Concrete types in public signatures.**
   Parameters and return types use the generated concrete class (e.g.
   `PointerToArray_float`, `vector_string`) rather than a generic interface
   like `IList<T>`.  This is intentional: a regular `List<float>` cannot be
   passed to a native function — only the native-backed wrapper owns the C++
   memory.  Using the concrete type makes the API honest about what it accepts,
   avoiding runtime crashes from implicit misuse.

2. **Standard interfaces via inheritance.**
   Mutable collections extend `NativeList<T>` (which implements `IList<T>`).
   Immutable collections extend `NativeReadOnlyList<T>` (which implements
   `IReadOnlyList<T>`).  Users get `foreach`, LINQ, indexing, `Count`, and all
   other standard collection functionality for free.

3. **Zero-copy bulk access for blittable types.**
   For element types with a fixed binary layout (byte, int, float, double,
   etc.), the generated classes expose `AsSpan()` and `AsReadOnlySpan()` that
   return `Span<T>` / `ReadOnlySpan<T>` directly over native memory.  No data
   is copied.

4. **Element-by-element marshaling only where necessary.**
   String and object element types still marshal one element at a time through
   P/Invoke.  This is unavoidable because each element requires managed↔native
   conversion.

### What gets generated

For each C++ collection type recognized by the backend, the generator produces:

- **A sealed C# class** extending `NativeList<T>` or `NativeReadOnlyList<T>`.
- **Collection helper C functions** (emitted in the supplemental native pass):
  - `_empty_constructor` — creates an empty instance
  - `_size` — returns element count
  - `_get_element` / `_set_element` — per-element access
  - `_push_back` / `_clear` — mutation (mutable types only)
  - `_resize` — bulk resize (blittable mutable types only)
  - `_get_data_ptr` — returns raw `Element*` pointer (blittable types only)
  - `_get_data_size_bytes` — returns `size() * sizeof(Element)` (blittable only)
- **P/Invoke declarations** for all of the above in the `NativeMethods` class.

### The managed API surface for a typical collection

For a blittable mutable type like `PointerToArray_float`:

```csharp
// Construction
var arr = new PointerToArray_float();                // empty
var arr = new PointerToArray_float(someEnumerable);  // from any IEnumerable<float>
var arr = new PointerToArray_float(someSpan);        // bulk init from ReadOnlySpan<float>

// Standard IList<float> usage
arr.Add(1.0f);
arr[0] = 2.0f;
float v = arr[0];
int n = arr.Count;
foreach (float f in arr) { ... }

// Zero-copy bulk access
Span<float> span = arr.AsSpan();                     // read-write over native memory
ReadOnlySpan<float> ro = arr.AsReadOnlySpan();       // read-only view
arr.CopyFrom(stackalloc float[] { 1, 2, 3 });       // bulk overwrite

// Conversion to managed collections (bulk memcpy, not per-element)
float[] managed = arr.ToArray();
List<float> list = arr.ToList();
```

For a read-only type like `ConstPointerToArray_float`, the mutable members
(`AsSpan`, `CopyFrom`, `Add`, `Clear`, etc.) are absent.

For non-blittable types like `vector_string`, the span-based methods are absent
and `ToArray()` / `ToList()` use per-element marshaling.

### Why IList\<T\> is wrong for parameters

An earlier version used `IList<T>` for parameters and `IReadOnlyList<T>` for
returns.  This was misleading because:

- Passing `new List<float>()` compiled but crashed at runtime.
- The error message had to explain that the caller needed a native-backed
  collection, which is exactly the kind of surprise a type system should
  prevent.

Using the concrete collection type makes the requirement visible at compile
time.  If a user needs to pass data from a managed array, they construct the
native collection explicitly:

```csharp
float[] data = { 1, 2, 3 };
node.SetVertices(new PointerToArray_float(data));
```

This is one extra line of code in exchange for a completely unambiguous API
contract.

### How collection types are identified

The backend identifies collection types through the interrogate database, not
through hardcoded type names.  A type qualifies as a collection facade if it
exposes specific method patterns (`size`, `operator[]`, `push_back`) that the
database records.  The `CollectionFacadeKind` classification determines whether
the type maps to a mutable or read-only base class.

This means the system works for any C++ project that exposes vector-like types
through interrogate, not just Panda3D's specific container typedefs.

## Wrapper naming and why it mattered

The managed code ultimately calls native entry points through `LibraryImport`.

That only works if the C# side and the native side agree exactly on exported
symbol names.

One subtle bug during this work was that the second pass initially reconstructed
wrapper entry points from the wrong persisted wrapper identity.  The fix was to
use the wrapper's exported name, not a re-synthesized name based on a different
field.

This seems small, but it is the kind of issue that turns into
`EntryPointNotFoundException` at runtime even when the rest of the architecture
is correct.

## Why warnings exploded, and why they were mostly generator bugs

At one point the managed build was producing thousands of warnings.

Most of those were not “normal noise.”  They came from bad generator policy:

- re-emitting inherited members that should simply have been inherited,
- re-emitting inherited PascalCase aliases on derived types,
- using `new` too aggressively,
- not distinguishing between true overrides and accidental hiding,
- and emitting nullable annotations without enabling nullable context.

The final policy that worked was:

- **inherit** when possible,
- **override** when the base virtual member is truly being overridden,
- **suppress duplicate re-emission** when the base already provides the same
  managed surface,
- and use **`new` only where C# semantics actually require hiding**, mainly for
  certain static members.

That reduced the managed build to zero warnings and zero errors without changing
the public API shape in ad hoc ways.

## Why static methods are different

One recurring source of confusion was whether warnings should be fixed by making
methods `virtual`.

For **instance methods**, `override` is often the right answer if the base member
is actually virtual.

For **static methods**, `virtual` is not even legal C#.

That is why some generated static methods — most notably things like
`GetClassType()` and the typed static `__CreateFromNative()` factories — still
use `new` when they intentionally shadow a base static member.

## Why the final warning-free Panda3D build still required project-level suppression

Two warning classes in the Panda3D-managed project were not generator
correctness problems:

- `CS8981`: some imported C++ names are intentionally lowercase and would become
  reserved-looking identifiers in C#.
- `CA2255`: `NativeResolver` intentionally uses `[ModuleInitializer]` to install
  the native library resolver early.

These were handled cleanly and narrowly:

- `CS8981` was suppressed at the Panda3D C# project level,
- `CA2255` was suppressed only around the intentional module initializer.

That left the final managed rebuild at zero warnings and zero errors.

## Build artifacts and executables involved

The final Panda3D-integrated system uses three important executables/outputs:

- `interrogate`: parser-backed native wrapper and database generation,
- `interrogate_csharp`: database-driven managed code generation,
- `combine_text_files.py`: a small Panda3D build helper that merges per-target
  native base and supplemental output into the final `_igate.cxx` file compiled
  into Panda's native library.

The important generated artifacts per target are:

- `target_igate_base.cxx`
- `target_csharp_supplemental.cxx`
- `target_igate.cxx`
- `target.in`

At the module level, the managed second pass produces:

- `*.cs` generated binding files
- a `*.csharp.stamp` marker file used by the build graph

## Summary of the important C# design decisions

The design decisions that turned out to matter most were:

1. **Do native and managed generation in separate passes.**
   The managed pass needs a complete database view.

2. **Keep the second pass database-driven.**
   If the `.in` files do not carry enough metadata, fix the database model
   rather than sneaking AST dependencies back in.

3. **Use interfaces for class-like types, concrete types for collections.**
   Interfaces are the only clean way to model C++ multiple inheritance in C#,
   so regular classes return and accept interface types.  Collection types are
   an exception: they use concrete classes in signatures because only
   native-backed collections can be passed to native code, and pretending
   otherwise with `IList<T>` causes runtime crashes.

4. **Do not expose raw `IntPtr` publicly.**
   Keep native handles internal and model lifetime explicitly.

5. **Treat ownership as first-class metadata.**
   Borrowed, owned, and refcounted are different and the bindings must preserve
   that distinction.

6. **Make native supplemental support explicit.**
   Do not assume the ordinary native wrapper pass already covers every managed
   lifetime helper.

7. **Prefer inheritance or override to hiding.**
    `new` is a narrow escape hatch, not a default strategy.

8. **Use explicit nullability metadata instead of broad heuristics.**
   Default non-null, then mark real nullable contracts precisely.

9. **Use project-level suppression only for truly non-actionable warning classes
   in the consuming managed project.**
   Fix generator logic first.

10. **Use concrete collection types and provide zero-copy bulk access.**
    Native-backed collections use concrete types in public signatures to prevent
    misuse.  For blittable element types, expose `Span<T>`-based accessors so
    large data transfers avoid per-element P/Invoke overhead.

11. **Identify collection types from the database, not from hardcoded names.**
    The backend must not contain project-specific type name checks.  Collection
    recognition works through method-pattern analysis in the interrogate
    database so that it generalizes to any C++ project.

## What to keep in mind when changing this backend

If you change the C# backend, verify all of the following:

- the managed project rebuilds cleanly,
- when integrated into Panda3D, the native Panda library exports the
  supplemental destructor stubs the managed bindings reference,
- when integrated into Panda3D, the sample application still starts and runs
  against the final built native library,
- the two-pass build graph still guarantees that pass 2 only runs after the
  full `.in` set exists,
- collection types use concrete class names in signatures, not generic
  interfaces like `IList<T>`,
- and the generator contains no hardcoded project-specific type names or
  namespace fixups.

If any of those regress, the architecture is no longer doing its job.
