#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ast.h"

namespace ww {

struct Namespace;

// A type as the compiler writes it into a program for when it runs (M6.6 part 1): what a list
// or map literal is made to hold, and the like.
struct RuntimeType {
    enum Kind : uint8_t { Unknown, Any, Int, Float, Number, String, Bool, Bytes, Range, TypeValue, Function, List, Map, Class, TypeParam, Char, Byte, Index, Set, Values };
    Kind kind = Unknown;
    bool nullable = false;
    uint8_t storage = 0;  // Int or Float kept at a storage width (D148): kTypeInt16 and the rest
    bool frozen = false;  // a List, Map or Set: FrozenList and the others (D26)
    // A class: where its global is, in a namespace by `global` or, for a private one, in the
    // file `privateFile`; its type arguments in `args`.
    const Namespace* ns = nullptr;
    std::string privateFile;
    std::string global;
    bool value = false;     // a class that is a value type
    std::string typeParam;  // a type parameter: its name
    std::vector<RuntimeType> args;
};

// The value of a constant (R2, D139), worked out when the program is checked.
struct ConstValue {
    enum Kind : uint8_t { None, Int, Float, Bool, String, Char, Byte };  // a Char's or Byte's in `i`
    Kind kind = None;
    int64_t i = 0;
    double d = 0;
    bool b = false;
    std::string s;
};

// One mistake the type checker found.
struct CheckDiagnostic {
    std::string file;
    int line = 0;
    int col = 0;
    std::string message;
};

// The type checker (M6.5): a pass of its own over the whole program, after it has been
// parsed and before it is compiled, that works out the type of every expression and reports
// what cannot be right (a wrong argument, a member that does not exist, an int put where a
// string goes, a value that may be null used as if it could not be).
//
// What it finds is a compile error: the program does not run (M6.5 part 3, step 5). `ww
// check` lists all of it. A parameter has its type written; a local's type is worked out from
// its first value, and a value whose type cannot be known is `any`, used through a cast.
// How many nanoseconds one of a Duration's units is (`seconds`, as `2.seconds` writes it); 0
// for a name that is no unit (G52).
int64_t durationUnit(const std::string& name);

// A member of a class as reflection lists it (D38), for the description the compiler writes.
struct ReflectedMember {
    std::string name;
    const Annotations* annotations = nullptr;  // those written on it (D153)
    const FunctionDecl* fn = nullptr;   // a method's or an init's; null for a field and a generated init
    RuntimeType type;                   // a field's type, a method's result; Unknown where it gives nothing
    int flags = 0;                      // a field's: 1 it may be set, 2 a property worked out by its getter, 4 static;
                                        // a constructor's: 8 a secondary one, the static function `fn`
    std::vector<std::string> paramNames;
    std::vector<RuntimeType> params;
    std::vector<bool> optional;         // the parameter has a default
    std::vector<const Annotations*> paramAnnotations;  // each parameter's, where any are written
};
struct ReflectedClass {
    std::vector<ReflectedMember> fields, methods, constructors;
    std::vector<ReflectedMember> staticFields, staticMethods;  // its own (G44)
    std::vector<RuntimeType> kinds;     // an enum class's (D39)
    std::vector<RuntimeType> supertypes;  // the class it extends and its interfaces (G42)
};

class Checker {
public:
    Checker();
    ~Checker();

    // A file of the program, once it has been parsed. The statements stay the caller's, and
    // must outlive the checker. `ns` is the namespace its top-level names go in.
    void addUnit(const std::vector<StmtPtr>& statements, const std::string& file, const std::string& fileKey,
                 Namespace* ns, bool prelude, bool shell = false);

    // The shell's last input, taken back because it did not compile: the names it declared
    // are as they were before it.
    void forgetShellUnit();

    // A file whose compiled code is put back from the cache (D145), with nothing it was compiled
    // from changed: its declarations are read, but its bodies are not checked again. What was
    // learned from them that other files rely on, which of its functions run a lambda they are
    // given there and then (R1), is handed back as inPlaceFacts gave it. checkBodiesOf checks
    // such a file after all, when its code could not be put back.
    void skipBodiesOf(const std::string& fileKey, std::string inPlaceFacts);
    void checkBodiesOf(const std::string& fileKey);
    std::string inPlaceFacts(const std::string& fileKey);
    // What other files can learn of a file's functions only from their bodies: the result
    // types not written, and the lambda parameters run there and then. With the file's text
    // outside those bodies it makes the file's summary (D145).
    std::string summaryFacts(const std::string& fileKey);

