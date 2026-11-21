#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "value.h"

namespace ww {

class Program;

// Instruction set of the WinterWolf VM. Operands are little-endian: u8, u16, or i32 (absolute
// jump targets within the function).
enum class Op : uint16_t {
    Const,         // u16 constant                  -> value
    Null,
    True,
    False,
    Pop,
    Dup,
    Dup2,

    GetLocal,      // u16 slot
    SetLocal,      // u16 slot                      (value stays on the stack)
    GetGlobal,     // u16 global
    SetGlobal,     // u16 global
    DefineGlobal,  // u16 global                    value ->
    GetUpvalue,    // u16 index
    SetUpvalue,    // u16 index

    GetProp,       // u16 name, u16 cache           obj -> value
    SetProp,       // u16 name, u16 cache           obj value -> value
    GetIndex,      //                               obj index -> value
    SetIndex,      //                               obj index value -> value
    GetSuper,      // u16 name                      this superclass -> bound method
    SetSuper,      // u16 name                      this superclass value -> what the setter gave: super.name = value

    Add, Sub, Mul, Div, Mod, Neg, Not,
    Eq, Lt, Le, Gt, Ge,
    Is,            //                               value class -> bool

    Jump,          // i32 target
    Loop,          // i32 target                    backward jump; a garbage-collection safe point
    JumpIfFalse,   // i32 target                    pops the condition
    And,           // i32 target                    jumps keeping a falsy value, else pops
    Or,            // i32 target                    jumps keeping a truthy value, else pops

    Call,          // u8 argc                       callee args... -> result
    Invoke,        // u16 name, u8 argc             receiver args... -> result
    SuperInvoke,   // u16 name, u8 argc             this args... superclass -> result
    Closure,       // u16 nested function
    CloseUpvalue,  //                               pops a local that a closure captured
    Return,

    Class,         // u16 name                      -> class
    Inherit,       // u8 count                      supertypes... subclass -> superclass or null
    Method,        // u16 name                      class closure -> class
    StaticMethod,  // u16 name                      class closure -> class

    BuildList,     // u16 count
    BuildMap,      // u16 entry count               key value ... -> map
    BuildString,   // u16 part count
    Range,         // u8 flags                      start end [step] -> range  (1 inclusive, 2 counts down, 4 has a step)

    IterInit,      //                               iterable -> iterator
    IterNext,      // u16 slot, i32 target          pushes the next item, or jumps when exhausted

    TryBegin,      // i32 handler address
    TryEnd,
    Throw,
    Import,        // u16 module                    -> null (runs the module on first use)

    // Declared variable types. The u8 is a TypeId, optionally with kTypeNullable; for
    // kTypeInstance the class is on the stack above the value.
    CheckType,          // u16 variable name, u8 type    value [class] -> value  (throws on a mismatch)
    DefineTypedGlobal,  // u16 global, u8 type           value [class] ->
    CheckParam,         // u16 parameter name, u8 type   value [class] -> value  (throws on a mismatch)
    CheckReturn,        // u8 type                       value [class] -> value  (throws on a mismatch)

    // Declared class fields. DeclareField's second operand is the type in its low byte plus
    // kFieldVal / kFieldRequired; a class-typed field passes a closure that returns the class.
    DeclareField,       // u16 name, u16 flags           class [type closure] -> class
    FieldInit,          //                               class closure -> class  (runs on each new instance)
    ReturnInit,         //                               Return from an initializer: checks required fields
    SuperInit,          // u8 argc                       this args... superclass -> value  (runs its init, if any)
    DefaultArg,         // u16 slot, i32 target             jumps if the argument in `slot` was passed
    CallNamed,          // u8 positional, u8 named         callee args... (name value)... -> result
    Getter,             // u16 name                        class closure -> class  (get name())
    Setter,             // u16 name                        class closure -> class  (set name(value))

    // A local variable that a function inside its own initial value refers to: its slot is
    // set aside first, marked "no value yet", and such a function reads it with a check.
    Unset,              //                                 -> (the "no value yet" mark)
    GetUpvalueChecked,  // u16 index, u16 variable name    -> value  (throws if it has no value yet)

    Shl,                //                                 a b -> a << b
    Shr,                //                                 a b -> a >> b
    BitAnd,             //                                 a b -> a & b
    BitOr,              //                                 a b -> a | b
    BitXor,             //                                 a b -> a ^ b
    BitNot,             //                                 a -> ~a

    // A name in a lambda literal that is none of its variables: a member of the object the
    // lambda is run on, or of one that an enclosing lambda is run on, and failing that a global.
    GetImplicit,        // u16 name, u16 global            -> value
    SetImplicit,        // u16 name, u16 global            value -> value

    Identical,          //                                 a b -> bool  (a === b: the very same object)

    JumpIfNull,         // i32 target                      jumps if the top is null; it stays either way
    JumpIfNotNull,      // i32 target                      jumps if the top is not null; it stays either way
    AssertNotNull,      //                                 value -> value  (throws if it is null: `value!!`)

    // `break`, `continue` or a `return` that has finally blocks to run, in the middle of an
    // expression: drops the half-built values under the top one, leaving exactly `count`
    // slots below it (the function's variables, one of which may not have its value yet).
    Settle,             // u16 count                       ... value -> locals value

    // Tests against a type written in the source; the u8 is as for CheckType.
    IsType,             // u8 type                         value [class] -> bool
    Cast,               // u8 type                         value [class] -> value  (throws on a mismatch)
    CastOrNull,         // u8 type                         value [class] -> value or null
    Swap,               //                                 a b -> b a

    // The end of a block that is an expression: drops the block's `count` variables from
    // under its value, closing any that a function captured.
    PopUnder,           // u16 count                       locals... value -> value

    // A call with `*list` among its arguments. The positional arguments are gathered into
    // one list first, with Spread adding the elements of each list in turn.
    Spread,             //                                 list value -> list  (value must be a list)
    CallSpread,         // u8 named                        callee list (name value)... -> result

    // Static members. StaticField's operands are as for DeclareField.
    StaticField,        // u16 name, u16 flags             class [type closure] value -> class
    StaticGetter,       // u16 name                        class closure -> class  (static get name())
    StaticSetter,       // u16 name                        class closure -> class  (static set name(value))

    // More for names that may be members of an object the code runs on (see GetImplicit).
    // TryImplicit pushes the member and jumps if one of those objects has the name, and
    // otherwise does nothing. InvokeImplicit calls the name as a method of the object that
    // has it, or failing that as a global function; the slot under the arguments is spare.
    TryImplicit,        // u16 name, i32 target            -> [value]
    InvokeImplicit,     // u16 name, u16 global, u8 argc   spare args... -> result

    // Enums. EnumEntry names an instance as the entry with that ordinal; EnumSeal ends the list.
    EnumEntry,          // u16 name, u16 ordinal           class instance -> class
    EnumSeal,           //                                 class -> class
    EnumNext,           // u16 name, u16 ordinal           class -> class  (the entry made next has these already in its init)
    PendingEnum,        // u16 name                        -> the name, to be settled by the parameter it is passed to

    // Interfaces. Require names a member the implementing class has to supply; FinishClass,
    // at the end of every class, checks that it supplies all that its interfaces require.
    MarkInterface,      //                                 class -> class
    Require,            // u16 name                        class -> class
    FinishClass,        //                                 class -> class

    // Data classes and taking values apart. DataField adds a header field to the parts of a
    // data class; Component gives part `index` of a list or of a data object.
    DataField,          // u16 name                        class -> class  ("": a data class with no header fields)
    UnstableKey,        //                                 class -> class: its objects can change in what == sees (D26)
    Lazy,               //                                 closure -> a lazy value, worked out when first read (D37)
    Extend,             //                                 class interface -> class: fitted to the interface (D33)
    Bridge,             // u16 name                        class getter -> class: a getter an extend gives it (D33)
    BridgeMethod,       // u16 name                        class method -> class: a method an extend gives it (D186)
    Component,          // u8 index                        value -> part

    MissingArg,         // u16 parameter name              throws: the call left out an argument that has no default
    // A function calling itself as its last step (D29): the arguments become its parameters
    // and it starts again in the same frame, which counts the times.
    TailLoop,           // u8 count                        arguments ->
    // Before a bare name's lookup (GetImplicit and the rest): the object in a local to try first,
    // that of a block copied in place (D58, D59); flags 1: that one and no other (an inline
    // function's body, which runs on no object of the frame's).
    ImplicitFirst,      // u16 slot, u8 flags              ->
    // `block(obj, a)` or `obj.block(a)`: a function run on an object (D58), the object where a
    // method has `this`.
    CallOn,             // u8 argc                         callee object args -> result

    // A class's static values are worked out by a function of its own, attached with
    // ClassInit and run on first need: by EnsureClassInit, or by any use of a static member.
    ClassInit,          //                                 class closure -> class
    EnsureClassInit,    //                                 class -> class

    // An extension function: a method added to a type from outside. The u8 is a TypeId, with
    // kTypeNullable when null takes it too; kTypeInstance has the class under the function,
    // and 0 stands for every value.
    Extension,          // u16 name, u8 type               [class] closure ->
    MarkAbstract,       //                                 class -> class
    MarkValue,          // u8 flags                        class -> class  (a value type, M6.6 part 2): bit 0 made from its header alone; bits 1-7 its phantom type parameters (G4)
    // A member written `override` has to have something to take the place of.
    CheckOverride,      // u16 name, u8 flags              class -> class  (1: written override; 2: a field or property)
    // Invoke, where the last argument is a lambda written after the call: it goes to the last
    // parameter, and the parameters between the other arguments and it take their defaults.
    InvokeTrailing,     // u16 name, u8 argc               receiver args... lambda -> result
    SuperInvokeTrailing,  // u16 name, u8 argc             this args... lambda superclass -> result

    // Who may use a member. ClassNumber gives a class its number and its library's, which
    // the code written in it carries (FunctionProto::classIds). MemberAccess says what the
    // class declares of a member: the kind is 1 private, 2 protected or 3 internal, for the
    // code with that number (the class's, or for internal its library's); 0 open, whatever it
    // was above; 4 as it was above, where what was private above is open here. With 0x10 it
    // speaks of assigning only (`private set`). StaticAccess is the same for a static member.
    ClassNumber,        // u16 class number, u16 library   class -> class
    MemberAccess,       // u16 name, u16 number, u8 kind   class -> class
    StaticAccess,       // u16 name, u16 number, u8 kind   class -> class

    // GetProp, SetProp, Invoke and InvokeTrailing for a member the class declares itself,
    // reached through `this` by the class's own code (or a static member of the class, by
    // its name): that code may always use it, so who may is not looked at.
    GetOwn,             // u16 name, u16 cache             obj -> value
    SetOwn,             // u16 name, u16 cache             obj value -> value
    InvokeOwn,          // u16 name, u8 argc               receiver args... -> result
    InvokeOwnTrailing,  // u16 name, u8 argc               receiver args... lambda -> result

    // A name with several forms (overloads, D56): MakeForms gathers them into one value whose
    // proto is the nested function given, a set of forms (FunctionProto::chooser), and which
    // calls the form a call's arguments fit.
    MakeForms,          // u16 nested function, u8 count   forms... -> the set of them
    // The same for a method of a class: the names of its forms, each the method's name with the
    // signature of the form it is, are on the stack; the set goes under the method's name.
    MethodForms,        // u16 nested function, u8 count   class names... -> class
    StaticForms,        // u16 nested function, u8 count   class names... -> class  (the same for static methods)

    // `map[key, default = start]`: the entry, or `start` where the key is not there (L118).
    GetIndexOr,         //                                 map key start -> value

    // Two instructions in one (M6.5 part 4), the second's operand being a local or a constant:
    // `GetLocal x; Mul` is MulLocal x, `Const k; Add` is AddConst k. Two floats or two ints
    // are worked out at once; anything else goes the way the plain instruction does.
    AddLocal, SubLocal, MulLocal, DivLocal, ModLocal, LtLocal, LeLocal, GtLocal, GeLocal, EqLocal,  // u16 slot      a -> a op local
    AddConst, SubConst, MulConst, DivConst, ModConst, LtConst, LeConst, GtConst, GeConst, EqConst,  // u16 constant  a -> a op constant
    StoreLocal,         // u16 slot                        value ->  (SetLocal; Pop)
    CheckLocal,         // u16 slot, u16 name, u8 type     ->  (a parameter's declared type, not a class)

    // A `for` over a range written in place, `for i in 0..<n`, counted without a range or an
    // iterator being made (M6.5 part 4). RangeLoop takes the bounds (and the step, by Range's
    // flags) and leaves three locals: the next value, how many are left and the step.
    // RangeNext pushes the next value, as IterNext does, or jumps when none are left.
    RangeLoop,          // u8 flags                        start end [step] -> next left step
    RangeNext,          // u16 slot, i32 target            pushes the next value, or jumps when there are no more

    // Three instructions in one: `GetLocal a; GetLocal b; Mul` is MulLocals a b, and so on;
    // and `GetLocal a; SetLocal b; Pop` is CopyLocal a b.
    AddLocals, SubLocals, MulLocals, DivLocals, LtLocals, LeLocals, GtLocals, GeLocals, EqLocals,  // u16 slot, u16 slot  -> a op b
    CopyLocal,          // u16 from, u16 to                ->

    // An anonymous object (`object : Shape { ... }`), its class made once (M6.5 part 4).
    // ObjectClass pushes the class kept in the global and jumps over the code that makes it,
    // which StoreClass ends by keeping it there (0xFFFF: never kept, made every time). A class
    // made with the type parameters of the code around it is kept by ObjectClassTyped and
    // StoreClassTyped instead, one for each set of type arguments the frame has (G66). Then
    // the variables of the code around that its members use are pushed, each by Capture
    // (kind 0 a local, 1 an upvalue, 2 a capture of the object this code runs on, 3 the
    // superclass of the class `index` slots down), and NewObject makes the object with them.
    // Its members reach them with GetCaptured and SetCaptured, on the object they run on.
    ObjectClass,        // u16 global, i32 target          -> [class, at target]
    StoreClass,         // u16 global                      class -> class
    ObjectClassTyped,   // u16 global, i32 target          -> [class, at target]
    StoreClassTyped,    // u16 global                      class -> class
    Capture,            // u16 index, u8 kind              -> captured variable
    NewObject,          // u8 count                        class captured... -> object (runs its init)
    GetCaptured,        // u16 capture, u16 name           -> value  (throws if it has no value yet)
    SetCaptured,        // u16 capture                     value -> value

    // Types when the program runs (M6.6 part 1). MakeType makes a type from a shape, a string
    // constant read left to right (shapeOperands): `?` makes what follows nullable; a any, i
    // int, f float, n Number, s string, b bool, y Bytes, r Range, F a function, L a list of
    // what follows, M a map of the two that follow, t Type, C<digit> the class on the stack with that
    // many type arguments after it, P a type on the stack. The classes and types it names are
    // on the stack in the order they come in the shape. A type made of nothing that changes is
    // made once, where it is written: CachedType pushes it and jumps over the code that makes
    // it, which CacheType ends by keeping it. A<digit> makes the type arguments of a call, the
    // types that follow.
    //
    // A generic function is given its type arguments by WithTypes, right before the call: the
    // frame the call pushes keeps them, or the object it makes, for a generic class. TypeArg
    // reads one of the frame's, and InstanceTypeArg one of an object's.
    MakeType,           // u16 shape                       operands... -> type
    CachedType,         // u16 cache, i32 target           -> [type, at target]
    CacheType,          // u16 cache                       type -> type
    TagCollection,      // u16 cache, i32 target           list-or-map -> list-or-map  (given the type kept, then at target; else on, to make it)
    TypeCollection,     //                                 list-or-map type -> list-or-map  (what it was made to hold)
    IsFull,             //                                 value type -> bool
    CastFull,           // u8 safe                         value type -> value  (throws on a mismatch, or with `safe` null)
    WithTypes,          //                                 type-arguments ->  (for the call that follows)
    TypeArg,            // u8 index                        -> type
    InstanceTypeArg,    // u8 index                        object class -> type  (of the class's own type parameters)
    SupertypeMaker,     //                                 class function -> class  (ObjClass::supertypeMakers)
    CheckResult,        // u16 name                        value type -> value  (WW_CHECK_NATIVE_RESULTS: throws on a mismatch)
    // `pts.add(Point(1, 2))` (M6.6 part 3): where the list keeps its items packed and the value
    // type is made from its header alone, the arguments are written into a new item at the
    // end, and on at target with true; otherwise on, the stack as it was, to the call and the add.
    AddNew,             // u8 argc, i32 target             list class args... -> [true, at target]