    // An `import` in a file: what it imports, and how the file reaches it.
    void addImport(const std::string& fileKey, const std::string& alias, bool all, Namespace* ns, const Node* at = nullptr,
                   const std::string& path = "");

    // Checks what was added since the last run: what a file uses must have been added by
    // then. Diagnostics are in the order found, without repeats.
    void run();

    std::vector<CheckDiagnostic> diagnostics;

    // Whether what is found in the standard library is reported too (`ww check --std`).
    bool reportStandardLibrary = false;

    // A parameter of a named function, a method or a class header has a type written (M6.5
    // part 3, step 5).
    bool requireTypes = true;

    // For a read of a constant (`maxRows`, `Limits.max`), or of a val whose value is constant
    // and the same for every read (top level, object, static or local): its value, to be
    // written in its place. False for anything else.
    bool constantAt(const Expr& at, ConstValue& out);

    // For an expression made only of literals and constants (`rows * 2`, `"v" + major`): its
    // value, worked out as the VM would. False for anything else, and where working it out
    // would fail (a division by zero), which is left to happen when the program runs.
    bool folded(const Expr& e, ConstValue& out);
    // Whether `e` is an int constant given where a byte goes, and which byte (D19).
    bool byteAt(const Expr& e, uint8_t& out) const;
    // For arithmetic or a comparison (`a + b`, `a < b`, `n += 1`): 1 where its operands are always
    // two ints, 2 where always two floats, 0 for anything else (M6.7 step 5).
    uint8_t numericKind(const Expr& e) const;
    // For a call of a C++ function with named arguments or a lambda after it: the argument
    // each parameter takes in turn (-1 for one not given, passed as null); null otherwise.
    const std::vector<int>* argumentOrder(const CallExpr& call) const;

    // For a call of a name with several forms (overloads, D56): the declaration of the form it
    // means, where that could be told; null otherwise, and the form is chosen when it runs.
    const FunctionDecl* chosenForm(const CallExpr& call) const;
    // For `grid[x, y]` (the IndexExpr) or `grid[x, y] = v` (the AssignExpr) on a class with an
    // op [] or op []= of its own: the declaration of the form it means, and whether there are
    // several; null otherwise.
    const FunctionDecl* chosenOperator(const Node& at, bool& several) const;
    // For a call of an extension function: the declaration the checker found, whether or not
    // the name has other forms; null for any other call.
    const FunctionDecl* extensionCalled(const CallExpr& call) const;
    // A bare name, or a call of one (the call and its callee), written in an extension function
    // itself, which means a global: the object it runs on has nothing by the name, as its type
    // says. It is the global whatever the object turns out to be, as in a method of a class.
    bool globalPastReceiver(const Node& at) const;
    // A call of a class's name that chose a secondary constructor: the static function made of it.
    const FunctionDecl* secondaryConstructor(const CallExpr& call) const;
    // A value type made from its header with literal defaults left out: those defaults, which
    // the call passes itself; null for any other call.
    const std::vector<const Expr*>* headerDefaults(const CallExpr& call) const;
    // Bundles of named arguments (D143): the names `...bundle` passes on at a call; whether a
    // call is params(...), a bundle's copy(...), or `+` is of two bundles.
    const std::vector<std::string>* bundleExpansion(const Expr& bundle) const;
    bool bundleMake(const Node& at) const;
    bool bundleCopy(const Node& at) const;
    bool bundleAdd(const Node& at) const;

    // Whether an object of the class can change in what its == and hashCode see, so that it
    // cannot be a key of a map or a set (D26): the compiler marks the class so for the checks
    // made when the program runs.
    bool unstableKey(const ClassStmt& cls);

    // A kind of an enum class named bare where one is expected (D39): `Click(1, 2)` for an Event,
    // `is Click`. For such a name or type, the class's full name (`Event.Click`); null otherwise.
    const std::string* kindName(const Node& at) const;