    // A place a value is changed through (M6.6 part 4, value-lists.html section 8): a Ref to the
    // value where it is. An item of a list of values is changed where it is; a variable or a
    // field that holds a box is first given a box of its own, which only it holds.
    PlaceLocal,         // u16 slot                        -> place
    PlaceUpvalue,       // u16 index                       -> place
    PlaceGlobal,        // u16 global                      -> place
    PlaceField,         // u16 name                        object -> place
    PlaceIndex,         //                                 list index -> place

    // Read where it is (M6.6 part 4, value-lists.html section 6), where the compiler has found
    // that nothing changes the list meanwhile: an item of a list of values is pushed as a Ref
    // to it; anything else as GetIndex and IterNext give it. A function given a Ref it may not
    // keep as it runs takes a copy as it starts (OwnLocal).
    GetIndexRef,        //                                 list index -> item
    IterNextRef,        // u16 slot, i32 target            -> [item]  (as IterNext)
    // A field read where it is (M6.6 part 5): as GetProp and GetOwn, but a value kept inside its
    // object is given as a Ref into it, where the checker has shown nothing changes it meanwhile.
    GetPropRef,         // u16 name, u16 cache             obj -> value
    GetOwnRef,          // u16 name, u16 cache             obj -> value
    // `comp.bounds = Rect(...)` (M6.6 part 5): the field as where the result goes, a Ref into the
    // object where it keeps a value of a value type inside it, and otherwise null.
    FieldDestination,   // u16 name                        obj -> place
    // Right before a call whose result may stay where the function found it (M6.6 part 5): a
    // value it gives back that is a Ref into a list or an object is not copied on return.
    WithRefResult,      //                                 ->

    // A number's conversion, or one of Math's functions, called where the checker knows it is
    // the standard library's (M6.6 part 6): worked out in place, with no method looked up or
    // C++ function called. `which` is a NumericOp; it takes one value, or two for min and max.
    Numeric,            // u8 which                        value [value] -> result

    // M6.6 part 8: `>>>`, which brings in zeros (D44), and the int sum, difference and product
    // that wrap around (`&+`, `&-`, `&*`, D27), where `+`, `-` and `*` stop with an error.
    Ushr,               //                                 a b -> a >>> b
    AddWrap,            //                                 a b -> a &+ b
    SubWrap,            //                                 a b -> a &- b
    MulWrap,            //                                 a b -> a &* b
    OwnLocal,           // u16 slot                        ->

    // Results built where they go (M6.6 part 4, value-lists.html section 7). Right before a call,
    // WithDestinationAt gives it a place for its result, the one `depth` slots under the call's
    // own (`pts[i] = f(...)`), and WithFrameDestination passes on the frame's own, for a call
    // whose result it returns. A function that gives a value of a value type
    // (FunctionProto::typeArgs has kTakesDestination) keeps it for such calls of its own; making
    // a value from its header alone writes its fields there and gives the place.
    WithDestinationAt,  // u8 depth                        ->
    WithFrameDestination, //                               ->
    FromEnd,            //                                 k -> ^k
    // `for ((a, b) in ...)`: as IterNext, pushing the item's two parts; a map's key and value
    // with no entry made for them (D64, D73).
    IterNextPair,       // u16 slot, i32 target            -> a b

    // Reflection (M6.6 part 8, group 6, D48). ClassOf makes what a `#` was given the type of a
    // class, and pushes the function it is written in, whose rights to members the Class keeps:
    // `how` 0 a type, 1 a value, of whose class, 2 a class itself. ClassMeta gives a class the
    // nested function that describes its members for reflection, made into a function when
    // first asked for: `which` 0 its fields, 1 its methods and constructors (ObjClass::meta).
    ClassOf,            // u8 how                          operand -> type function
    ClassMeta,          // u16 nested function, u8 which   class -> class
    // An `atomic var` field changed in one step, by any core of a parallel section (D92):
    // `kind` 0 +=, 1 -=, 2 = max, 3 = min, giving the new value, or 4 compareAndSet, which takes
    // what is expected before the new value and gives whether it set it.
    AtomicField,        // u16 name, u8 kind               object [expected] value -> result
    // A parameter declared as a union holding one enum (`output : Stream | FilePath`): a bare name
    // that arrived for it becomes that enum's entry; anything else is left as it is.
    SettleEnum,         //                                 value class -> value
    // `grid[x, y]` (D133): a class's op [] or op []= with `count` indexes.
    IndexN,             // u8 count                        object index... -> value
    SetIndexN,          // u8 count                        object index... value -> result
    DupN,               // u8 count                        a b ... -> a b ... a b ...
    // Bundles of named arguments (D143): a map of names to values. BUNDLE_VALUES gives, for the
    // `count` names on the stack, each name and the bundle's value of it, for CALL_NAMED;
    // MERGE_BUNDLE is a new bundle of both, the second's values winning.
    BundleValues,       // u8 count                        bundle name... -> name value ...
    MergeBundle,        //                                 bundle bundle -> bundle
    // The frame's own type arguments, whole, for the call that follows, as WithTypes would give
    // them (G3): `isA<T>(x)` inside `fun <T>`, with no type made or looked up.
    FrameTypes,         //                                 ->  (for the call that follows)
    // `pts.insert(i, Point(1, 2))` and `pts.add(i, Point(1, 2))` (G7): as AddNew, the new item made at
    // position i, an int from 0 to the list's size, and on at target with null, or with true for add
    // (the high bit of argc); otherwise on, the stack as it was, to the call and the insert or the
    // add, which say what is wrong.
    InsertNew,          // u8 argc|0x80, i32 target        list position class args... -> [null or true, at target]
    // Where a class comes from (Class.origin): set where a library's class, or the standard
    // library's, is made; the program's own have none, and give Program.name.
    ClassOrigin,        // u16 name                        class -> class

    // Arithmetic and comparisons whose operands the checker knows (M6.7 step 5, D178): two ints
    // (I) or two floats (F). In the stack code they run as Add and the rest do; the register code
    // has R instructions for them that test nothing of their operands.
    AddI, SubI, MulI, LtI, LeI, GtI, GeI, EqI, AddF, SubF, MulF, DivF, LtF, LeF, GtF, GeF, EqF,

    // Values of value types in a frame's places, not in boxes (M6.7 step 6, D149; ObjPlaces).
    // EnterPlaces, a function's first instruction, gives its frame `count` places; one with none
    // has it with 0, which the translator leaves out. SetPlace gives a variable that has a place
    // the value on top: copied into its place, the variable a Ref to it (a value too large for a
    // place, or a frame given none, is kept as SetLocal keeps it). WithPlace, right before a call,
    // empties a place for the call to make its result in, as WithDestinationAt gives one. KeepLocal:
    // a variable with no place of its own keeps a copy of a value in one of the frame's, which may
    // change or be reused.
    EnterPlaces,        // u8 count                        ->
    SetPlace,           // u16 slot, u8 place              value -> value
    WithPlace,          // u8 place                        ->
    KeepLocal,          // u16 slot                        ->
    KeepTop,            //                                 value -> value (a copy, where in a place)
    // A class whose fields' types depend on its type arguments (G4): the function that gives them.
    FieldTypesMaker,    //                                 class function -> class  (ObjClass::fieldTypesMaker)
    // The fields of a class that keep a value of a generic value type declared wider than its
    // phantom type parameters can be, beside which they keep its own (G4): their names, one to a line.
    TypeSlots,          // u16 names                       class -> class
    // A method put in place behind a check (2026-11-13): jumps to the call made as usual unless
    // the value is an object of exactly the class global `global` holds, whose method the body
    // put in place is (a subclass's may be another).
    JumpIfNotClass,     // u16 global, i32 target          value ->

    Count_,

    // ---- The code the VM runs (M6.7, registers.cpp) ----
    // Made by the translator from the stack code as a function is loaded: never written by the
    // compiler nor kept in a bytecode file, where the verifier takes them for unknown.
    MoveS,         // u8 to, u8 from                   slot `to` = slot `from`
    MoveK,         // u8 to, u8 constant               slot `to` = the constant
    SetTop,        // u8 depth                         the stack's top at that slot
    // An R instruction does what the stack code it was made from would have done, where its
    // operands are numbers or a field is where it was found before; anywhere else it goes to its
    // slow way, code after the function's own that puts its operands where the stack would have
    // had them, runs that stack code, and comes back.
    // u8 A, u8 B, u8 C, u8 T, i32 slow: slot A = B op C, B and C slots (SS), or C a constant (SK),
    // or B one (KS); the stack's top at slot T after it.
    RAddSS, RAddSK, RAddKS,
    RSubSS, RSubSK,
    RMulSS, RMulSK, RMulKS,
    RDivSS, RDivSK,
    RLtSS, RLtSK,
    RLeSS, RLeSK,
    RGtSS, RGtSK,
    RGeSS, RGeSK,
    REqSS, REqSK,
    // u8 B, u8 C, u8 T, i32 target, i32 slow: a comparison and the JumpIfFalse after it, jumping
    // to target where it does not hold; the top at T either way.
    RJumpLtSS, RJumpLtSK,
    RJumpLeSS, RJumpLeSK,
    RJumpGtSS, RJumpGtSK,
    RJumpGeSS, RJumpGeSK,
    RJumpEqSS, RJumpEqSK,
    // u8 A, u8 B, u8 T, u16 cache, i32 slow: slot A = the field of the object in slot B, where
    // GetProp's cache says it is.
    RGetProp,
    RModSS,             // as RAddSS, floats worked out at once
    RModSK,
    RSubKS,             // as RAddKS
    RDivKS,
    // u8 A, u8 B (obj), u8 C (value: a slot, or a constant for SK), u8 T, u8 S, u16 cache, i32 slow:
    // the field the cache says, of an object in slot B, given C; slot A = C.
    RSetPropSS,
    RSetPropSK,
    RGetGlobal,         // u8 A, u8 T, u8 S, u16 global, i32 slow: slot A = the global
    RGetUpvalue,        // u8 A, u8 T, u16 upvalue: slot A = the upvalue's value
    RGetIndexSS,        // u8 A, u8 B, u8 C, u8 T, u8 S, i32 slow: slot A = list B's item C
    RGetIndexSK,
    RNeg,               // u8 A, u8 B, u8 T, u8 S, i32 slow: slot A = -B
    RNot,               // the same: slot A = !B
    RJumpIfFalse,       // u8 B, u8 T, i32 target, i32 slow: jumps where B is false
    // As RAddSS and the rest, on two ints: a shift by a constant count, & and |.
    RShlSK,
    RShrSK,
    RBitAndSS,
    RBitAndSK,
    RBitOrSS,
    RBitOrSK,
    // A shift by a count in a slot, from 0 to 63 (any other the stack instruction's way).
    RShlSS,
    RShrSS,
    // As those (2026-11-14, a hash written in WinterWolf): ^ of two whole numbers, >>> by a count
    // from 0 to 63, and &+, &- and &* of two ints (which alone the stack instructions take).
    RBitXorSS,
    RBitXorSK,
    RUshrSK,
    RUshrSS,
    RAddWrapSS,
    RAddWrapSK,
    RSubWrapSS,
    RSubWrapSK,
    RMulWrapSS,
    RMulWrapSK,
    // On operands of the kinds the checker knows (M6.7 step 5): as RAddSS, RLtSS and RJumpLtSS,
    // testing nothing; an int's overflow alone goes the slow way.
    RAddISS, RAddISK, RAddIKS, RSubISS, RSubISK, RSubIKS,
    RMulISS, RMulISK, RMulIKS, RAddFSS, RAddFSK, RAddFKS,
    RSubFSS, RSubFSK, RSubFKS, RMulFSS, RMulFSK, RMulFKS,
    RDivFSS, RDivFSK, RDivFKS, RLtISS, RLtISK, RLeISS,
    RLeISK, RGtISS, RGtISK, RGeISS, RGeISK, REqISS,
    REqISK, RLtFSS, RLtFSK, RLeFSS, RLeFSK, RGtFSS,
    RGtFSK, RGeFSS, RGeFSK, REqFSS, REqFSK, RJumpLtISS,
    RJumpLtISK, RJumpLeISS, RJumpLeISK, RJumpGtISS, RJumpGtISK, RJumpGeISS,
    RJumpGeISK, RJumpEqISS, RJumpEqISK, RJumpLtFSS, RJumpLtFSK, RJumpLeFSS,
    RJumpLeFSK, RJumpGtFSS, RJumpGtFSK, RJumpGeFSS, RJumpGeFSK, RJumpEqFSS,
    RJumpEqFSK,
    // `node.sizes.pref.width` (G14): two or three fields read through values held in one another,
    // the last a plain one, as one instruction. Its cache keeps, for the class last seen, where
    // the last field is: read there at once, with nothing looked up on the way. u8 A, u8 B, u8 T,
    // u8 S, u16 cache, u8 hops, u16 name x3, i32 slow.
    RGetPath,
    // A method called on the receiver in slot R with up to four arguments, slots or constants, its
    // result to slot A (M9/M10): a method in C++ of a string, list, map, set or bytes called at
    // once, a method of an object's class entered at once; anything else the slow way, Invoke's
    // stack code. u8 A, u8 R, u8 T, u8 S, u8 argc, u8 flags (bit k-1: Xk a constant; 16:
    // InvokeOwn), u8 X1, X2, X3, X4, u16 name, u16 cache, i32 slow, i32 resume (in the slow way,
    // just after its Invoke: where a call returns to, or a task set aside goes on). The cache keeps
    // the C++ method last called here and the kind of value it was called on (nativeCacheEntry).
    RInvoke,
    // `f(x1, x2)`: the function in slot F called with up to four arguments, slots or constants,
    // its result to slot A (M9/M10). A closure that needs nothing done first is entered at once;
    // any other call goes the slow way, Call's stack code. u8 A, u8 F, u8 T, u8 S, u8 argc, u8
    // flags (bit k-1: Xk a constant), u8 X1, X2, X3, X4, i32 slow, i32 resume (as RInvoke's).
    RCall,
    // `items[i].x`: field x of item i of a list of a value type's items, read where the item is,
    // as GetIndexRef and the GetProp after it do; the field where GetProp's cache says. u8 A, u8 B
    // (the list), u8 C (the index: a slot, or a constant for SK), u8 T, u8 S, u16 cache, i32 slow.
    RGetIndexPropSS,
    RGetIndexPropSK,
    // A function's whole run code until it is first called (M9/M10): its run code is made then,
    // from its stack code, and run from its start. No operands.
    Translate,
    RunCount_,          // how many instructions there are, the run code's included
};
// The stack code's instructions are a byte each; the run code's, two (kOpBytes).
static_assert(static_cast<int>(Op::Count_) <= 256, "a stack code instruction is a byte");
constexpr size_t kOpCount = static_cast<size_t>(Op::RunCount_);

// The types a variable can be declared with (`var n : int`). Any other name is a class.
enum TypeId : uint8_t {
    kTypeInt = 1,
    kTypeFloat,
    kTypeNumber,  // int or float
    kTypeString,
    kTypeBool,
    kTypeList,
    kTypeMap,
    kTypeRange,
    kTypeFunction,
    kTypeBytes,
    kTypeType,      // a type, as the program runs (M6.6 part 1): `items.itemType`
    kTypeInstance,  // an instance of a particular class or of one of its subclasses
    kTypeChar,      // one character (D24)
    kTypeByte,      // 0 to 255 (D19)
    // Storage widths (D148): ints (a float for float32) that must fit where they are kept.
    kTypeInt16,
    kTypeInt32,
    kTypeUint16,
    kTypeUint32,
    kTypeFloat32,
    kTypeIndex,     // a position: an int from the start, or ^k from the end (D146)
    kTypeSet,
    // A list, map or set made by frozen(), which nobody may change (D26).
    kTypeFrozenList,
    kTypeFrozenMap,
    kTypeFrozenSet,
    kTypeValues,  // a map's values, a live view of them (G22)
    kTypeNull,    // null alone, the type null# stands for (D48, as Python's NoneType)
    kTypeCount_,
};

// For a frozen collection's type, the type it is a kind of (FrozenList, List); any other as it is.
inline uint8_t unfrozenTypeId(uint8_t id) {
    return id == kTypeFrozenList  ? static_cast<uint8_t>(kTypeList)
           : id == kTypeFrozenMap ? static_cast<uint8_t>(kTypeMap)
           : id == kTypeFrozenSet ? static_cast<uint8_t>(kTypeSet)
                                  : id;
}
// For a list's, map's or set's type, its frozen kind; 0 for any other.
inline uint8_t frozenTypeId(uint8_t id) {
    return id == kTypeList ? kTypeFrozenList : id == kTypeMap ? kTypeFrozenMap : id == kTypeSet ? kTypeFrozenSet : 0;
}

// A type written `ww.Range` (the built-in Range, where the program has its own class of that
// name): the built-in name, Range; any other name as it is.
std::string builtinSpelling(const std::string& name);

// Whether `id` names one of the types above other than a class's.
inline bool isBuiltinTypeId(uint8_t id) { return id >= kTypeInt && id < kTypeCount_ && id != kTypeInstance; }
// Whether a value of type `id` is an int kept at a narrower width: it must fit its range.
inline bool isIntStorage(uint8_t id) { return id >= kTypeInt16 && id <= kTypeUint32; }
// How a value of type `id` is kept narrow (D19, D24, D148), or Narrow::None.
inline Narrow narrowOf(uint8_t id) {
    switch (id) {
        case kTypeByte: return Narrow::Byte;
        case kTypeChar: return Narrow::Char;
        case kTypeInt16: return Narrow::Int16;
        case kTypeInt32: return Narrow::Int32;
        case kTypeUint16: return Narrow::Uint16;
        case kTypeUint32: return Narrow::Uint32;
        case kTypeFloat32: return Narrow::Float32;
        default: return Narrow::None;
    }
}

// The range of an int storage width.
inline void intStorageRange(uint8_t id, int64_t& low, int64_t& high) {
    switch (id) {
        case kTypeInt16: low = -32768; high = 32767; break;
        case kTypeInt32: low = -2147483648LL; high = 2147483647; break;
        case kTypeUint16: low = 0; high = 65535; break;
        default: low = 0; high = 4294967295LL; break;
    }
}

const uint8_t kTypeNullable = 0x80;  // `type?`: null is allowed as well
const uint8_t kTypeMask = 0x7F;
// Never the type of a value: the type arguments of a call, kept as one type (M6.6 part 1).
const uint8_t kTypeArguments = 0x7F;
// Never the type of a value either: in the pattern of a field's type, the type parameter `id -
// kTypeHole` of the class declaring it, the class's own type arguments put in its place (G4).
const uint8_t kTypeHole = 0x60;
const uint8_t kTypeHoles = 16;
// FunctionProto::typeArgs.
const uint8_t kTypeArgsOwn = 1;
const uint8_t kTypeArgsKept = 2;
// A function that gives a value of a value type, given where its result goes (M6.6 part 4).
const uint8_t kTakesDestination = 4;
// A function whose frame has places for its variables' values (M6.7 step 6): EnterPlaces gave it
// some, and CallFrame::placesEnd says where they end.
const uint8_t kHasPlaces = 8;

// What Op::Numeric works out (M6.6 part 6), each as the standard library's function does.
enum class NumericOp : uint8_t {
    ToInt,       // Number.toInt(), toLong()
    ToFloat,     // Number.toFloat(), toDouble()
    RoundToInt,  // Number.roundToInt(), roundToLong()
    IsNaN,
    IsInfinite,
    IsFinite,
    Sqrt,        // Math.sqrt
    Floor,       // Math.floor, ceil and round: ints
    Ceil,
    Round,
    AbsInt,      // Math.abs, of an int and of a float
    AbsFloat,
    MinInt,      // Math.min and max of two ints, and of two floats
    MaxInt,
    MinFloat,
    MaxFloat,
    ToByte,         // int.toByte(): an error outside 0 to 255
    ToByteWrapped,  // int.toByteWrapped(): the low 8 bits
    Count_
};
inline int numericArity(uint8_t which) {
    return which >= static_cast<uint8_t>(NumericOp::MinInt) && which <= static_cast<uint8_t>(NumericOp::MaxFloat) ? 2 : 1;
}

const int kFieldVal = 0x100;       // the field can be assigned only once
const int kFieldRequired = 0x200;  // the initializer has to assign it
const int kFieldOverride = 0x400;  // declared again, in place of the superclass's field of that name
const int kFieldNested = 0x800;    // a static: a class or an object declared inside the class (StaticField)

// Maps a built-in type name to its TypeId; returns 0 for any other name.
uint8_t builtinTypeId(const std::string& name);

// "int", "string?", ... For kTypeInstance pass the class name.
std::string typeText(uint8_t type, const std::string& className);

// Lines from this one up, in a function's line table, stand for calls put in place (FunctionProto::inlined).
const int kInlinedLine = 1 << 28;

struct UpvalueDesc {
    bool isLocal;    // captured from the enclosing function's locals (else from its upvalues)
    uint16_t index;
    // Taken from capture `index` of the anonymous object the enclosing function runs on, a
    // member of its class (M6.5 part 4; see Op::NewObject).
    bool fromCapture = false;
};

// In FunctionProto::outerReceivers: the entry is a capture of the anonymous object the
// function, a member of its class, runs on, not one of its upvalues.
constexpr uint16_t kCapturedReceiver = 0x8000;

// In a module's code, the instructions that make one of its top-level declarations (M6.5 part
// 5): run only to make that, and left out of a built program that never reaches it. A function
// or a class is reached by its global, `key` its slot; an extension function, which is found by
// its name, by any use of that name, `key` the constant that is its name.
struct DeclarationSpan {
    // Reflect and ReflectMethods: what describes a class's fields, or its methods, for
    // reflection (Op::ClassMeta), kept only where reflection reaches it (D50); `key` is the
    // global of the declaration the class is part of.
    enum Kind : uint8_t { Global, Extension, Reflect, ReflectMethods };
    uint32_t start;
    uint32_t end;
    uint16_t key;
    Kind kind = Global;
};

// What the file a function is written in has in reach (G31, D188): the keys of its folder,
// itself and the folders it imports (libraryKey in the compiler), and the same as bits by the
// numbers extensionLibrary gives them. An extension function is found for a call where its
// key is among the calling function's.
struct FileReach {
    std::vector<std::string> keys;
    std::vector<uint64_t> bits;
    bool has(uint16_t library) const {
        const size_t word = library >> 6;
        return word < bits.size() && ((bits[word] >> (library & 63)) & 1) != 0;
    }
};
// The reach of those keys, one for each set of keys, kept while the process runs.
const FileReach* makeReach(std::vector<std::string> keys);

// The number of a library key, the same for every VM of the process. The standard library's
// is kEverywhere: it is in reach of every file.
uint16_t extensionLibrary(const std::string& key);
constexpr uint16_t kEverywhere = 1;
// What an extension function's signature has before its library's key.
constexpr char kLibraryMark = '\x02';

// A compiled function: the unit the compiler emits and the bytecode file stores.
struct FunctionProto {
    std::string name;
    std::string source;
    int arity = 0;
    // The number of arguments a call may pass for its frame to be made at once (QUICK_CALL in
    // vm.cpp): its arity, where it is no set of forms, takes no vararg and keeps no type
    // arguments; else -1, every call going through VM::call. Set as the function is verified.
    int quickArity = -1;
    // And the fewest it may pass, the rest left to their defaults, which it fills in itself.
    int quickFewest = 0;