    // A type written with a name `type` gave it (D41): the type it names, written out, for the
    // checks made when the program runs; null for any other.
    const TypeAnnotation* aliasExpansion(const TypeAnnotation& use) const;

    // A data class extending a data class (D65): how many of its header's fields are its
    // parent's, which it starts with; 0 for any other class.
    size_t inheritedDataFields(const ClassStmt& cls);

    // For a method whose name has several forms among the classes it is related to: the
    // declaration, as high up as it goes, of the form it is (the one it overrides, or itself),
    // whose signature it is known by when the program runs. Null where the name has one form.
    const FunctionDecl* formRoot(const FunctionDecl& method);

    // The forms of a method name a class has, its own and those it inherits, by their roots;
    // empty where the name has one form.
    std::vector<const FunctionDecl*> formRoots(const ClassStmt& cls, const std::string& name);
    // The private member a member of `cls` written override takes the place of: the file key, a tab
    // and the name of the class that declared it (o-private-shadow); empty for any other.
    std::string overriddenPrivate(const ClassStmt& cls, const std::string& name);

    // For a fun in an extend (D186): the interface's method it is, by whose form it is known
    // when the program runs. Null where it is none of them.
    const FunctionDecl* extendForm(const FunctionDecl& fun) const;

    // For a list or map literal: the type it is made as, where that is known.
    bool literalType(const Expr& literal, RuntimeType& out) const;
    // A type written with a built-in type's name that means a class of the program's own there.
    bool namesOwnClass(const TypeAnnotation& written) const;
    // A type written with a name that names nothing in reach, which the checker has reported.
    bool namesNothing(const TypeAnnotation& written) const;
    // `val out : open List<T> = []`: the type it is declared as, which names a type parameter.
    bool declaredLiteralType(const Expr& literal, RuntimeType& out) const;

    // For a call of a generic function or class written in WinterWolf: what it gives the type
    // parameters, in their order, an Unknown where that was not worked out.
    bool typeArguments(const Node& call, std::vector<RuntimeType>& out) const;
    // Whether a binary operator calls an operator added to its left value's type (`op Number.*`).
    bool leftOperator(const Node& binary) const;

    // For a call of a function the C++ provides (`external`): what its declaration says it
    // gives, its type parameters filled in, and its name; false where that is nothing, or not
    // known.
    bool externalResult(const Node& call, RuntimeType& out, std::string& name) const;

    // For `pts.add(Point(1, 2))` on a list of a value type: whether the value made is the
    // list's item type, so that it can be made in the list (M6.6 part 3).
    bool addsNewValue(const Node& call) const;

    // Whether an expression of a value type is a place a change is made through (M6.6 part 4):
    // `p` in `p.x = 4` for a var p, `pts[i]`, `obj.pos`.
    bool isValuePlace(const Expr& e) const;
    // Values in variables (M6.7 step 6, D149): a local of a value type that keeps its value in a
    // place of its frame's; a store into a variable with no place of its own of a value that may
    // be in one, which keeps a copy; a read of a variable with a place that takes a copy, its
    // statement changing the variable inside it.
    bool placeLocal(const VarStmt& v) const;
    bool keepsPlace(const Node& store) const;
    bool copiesPlaceRead(const Node& read) const;
    // A place that is a list's item handed over where it is, which a change goes through to: a
    // for loop's item over a list, or the lambda's of a list's forEach (D167). Any other variable
    // takes a copy of its own before it is changed.
    bool isHandedPlace(const Expr& e) const;
    // A for loop over a list whose item is changed through: it hands the loop the item itself.
    bool changesItem(const ForInStmt& loop) const;
    // `val before = it`, where `it` is an item handed over that is changed through somewhere: the
    // variable takes a copy, so that it keeps the value as it was.
    bool keepsChangedItem(const Expr& e) const;

    // `5.seconds`: a read of an extension property, compiled as a call of its getter.
    bool extensionGetter(const MemberExpr& e) const;

    // An atomic field's change (D92), one operation: `x.hits += 1`, `best = max(best, v)`,
    // `x.hits.compareAndSet(a, b)`. `object` null for the object the code runs on.
    struct AtomicFieldOp {
        uint8_t kind = 0;  // 0 add, 1 subtract, 2 max, 3 min, 4 compare and set, 5 read, 6 set (under the lock, 8.7f)
        const Expr* object = nullptr;
        std::string field;
        const Expr* operand = nullptr;
        const Expr* expected = nullptr;
    };
    bool atomicField(const Expr& e, AtomicFieldOp& out) const;