    // Fewest arguments a call may pass. Parameters beyond that have default values, which the
    // function fills in itself (see DefaultArg). -1 means "the same as arity".
    int minArity = -1;
    int requiredArgs() const { return minArity < 0 ? arity : minArity; }

    // The `vararg` parameter (counted from 0), which receives the arguments from its position
    // on as a list, or -1. Parameters after it can only be passed by name.
    int varargIndex = -1;

    // Whether its frame keeps type arguments (M6.6 part 1): kTypeArgsOwn for a generic
    // function, given its own by WithTypes; kTypeArgsKept for a lambda or local function that
    // reads those of the function it is written in, or makes one that does, which its closure
    // keeps. Only such a function's frame has them: the others are called without.
    uint8_t typeArgs = 0;
    // Of a generic function declared inside another generic function (G1): how many type
    // arguments of the functions around it come first in its frame's, its own after them.
    uint8_t typeArgBase = 0;

    // Bit i is set when parameter i is declared with a class type, the only kind of parameter
    // that can turn a bare name into an enum entry. (Parameters past the 64th never do.)
    uint64_t classParams = 0;

    // Parameter names (interned), for calls that pass arguments by name. Empty for functions
    // that cannot be called that way.
    std::vector<ObjString*> paramNames;

    // Stack slots the function needs, counted from its frame base. Filled in by verifyFunction.
    int maxStack = 0;
    // For a function that only reads a field of its receiver and returns it, as `get() => field`
    // does: the field's name, so that a getter of that kind is read as its field (M6.7). Filled
    // in by verifyFunction; null for any other.
    ObjString* readsField = nullptr;

    std::vector<uint8_t> code;
    std::vector<int> lines;                // source line of each byte in `code`