    // A for through an Iterator: 0 any other; 1 over an Iterator itself; 2 over what an object's
    // iterator() gives.
    int protocolLoop(const ForInStmt& loop) const;

    // `Image#` and `x#` (D48): 0 where it is the class of the object x; 1 where it names a class
    // or a built-in type, written in `out`; 2 where it names a class declared inside a function,
    // whose name gives the class itself.
    int namedClassOf(const ClassOfExpr& e, RuntimeType& out) const;
    // What a built program keeps for reflection (D50, G42). For `x#`: the member asked at once
    // of the Class it gives ("fields" for `x#.fields`), empty where it is used otherwise; and
    // for `x#` of a value, the type the checker found it to have, false where that is not
    // known. A use of `methods`, `method` or `constructors` that asks nothing of a Class's
    // methods: on what is known not to be a Class, or at once on an `x#`.
    std::string classOfAsked(const ClassOfExpr& e) const;
    bool classOfValueType(const ClassOfExpr& e, RuntimeType& out) const;
    bool asksNoMethods(const MemberExpr& m) const;
    // A class's own members, as reflection lists them (D38); false where the class is not known.
    bool reflected(const ClassStmt& cls, ReflectedClass& out);
    // A generic value type's type arguments when the program runs (G4). The type parameters of a
    // value type that no field of it shows (a function type shows none): bit i for parameter i.
    // And the fields of a class that keep a value of such a type declared wider than what such a
    // parameter can be (`var id : Id<any>`), which keep the value's own beside it.
    uint32_t phantomParams(const ClassStmt& cls);
    std::vector<std::string> typeSlottedFields(const ClassStmt& cls);

    // Where an item of a list of values is read where it is, not copied (M6.6 part 4,
    // value-lists.html section 6): `pts[i]`, a for loop's variable; and the parameters, and
    // `this`, a function copies as it starts, should it be given an item where it is.
    bool readsInPlace(const IndexExpr& index) const;
    // Whether a field read (a member, or a bare name in a method) gives a value kept inside its
    // object where it is (M6.6 part 5): where nothing that may change values of its type runs
    // while it is in use.
    // A lambda run on an object its type says (`Grid.() -> unit`): `this` in it is that object (D58).
    bool runsOnObject(const FunctionDecl& lambda) const;
    // A lambda without an arrow given a function type that takes nothing: it has no `it`, which
    // is then that of a lambda around it (D55).
    bool hasNoIt(const FunctionDecl& lambda) const;
    // A lambda given where an interface with one abstract method is expected (D28): the
    // interface as the running program names it, and the method the lambda becomes.
    bool samConversion(const Expr& lambda, RuntimeType& iface, std::string& method, int& arity) const;
    // `Action { ... }`, an interface with one abstract method called with a lambda: the call is
    // that lambda, given as the interface (D28).
    bool samCall(const CallExpr& call) const;
    // A call of the function it is written in, on the same object, which nothing overrides:
    // as its last step, a loop (D29).
    bool callsItself(const CallExpr& call) const;
    // Such a call on another receiver (8.4c): the loop puts that receiver where `this` was.
    bool callsItselfOn(const CallExpr& call) const;
    // Any call of the function it is written in, on the same object; `overridden` when a
    // subclass has one of its own (D29, for `tail fun`).
    bool selfCall(const CallExpr& call, bool& overridden) const;
    // Script.compile or .load, or ScriptVM's, the standard library's: its api and gives name a contract.
    bool scriptCompileCall(const CallExpr& call) const;