    // Calls of small functions the compiler put in place of the call (M9/M10 part zero), for a
    // stack trace to show as calls: a line of `lines` from kInlinedLine up stands for line
    // `line` of the function `name`, in `source`, called at `callLine`, which is such a line
    // again where that call was put in place too, of a call before it in this list.
    struct InlinedCall {
        std::string name, source;
        int line = 0;
        int callLine = 0;
    };
    std::vector<InlinedCall> inlined;
    // The line of the function's own source a line of `lines` or `runLines` is at: for code put in
    // place of a call, the call's.
    int sourceLine(int line) const {
        while (line >= kInlinedLine && static_cast<size_t>(line - kInlinedLine) < inlined.size())
            line = inlined[static_cast<size_t>(line - kInlinedLine)].callLine;
        return line;
    }

    // The code the VM runs (M6.7): made from `code` by translate (registers.cpp) when the
    // verifier has passed it; with the source line of each of its bytes, and the ranges of
    // `rights` in its offsets. Not part of the program as it is written out. Each instruction
    // is kThreaded bytes, then its opcode (kOpBytes) and its operands: built with Clang or GCC, those bytes
    // hold the address of its code in the VM's loop, written the first time the VM runs the
    // function (`addressed`), so that each instruction goes on to the next through one load
    // (D160); built with MSVC they are not used.
    std::vector<uint8_t> run;
    struct Addressed {
        std::atomic<bool> done{false};
        Addressed() = default;
        Addressed(Addressed&& other) noexcept : done(other.done.load()) {}  // a function read later is moved in
        Addressed& operator=(Addressed&& other) noexcept {
            done.store(other.done.load());
            return *this;
        }
    };
    mutable Addressed addressed;
    // Until the function is first called, the program it is translated in then (translateNow);
    // its run code is the one instruction Translate. Null once translated.
    const Program* untranslated = nullptr;
    std::vector<int> runLines;
    struct RunRights {
        uint32_t start, end;  // a range of `run`
        uint32_t which;       // the rights it has, in `rights`
    };
    std::vector<RunRights> runRights;
    // Native code (M9.5, src/native/regions.cpp): the VM's top where each instruction of `run`
    // starts, as the translator had it, kept for the regions to be made from; where each of its
    // loops starts (whose address in the code is VM::nativeFirst's until that loop's regions are
    // made, the second time it starts), and for each how far it has got (1 started once, 2 its
    // regions made); and the regions made, each way in where in `run` its instruction starts
    // (whose address in the code is then the VM's way into it) with its machine code.
    std::vector<std::pair<uint32_t, uint16_t>> runTops;
    // Where its regions' context is kept as they run (2026-11-19: a register held its address
    // before): in the frame, from this slot on, past its own, kNativeContextSlots of them (native::
    // RegionContext, laid there by the VM as a region is gone into, at an offset from the frame
    // the stencils have built in: ww_ctxat). Given where native code may be made of it (its loops
    // found), maxStack that much more, so that a call leaves room for it. 0: none.
    int nativeContextSlot = 0;
    // For each instruction of the run code, where it starts (runLiveAt, in order; 2026-11-19): the
    // slots the VM may read again from there before writing them, `runLiveWords` words of bits
    // each (slotsLive, registers.cpp), which a way out of native code writes back; empty where not
    // worked out (every slot may be read). Each stack code instruction's, where it starts, whether
    // the translator knew the VM's top there or not (runTops has only those it did). While
    // translating, each one's stack code instruction and the locals the stack the translator holds
    // there still names (runOrigins, then runLive).
    std::vector<uint32_t> runLiveAt;
    std::vector<uint32_t> runOrigins;
    std::vector<uint64_t> runLive;
    uint8_t runLiveWords = 0;
    // Whether slot `slot` may be read again from run code offset `at` (an instruction's start);
    // true where that is not known. A run code instruction no stack code instruction starts at is
    // the rest of the one before it that does (a value the stack held put in its slot, then the
    // instruction itself, 2026-11-19): what may be read there, it may.
    bool slotLiveAt(uint32_t at, int slot) const {
        if (runLiveWords == 0 || runLive.size() != runLiveAt.size() * runLiveWords || slot < 0 || slot >= 64 * runLiveWords) return true;
        auto it = std::upper_bound(runLiveAt.begin(), runLiveAt.end(), at);
        if (it == runLiveAt.begin()) return true;
        const uint32_t from = *(it - 1);
        it = std::lower_bound(runLiveAt.begin(), runLiveAt.end(), from);
        bool live = false;
        // Every one starting there (an instruction joined of several): live where any has it.
        for (; it != runLiveAt.end() && *it == from; ++it) {
            const uint64_t word = runLive[static_cast<size_t>(it - runLiveAt.begin()) * runLiveWords + static_cast<size_t>(slot) / 64];
            live = live || ((word >> (slot % 64)) & 1) != 0;
        }
        return live;
    }
    std::vector<uint32_t> nativeHeads;
    std::vector<uint8_t> nativeArrivals;
    // A whole function's region (2026-11-15): where it starts (just past its parameters'
    // prologue; UINT32_MAX for none), whose address in the code is VM::nativeCall's until the
    // function has been called often enough (nativeCalls counts); then each of its returns
    // (nativeReturns) is VM::nativeReturn's, which makes the region the next time one is come to,
    // the frame holding what each of its locals is.
    uint32_t nativeCallHead = UINT32_MAX;
    uint32_t nativeCalls = 0;
    // Whether its region has been made again, once, after it was dropped past its misses' limit
    // (2026-11-16, VM::nativeEnter): made at the next return, from the frame there.
    bool nativeCallsAgain = false;
    std::vector<uint32_t> nativeReturns;
    struct NativeRegion {
        uint32_t head;
        void* code;
        // How often the region's quick ways have not done what they do (an item of another
        // kind, a guess wrong), shared by its ways in: past a limit, the VM stops going in.
        uint32_t* misses;
        // The region's start, whose prologue looks at lists and fields once; any other way in
        // (after an island) reads none of what the context keeps for that (VM::nativeEnter),
        // unless it looks at them again itself (a loop that calls out and comes back, 2026-11-17).
        bool first = false;
        // Whether the region looks at lists or fields once (its prologue): where it does not,
        // nothing of the context's lists and held fields is read, and none is set to nothing as
        // it is entered (2026-11-15: some 600 bytes, each time a function's region is entered).
        bool looks = true;
        // A loop's region made first (registers-plan.html part B, 2026-11-17): how often each of its
        // instructions ran, by where it starts in `run`, and its times round (`runs[at.size()]`); shared
        // by its ways in. Null for one made again from them, or a whole function's.
        struct RegionCounts* counts = nullptr;
    };
    // The regions made so far as published: a list never changed once published, replaced whole
    // as a loop's regions are added (under VM::nativeFirst's lock), so that a core reading it as
    // another adds reads a whole one; every list kept for as long as the function is.
    struct RegionsNow {
        std::atomic<const std::vector<NativeRegion>*> list{nullptr};
        RegionsNow() = default;
        RegionsNow(RegionsNow&& other) noexcept : list(other.list.load()) {}  // a function read later is moved in
        RegionsNow& operator=(RegionsNow&& other) noexcept {
            list.store(other.list.load());
            return *this;
        }
        const std::vector<NativeRegion>* load(std::memory_order order) const { return list.load(order); }
        void store(const std::vector<NativeRegion>* to, std::memory_order order) { list.store(to, order); }
    };
    RegionsNow regionsNow;
    std::vector<std::unique_ptr<std::vector<NativeRegion>>> regionLists;
    std::vector<DeclarationSpan> spans;    // a module's: the code of each top-level declaration
    std::vector<Value> constants;          // ints, floats and interned strings
    std::vector<FunctionProto*> protos;    // nested functions
    std::vector<UpvalueDesc> upvalues;

    // For a lambda literal inside other lambda literals: the upvalues holding slot 0 of each
    // enclosing one, innermost first. That slot is the object the lambda was run on, if any.
    std::vector<uint16_t> outerReceivers;

    // The numbers of the classes the function is written inside of, innermost first, and
    // then of its library: a member that is private, protected or internal is open to the
    // function when one of these is among the numbers the member is marked with.
    std::vector<uint16_t> classIds;
    uint16_t ownClass = 0;  // the first of them, when the function is written in a class

    // A form of a name that has several (overloads, D56): what tells it from the others, the
    // types of its parameters as written, `(float, float)`; and those types for choosing a
    // form when the program runs, where the compiler could not: a TypeId with kTypeNullable
    // per parameter (0 untyped), and for kTypeInstance the class's name. Empty otherwise.
    std::string signature;
    std::vector<uint8_t> paramTypes;
    std::vector<ObjString*> paramClasses;
    // For a parameter of a function type, one more than the number of parameters that type
    // has, so that a lambda taking two tells `forEach { k, v -> }` from `forEach { entry -> }`;
    // 0 for any other.
    std::vector<uint8_t> paramCounts;

    // The set of a name's forms, made by MakeForms: it has no code of its own; calling it
    // calls the form the arguments fit (VM::chooseForm).
    bool chooser = false;
    // Read from a program ww itself made (its cache, a worker's image) and not checked yet:
    // checked by the verifier at its first call, before its code runs, its maxStack and caches
    // found then (translateNow; VM's Translate makes room for its stack).
    bool unverified = false;
    // Of an extension function, the number of its library (extensionLibrary), from the key at
    // the end of its signature; 0 for any other function.
    uint16_t library = 0;

    // Where each GetProp, SetProp, GetOwn and SetOwn remembers the field it last found (M6.5
    // part 4), by its second operand. Each holds the layout of the object's class
    // (ObjClass::shape) in its high 32 bits and the field's place in bits 16 to 31, and for a
    // SetProp or SetOwn the field's type in bits 8 to 15; 0 when nothing is there yet.
    // Made empty by verifyFunction; not part of the program as it is written out.
    std::unique_ptr<std::atomic<uint64_t>[]> caches;
    uint32_t cacheCount = 0;

    // What its file has in reach, which calls of extension functions in it go by; none for a
    // function no file is written in (the set of a name's forms), which reaches everything.
    const FileReach* reach = nullptr;
    // Sets `library` from the signature.
    void noteLibrary();

    // Code copied in from an inline function of another class or library (D59, G36): from
    // `start` to `end` it may use too what the numbers in `ids` may (a class's, then a
    // library's), as the inline function's own code may; the rest of the function may not.
    struct Rights {
        uint32_t start = 0, end = 0;
        std::vector<uint16_t> ids;
    };
    std::vector<Rights> rights;

    // What a built program keeps of its classes for reflection (D50, G42), read only when it
    // is built, which then drops them. A note at `at`, in order: of the Op::ClassOf there, the
    // classes the Class it gives may be, by the globals of the top-level declarations they are
    // part of (with the classes below them, unless kReflectExact), or every class
    // (kReflectAll); and what is asked of it: nothing, kReflectFields for anything (it may be
    // given on, and asked for its methods where those of any Class given on are), or with
    // kReflectMethods too its methods (`x#.methods`). Or kReflectNotMethods, of the
    // instruction there, a use of `methods`, `method` or `constructors` that asks nothing of a
    // Class's methods; any other such use asks for the methods of every Class given on.
    // And `supertypes`: what each class declared here is besides, its global (or kNoGlobal for
    // one declared inside a function), then those of the classes it extends and its interfaces,
    // as `extend` adds them too. Kept apart, and made only for a function that has some.
    struct ReflectNote {
        uint32_t at = 0;
        uint8_t flags = 0;
        std::vector<uint16_t> globals;
    };
    struct Reflection {
        std::vector<ReflectNote> notes;
        std::vector<std::vector<uint16_t>> supertypes;
    };
    std::unique_ptr<Reflection> reflection;
    Reflection& noted() {
        if (reflection == nullptr) reflection = std::make_unique<Reflection>();
        return *reflection;
    }

    // A class's description (Op::ClassMeta, compileMeta): a program's image keeps it apart, and
    // a program read from one reads it only when reflection first asks for it (G47). Until then
    // `deferred` holds its bytes, with what reads them, and of the rest only its typeArgs.
    bool describes = false;
    struct Deferred {
        std::string bytes;
        VM* vm;
        Program* program;
    };
    std::unique_ptr<Deferred> deferred;

    // Of a function read from a program ww made and kept (its cache, a worker's image): its
    // body, all after its header (code, constants, lines, nested functions), left as the bytes
    // it was written in until something first asks for it (loadBody): a program's start reads
    // many functions it never calls, and most of reading one is its body (2026-11-09).
    struct LaterBody {
        std::string bytes;
        VM* vm;
        Program* program;
        int nesting;
    };
    std::unique_ptr<LaterBody> laterBody;
};

// Reads a description kept apart into the function, and verifies it; nothing for one read.
// Safe from any thread: `reader`, the VM asking (a core's in a parallel section), makes its
// names, which a core's does in its program's table under that table's lock; null for the
// program's own.
void loadDeferred(FunctionProto& f, VM* reader = nullptr);
// Reads the body of a function read without it (FunctionProto::laterBody), and what it holds;
// nothing for one that has its body. Safe from any thread.
void loadBody(FunctionProto& f);

// What the verifier (analyzeStack) found of each instruction of a function's stack code, by
// where it starts, besides the stack's depth before it: how many values it takes off the stack
// and puts on, and where else than on to the next it goes (from, to). Kept for the slots a way
// out of native code writes back (slotsLive, registers.cpp, 2026-11-19).
// The slots a function's regions' context takes in its frame (FunctionProto::nativeContextSlot):
// native.cpp checks native::RegionContext fits.
constexpr int kNativeContextSlots = 200;

struct StackSteps {
    std::vector<uint16_t> pops, pushes;
    std::vector<std::pair<uint32_t, uint32_t>> jumps;
};

// Makes the code the VM runs from `f`'s code, given the stack's depth before each of its
// instructions (-1 where none is reached): registers.cpp (M6.7). With `steps`, what slots may be
// read again from each instruction is worked out too, for native code (FunctionProto::runLive).
void translate(FunctionProto& f, const std::vector<int>& depths, const StackSteps* steps = nullptr);
// The bytes before each opcode of the code the VM runs, which hold where its code is (D160).
// (Each instruction starting at a multiple of them, so that they never span two cache lines,
// was measured 1% to 4% slower: the code is larger, and each instruction rounds its end up.)
constexpr size_t kThreaded = 8;
// The bytes of a run code instruction's opcode, little-endian: the run code has more instructions
// than a byte numbers (the stack code's and its own R instructions), and threaded, its opcode is
// not what takes it to its code, so a byte more costs little (measured in M6.7).
constexpr size_t kOpBytes = 2;
inline Op opAt(const uint8_t* at) { return static_cast<Op>(at[0] | at[1] << 8); }
// The length of the instruction of run code whose opcode is at `at` (kThreaded bytes after its
// start), not counting those bytes, and its name.
size_t runSize(const std::vector<uint8_t>& run, size_t at, const char*& name);
// The bytes of a stack code instruction's operands.
size_t operandBytes(Op op);
// Where the VM's instructions are functions (vm.cpp, M6.7), what writes each one's address into
// run code as it is made; null where they are known only once the VM runs.
extern void (*writeRunAddresses)(FunctionProto& f);
// Native code (src/native/regions.cpp, M9.5), null where ww has none. As a function's run code
// is finished, before its addresses are written: where its loops start (FunctionProto::
// nativeHeads), true where it has any. The first time one of them starts (VM::nativeFirst):
// its regions.
extern bool (*findNativeLoops)(FunctionProto& f);
// What a region made first counted (FunctionProto::NativeRegion::counts): each instruction's place
// in `run`, how often each ran (one more: the times round), whether it has been asked for again.
struct RegionCounts {
    std::vector<uint32_t> at;
    std::unique_ptr<uint64_t[]> runs;  // and runs[at.size()]: the times round; runs[at.size() + 1]: 2 where loopbackc left for the counts, 1 once asked
    bool asked = false;
};

// What the running program held as a function's regions were asked for (2026-11-17,
// background-plan.html): taken on the core running it (takeNativeSeen), and all the making
// reads of what the program holds, so that it may run on another core while the program goes
// on and changes it. Its objects (`roots`) are kept from the collector for as long as it is.
struct NativeSeen {
    // A list as it was: ObjList's store(), packed(), size(), and its pack's.
    struct List {
        ListStore store = ListStore::Values;
        bool packed = false;
        bool nullable = false;
        Narrow narrow = Narrow::None;
        const ObjClass* itemClass = nullptr;
        const ObjTypeInfo* itemType = nullptr;
        uint32_t typeSlot = 0;
        uint32_t width = 0;
        size_t size = 0;
        Value first{};  // a list of values' first item, where it has one
    };
    // An object as it was: its bytes from its start to its slots' end, and the slot of each
    // field named in the function's constants (ObjInstance::fieldIndex: -1 for none).
    struct Instance {
        uint32_t shape = 0;
        uint16_t slotsStart = 0;
        std::vector<uint8_t> bytes;
        std::vector<std::pair<const ObjString*, int64_t>> slots;
    };
    // A variable's place as it was (ObjPlace's type and class).
    struct Place {
        const ObjTypeInfo* type = nullptr;
        const ObjClass* cls = nullptr;
    };
    std::vector<Value> frame;                // the call's live slots
    std::vector<const ObjClass*> frameSmall;  // for each, a Small value's class (smallClassOf)
    std::vector<Value> globals;
    std::unordered_map<const Obj*, List> lists;
    std::unordered_map<const Obj*, Value> iterated;  // an iterator's source
    std::unordered_map<const Obj*, Instance> instances;
    std::unordered_map<const Obj*, Place> places;
    // For each string constant of the function, whether that bare name is no member of the
    // object in the frame's slot 0 (VM::noMemberForRegion), where it holds an object or a value.
    std::vector<std::pair<const ObjString*, bool>> noMember;
    // Made again from what the region made first counted (registers-plan.html part B): how often
    // each instruction ran, by where it starts in `run`, over its times round; empty the first time.
    std::unordered_map<uint32_t, uint64_t> runs;
    uint64_t rounds = 0;
    std::vector<Value> roots;  // marked by the VM as its own while the making may read them
    // The places' caches (FunctionProto::caches) as they were, which the making reads in place of
    // the caches themselves (B84, toww's SoundTest, 2026-11-20): the VM fills them as it runs on
    // while a build is made in the background, and the making reads one more than once, in its
    // passes. A field read the analysis saw no cache for, as a value written to its slot (getpv),
    // and the lowering then saw kept as an int, in a register alone (getpi): the add that came
    // after read the slot, never written, and t.next += ... gave what an older frame left there.
    std::vector<uint64_t> caches;
};
// What the program holds, taken for `f`'s regions: `frame`, the call's `frameSlots` live slots;
// the VM's globals; `noMember` asks the VM (`vm`) whether a bare name is a member of an object.
using NoMemberFn = bool (*)(const void* vm, const FunctionProto& f, const ObjString* name, const Value& receiver);
extern std::unique_ptr<NativeSeen> (*takeNativeSeen)(const FunctionProto& f, const Value* frame, int frameSlots, const Value* globals, size_t globalCount,
                                                     NoMemberFn noMember, const void* vm);
// The regions of the loop starting at `head` in `run` are added to `made`, from what the
// program held as they were asked for (`seen`: the frame's slots say what its lists hold, a
// guess the regions check). Reads nothing the program may change meanwhile.
extern void (*makeNativeRegions)(FunctionProto& f, const NativeSeen& seen, uint32_t head, std::vector<FunctionProto::NativeRegion>& made);
// The builds of a program's functions dropped as it ends (vm.cpp's builder; null where none).
extern void (*dropNativeBuildsOf)(const std::vector<std::unique_ptr<FunctionProto>>& owned);

constexpr uint8_t kReflectFields = 1, kReflectMethods = 2, kReflectExact = 4, kReflectAll = 8, kReflectNotMethods = 16;
// A note of the Op::ClassMeta there, of a class no global holds (one declared inside a function,
// an anonymous object's, G43): `globals` are the classes it is below; a built program that never
// asks for its description empties it.
constexpr uint8_t kReflectDescribes = 32;
constexpr uint16_t kNoGlobal = 0xFFFF;


// A whole compiled program, ready to run or to write to a bytecode file.
class Program {
public:
    Program() = default;
    Program(Program&&) = default;
    Program& operator=(Program&&) = default;
    // Native code still to be made of its functions (the builder's, vm.cpp) dropped first.
    ~Program();
    std::vector<std::string> globalNames;