    // A call of an inline function (D59): the function, which parameter each argument goes to,
    // and the file it is written in, whose names its body sees.
    struct InlineCall {
        const FunctionDecl* fn = nullptr;
        std::vector<int> paramOf;
        std::string fileKey;
        Namespace* ns = nullptr;
        const ClassStmt* owner = nullptr;  // a member's class
        std::string ownerName;             // as the program names it: `Outer.Inner`
        bool extension = false;
        bool prelude = false;              // the standard library's
        // Not written `inline`: a small function the checker chose to put in place of the call
        // (M9/M10 part zero), which a stack trace still shows as a call of its own.
        bool automatic = false;
        // And its body is worked out from its parameters alone, all of them numbers, booleans,
        // characters or text, by operators: nothing the program writes runs meanwhile.
        bool pure = false;
        // A class's method, which a subclass may have another of: put in place behind a check
        // that the object is of exactly that class, the call made as usual otherwise (2026-11-13).
        bool guarded = false;
    };
    // A call (or `a[i]` of an inline op [], B2) put in place.
    bool inlineCall(const Node& call, InlineCall& out) const;
    // Whether compiling file `from` depends on file `to` (CompilerSession::dependsOn): a body of
    // another file put in place where the call does not name it (an interface's one class, objects
    // plan part 2.1) only where a kept unit of `from` is compiled again when `to` changes.
    void setDependsOn(std::function<bool(const std::string& from, const std::string& to)> dependsOn);
    // Whether a lambda given for this parameter of an inline function is copied in with it: the
    // body only calls it, or passes it on to an inline function that copies it in (D59).
    bool copiedIn(const FunctionDecl& fn, int param) const;
    // `block(obj, ...)` and `obj.block(...)`: a function value of a type run on an object,
    // called on one (D58).
    bool callsOnObject(const CallExpr& call) const;
    // `block()` of a value run on an object, where the code runs on one of its type: run on that (M7).
    bool callsOnThis(const CallExpr& call) const;
    bool memberLocalCall(const CallExpr& call) const;
    // `a downTo b` or `r step n`, the standard library's (D72): which of them.
    bool rangeWord(const CallExpr& call, std::string& which) const;
    bool readsFieldInPlace(const Node& read) const;

    // A field written without its type: the type the checker works out for it, where that is an
    // int, a float or a bool, so that its objects keep it as a payload (M6.6 part 6).
    // A field with no type written whose value is an int, a float, a bool, a char or a byte, a
    // storage width's in `out.storage` (G18).
    bool inferredFieldType(const VarStmt& field, RuntimeType& out) const;
    // The list a function's `vararg` parameter gathers, `List<T>` for `vararg items : T` (G25).
    bool varargListType(const FunctionDecl& fn, RuntimeType& out) const;
    // A field with no type written whose value is of a value type, null or not (G16).
    bool inferredValueFieldType(const VarStmt& field, RuntimeType& out) const;
    bool loopsInPlace(const ForInStmt& loop) const;
    // `for (e in map)` over a map that is not null: an entry of it each time round.
    bool loopsOverMap(const ForInStmt& loop) const;
    bool loopPassesOn(const ForInStmt& loop) const;  // its variable's one use is to be passed on
    std::vector<int> paramsOwned(const FunctionDecl& fn) const;
    // The default an override takes for parameter `i` from what it overrides, where it writes
    // none of its own; null where there is none to take.
    const Expr* inheritedDefault(const FunctionDecl& fn, size_t i) const;
    // Whether a lambda gives a value to a variable of the code around it (`saved = it`).
    bool keepsOutside(const FunctionDecl& lambda) const;
    // An inline function's own loop that may read its items where they are and hand them to the
    // lambdas it was given (G15): every use of its variable an argument or what is returned; the
    // lists a function or lambda may change, the functions it was given counted or not; and
    // whether a lambda may give back a value (the item where it is, perhaps).
    bool loopPassesAllOn(const ForInStmt& loop) const;
    bool changesLists(const FunctionDecl& fn, bool countGiven) const;
    bool mayGiveValue(const FunctionDecl& lambda) const;
    bool ownsThis(const FunctionDecl& fn) const;

    // Results built where they go (M6.6 part 4): whether a function or lambda gives a value of a
    // value type, so may be given where its result goes; whether a call makes a value of a
    // value type; whether `pts[i] = f(...)` may build the result in the item.
    bool returnsValue(const FunctionDecl& fn) const;
    bool constructsValue(const CallExpr& call) const;
    bool buildsInPlace(const AssignExpr& assign) const;

    // For an extension function: its receiver's type as written; null for anything else.
    const TypeAnnotation* extensionReceiver(const FunctionDecl& fn) const;
    // For an extension function: the key of its library when the program runs (libraryKey).
    std::string extensionLibraryKey(const FunctionDecl& fn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ww