    // Top-level function of each source file. Module 0 is always the prelude.
    std::vector<FunctionProto*> modules;
    int entry = 0;

    FunctionProto* newProto();

    // For Compiler.compile in a program read from a bytecode file or built (a program that names
    // Compiler): what it was compiled from, so that the compiler's view of it can be made again.
    // Every file read but the standard library's, by its key (its whole path) and its text as
    // read; its main file's key; the library folders its imports were looked for in. Empty in
    // any other program.
    struct Source {
        std::string key, text;
    };
    std::vector<Source> compiledFrom;
    std::string compiledMain;
    std::vector<std::string> compiledLibraries;

    // Every function's run code made that is not made yet: before a parallel section, whose
    // cores share the functions and must not make one's run code while another runs it.
    void translateAll();

private:
    std::vector<std::unique_ptr<FunctionProto>> owned_;
};

// Checks that every operand of `f` is in range and that the stack depth is consistent on
// all paths, and records the function's stack requirement. The VM relies on both.
// Throws std::runtime_error if the bytecode is malformed. Its run code is made at its first
// call (Translate) unless `now`: a program's start makes only what it runs.
// `trusted`: the code is ww's own making (a kept program, a worker's image), and a function
// translated at its first call is checked by the verifier then, rather than now.
void verifyFunction(FunctionProto& f, const Program& program, bool now = false, bool trusted = false);
// The run code of a function verified but not yet translated, made now (Op::Translate).
void translateNow(FunctionProto& f);
// A function's run code until its first call: the one instruction Translate (registers.cpp).
void runLater(FunctionProto& f);

// For the compiler, part-way through a function: how many stack slots are in use where the
// code written so far ends, counted from the frame base, or -1 if that point is never reached.
int stackDepthAtEnd(FunctionProto& f, const Program& program);

void disassemble(const Program& program, std::ostream& out);

// How many classes and types a MakeType shape takes from the stack; -1 if it is malformed.
int shapeOperands(const std::string& shape);

// The length of the instruction at `at`, its operands included.
size_t instructionSize(const std::vector<uint8_t>& code, size_t at);

// Where in an instruction a place in the code to go to is, counted from the instruction's start
// in bytes; 0 for one that has none.
int targetOperand(Op op);

// An instruction's name, as the disassembly shows it.
const char* opName(Op op);

// Bytecode files. readProgram verifies everything it loads and throws std::runtime_error
// on a malformed file.
void writeProgram(const Program& program, std::ostream& out);
// `trusted` as for verifyFunction: a program kept in the cache, or a worker's image, both made by
// this ww; a bytecode file or a built program's own is checked whole as it is read.
void readProgram(Program& program, VM& vm, const std::string& data, bool trusted = false);
bool looksLikeBytecode(const std::string& data);

// ---- A part of a program kept by itself (D145) ----
//
// A file's or a folder's modules are compiled once and kept, and put back into a later
// compile of the program. What their code says of the rest of the program goes by numbers
// that compile gave out: a global's slot, a module's index, and a class's or library's number
// (Op::ClassNumber, Op::MemberAccess, FunctionProto::classIds). renumber gives each of those
// to the function for its kind and puts in what it returns, all through a function and the
// functions nested in it; with functions that return what they are given it only lists them.
struct Renumbering {
    std::function<int(int)> global, module, number;
};
void renumber(FunctionProto& f, const Renumbering& r, VM& vm);

// The modules of such a part, by themselves: written, and read back into `program` (not yet
// verified, since their numbers are still the ones they were kept with).
void writeModules(const std::vector<const FunctionProto*>& modules, std::ostream& out);
std::vector<FunctionProto*> readModules(Program& program, VM& vm, const std::string& data);
// Verifies a module read back and renumbered, as readProgram does every module.
// `trusted` as for verifyFunction: a unit ww kept and puts back, each function checked at its
// first call.
void verifyModule(FunctionProto& module, const Program& program, bool trusted = false);

// Takes out of a program the top-level declarations it can never reach, with their code
// (M6.5 part 5): for a built program, after it is compiled whole. `globalsByName` are the
// globals the VM looks up by their names, `membersByName` the members. Says what it took out.
struct LeftOut {
    size_t declarations = 0;
    size_t codeBefore = 0;  // the modules' own code, in bytes
    size_t codeAfter = 0;
};
LeftOut leaveOutUnreachable(Program& program, const std::vector<std::string>& globalsByName,
                            const std::vector<std::string>& membersByName);

// Whether any code of the program names one of these globals.
bool namesGlobal(const Program& program, const std::vector<std::string>& names, bool outsideStd = false);

// The name of the member the instruction at `at` uses (GetProp, Invoke and the like), with
// the signature of a form after it; null for an instruction that uses none.
const std::string* memberNameAt(const FunctionProto& f, size_t at);

}  // namespace ww
