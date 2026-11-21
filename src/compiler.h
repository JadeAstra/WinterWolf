#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "bytecode.h"
#include "cache.h"

namespace ww {

class VM;
class Checker;

// A name declared at the top level of a file.
// Snippy files (.py, .snippy), made into WinterWolf source as they are read (snippy.cpp).
bool isSnippyPath(const std::string& path);
std::string translateSnippy(const std::string& path, const std::string& name, const std::string& source, bool main);

struct GlobalName {
    enum Visibility : uint8_t { Public, Internal, Private };

    int slot = -1;
    Visibility visibility = Public;
    bool isVal = false;
    bool declared = false;  // a declaration of it has been seen; otherwise it has only been used
    std::string file;       // the file that declares it

    // A function with several forms (overloads, D56): each form is in a slot of its own, by
    // its signature, and the name's slot holds the set of them, which calls the form a call's
    // arguments fit. Empty for a name declared once.
    bool isFunction = false;
    // For a function declared once outside the shell, with a return type written that is int,
    // float, bool or string: that type. Every return of such a function is checked or proved,
    // and its name is never given another value, so a call of it gives that type (M6.5 part 4).
    uint8_t returnType = 0;
    std::string firstSignature;  // of the first form, until a second one comes
    std::vector<std::pair<std::string, int>> forms;  // signature and slot, in the order written
    std::vector<std::string> keys;  // each form's formKey, which no two may share

    int formSlot(const std::string& signature) const {
        for (const auto& [written, at] : forms)
            if (written == signature) return at;
        return -1;
    }
};

// What tells the forms of a name apart: the types of its parameters as written,
// `(float, float)`; one without a type is `*`.
std::string signatureText(const FunctionDecl& fn);

// A type as written, as signatures show it: the last part of a class's name, `List<int>`, `string?`.
std::string annotationText(const TypeAnnotation& t, const std::vector<TypeParam>* own = nullptr);

// What makes two forms one: signatureText with the function's own type parameters by their
// places, `(#0)`, as the names they are written with tell no call apart.
std::string formKey(const FunctionDecl& fn);

// A folder of source files. Its files share their top-level names; other folders see the
// public ones when they import it, and the internal ones too if they belong to the same library.
struct Namespace {
    std::string directory;
    std::string library;
    std::unordered_map<std::string, GlobalName> names;  // public and internal
};

// The key an extension function's library has when the program runs (G31, D188): its folder's
// with its access word, or its file's for a private one; the standard library is one for all.
inline std::string libraryKey(const Namespace* ns, const std::string& fileKey, uint8_t visibility) {
    if (ns == nullptr || ns->directory == "<builtin>") return "p:<builtin>";
    if (visibility == 2) return "f:" + fileKey;
    return (visibility == 1 ? "i:" : "p:") + ns->directory;
}

// How long compiling took, by part, in milliseconds: shown when WW_TIMES is set, for the
// budgets of D145. A part inside another of the same kind is counted once.
struct CompileTimes {
    double parse = 0, check = 0, compile = 0;
    double cacheCheck = 0, cacheRead = 0;  // a kept program: its files looked at, and it read in
    int unitsReused = 0, unitsCompiled = 0;  // units of the program put back from the cache, and compiled
    bool preludeAttached = false;            // the standard library came compiled with ww.exe
    // Where a program is compiled with units put back: the kept units read (planReuse), their code
    // put back (part of compiling), and the program loaded into the VM after.
    double unitsRead = 0, putBack = 0, load = 0;
};
extern CompileTimes compileTimes;

// Set by `ww --attach-prelude`, which compiles the standard library to attach to ww.exe:
// the copy already attached, if any, is not used for that.
extern bool compilingPrelude;

// Reads a whole file; returns false if it cannot be opened. Strips a UTF-8 byte order mark.
bool readFile(const std::string& path, std::string& out);

// With `alone`, a CompilerSession made on this thread parses the standard library's source on
// this thread only, not a file a thread as it does otherwise: for one made again and again (a
// script's, script.cpp), whose helpers' blocks, freed on this thread, would join its pool and
// leave theirs to take memory anew each time (about 10 MB a compile, 2026-11-19).
void parsePreludeAlone(bool alone);

// WW_CHECK_NATIVE_RESULTS: every value a function of the C++'s gives is checked against its
// `external` declaration when the program runs (M6.6 part 1). The standard library is then
// compiled from its source, and compiled programs are kept apart from the others.
bool checkingNativeResults();

// WW_COPY_READS (M6.6 part 4, value-lists.html section 12): every read of an item of a list of
// values that the compiler found could be made where the item is is made as a copy instead; what
// a loop hands on to a function, which may change it through its place, stays the item. The
// suite run so must print what it prints without.
bool copyingReads();

// Compiles source files into one Program. A session outlives a single compile so the shell
// can keep adding code that sees earlier globals. All of its entry points throw CompileError.
class CompilerSession {
public:
    // The VM supplies interned strings for the constants; it must outlive the program.
    // Every file is parsed and handed to the checker before anything is compiled; without a
    // checker given (`ww check` gives one, to read what it found) the session has its own.
    explicit CompilerSession(VM& vm, Checker* checker = nullptr);
    ~CompilerSession();

    Checker* checker = nullptr;

    Program program;

    // Whether what the type checker finds in the program is a compile error (true but for
    // `ww check`, which lists everything it finds).
    bool stopAtTypeErrors = true;

    // Directories searched for an import that is not found next to the importing file.
    std::vector<std::string> libraryPaths;

    // The files the program was compiled from, and the folders whose files it took, for the
    // cache of compiled programs (D145).
    const std::vector<SourceRead>& sourcesRead() const { return sourcesRead_; }
    std::vector<FolderRead> foldersRead() const;

    // Compiles a file and everything it imports. Returns the entry module index.
    int compileFile(const std::string& path);
    // The file compileFile compiled, as its key (its whole path), or empty.
    const std::string& mainFile() const { return mainKey_; }

    // What compileFile compiled the program from, put in `into` (Program::compiledFrom), for a
    // program that compiles code into itself as it runs (Compiler.compile) to be written as
    // bytecode or built and still have the compiler's view of itself made again.
    void keepSourcesIn(Program& into) const;
    // A session made again for such a program (Compiler.compile's first call): its sources are
    // read from what the program carries, not from the disk, and no unit is put back or kept,
    // so that it is compiled exactly as it was. Throws CompileError.
    void compileAgain(const Program& from);

    // For `ww test` (D153): a compiled file's statements, and the global a name at its top
    // level is kept in; null and -1 where there is none.
    const std::vector<StmtPtr>* statementsOf(const std::string& path) const;
    int globalOf(const std::string& path, const std::string& name);

    // The standard library compiled, as its image to be attached to ww.exe (D145, point 3);
    // empty if it cannot be made.
    std::string preludeImage();

    // Compiles source text as a new module. In shell mode a trailing expression becomes the
    // module's result.
    int compileSource(const std::string& source, const std::string& name, const std::string& baseDirectory,
                      bool shell = false);

    // A script given to Script.compile as a program runs (scripting plan, part 1): source text
    // made the program's entry module, with names of its own as a worker's script has, its
    // imports looked for from `baseDirectory`. `names` gets the global slot of each name it
    // declares at its top level, which the program calls its functions by.
    int compileScript(const std::string& source, const std::string& name, const std::string& baseDirectory,
                      std::unordered_map<std::string, int>& names);
    // The same with a contract (part 2): its source compiled first, among the script's names, then
    // its proxies and `api` (an object of the interface named `api`, where one is), then the
    // script, then what makes it give the interface named `gives` (where one is). `modules` gets
    // each of these modules, in the order they are to run; `contractSlots`, where given, the global
    // slot of each name the contract and its proxies declared among the script's.
    int compileScript(const std::string& source, const std::string& name, const std::string& baseDirectory,
                      std::unordered_map<std::string, int>& names, const std::string& contractSource,
                      const std::string& contractName, const std::string& api, const std::string& gives,
                      std::vector<int>& modules, std::unordered_map<std::string, int>* contractSlots = nullptr);

    // A file named as a script's contract (Script.compile(..., api = Door#)): checked to be one
    // (it imports nothing and holds only declarations), and its proxies compiled into this program
    // as a module of their own, which the call runs first.
    struct ScriptContract {
        std::string file;
        std::string name;
        std::string source;
        int proxies = -1;
    };
    const ScriptContract& useContract(const std::string& file, const std::string& fromFile, const Node& at);

    // For a safe script (the scripting plan, part 3): what it imports only from under these
    // folders (its own, where it was given one) and the libraries ww carries.
    bool importsLimited = false;
    std::vector<std::string> importRoots;
    bool importAllowed(const std::string& found) const;

private:
    std::unordered_map<std::string, ScriptContract> contracts_;
    int scriptsCompiled_ = 0;  // compileScript's, for keys of their own
    friend class ModuleCompiler;

    VM& vm_;
    std::unique_ptr<Checker> ownChecker_;

    // For each function or method of the C++ that the standard library declares: how many
    // arguments the C++ takes, how many its declarations cover between them, and the first.
    struct ExternalForms {
        int min = 0, max = 0, lowest = 0, highest = 0;
        std::string file;
        int line = 0, col = 0;
    };
    std::map<std::string, ExternalForms> externalForms_;

    // Names with several forms whose forms have been defined, and whose set is made next.
    std::vector<std::pair<GlobalName*, std::string>> pendingForms_;

    // Each file's statements, parsed once (by its key), and those of source text that is not a
    // file: the checker reads them, then the compiler. They live as long as the session.
    std::unordered_map<std::string, std::unique_ptr<std::vector<StmtPtr>>> parsed_;
    std::vector<std::unique_ptr<std::vector<StmtPtr>>> sources_;
    std::vector<SourceRead> sourcesRead_;
    std::vector<Program::Source> rawSources_;  // each file's text as read, for keepSourcesIn
    bool reusingUnits_ = true;                 // false in compileAgain
    // compileAgain's: the sources the program carries, which its later compiles read too.
    std::shared_ptr<void> carried_;
    std::map<std::string, FolderRead> foldersRead_;
    void noteFolder(const std::string& folder, const std::vector<std::filesystem::path>& files);
    std::unordered_set<std::string> gathered_;  // files handed to the checker
    bool gatheringShell_ = false;               // the unit being gathered is an input of the shell

    // What the shell's inputs have imported so far: each input sees what those before it did.
    struct ShellImport {
        Namespace* ns;
        std::string alias, path;
        bool all;
    };
    std::vector<ShellImport> shellImports_;
    void check();
    std::vector<StmtPtr>* parseFile(const std::string& key, const std::string& path, const std::string& name);
    // The program's own file, whose Snippy, if it is Snippy, is the __main__ module (snippy.cpp).
    std::string mainKey_;
    // Files read and parsed ahead, a few at a time on threads of their own, for parseFile to
    // take: the files a file imports, before they are gathered in order.
    struct Prefetched {
        bool read = false;
        std::string source;
        uintmax_t size = 0;
        long long time = 0;
        uint64_t skeleton = 0;
        std::unique_ptr<std::vector<StmtPtr>> statements;
        std::unique_ptr<CompileError> error;
    };
    std::unordered_map<std::string, Prefetched> prefetched_;
    void prefetch(const std::vector<std::pair<std::string, std::string>>& files);  // key and name
    bool findImport(const std::string& path, const std::string& fromDirectory, const std::string& file,
                    std::string& found, bool& folder) const;
    void gather(const std::string& key, const std::string& name, const std::string& directory,
                const std::vector<StmtPtr>& statements, Namespace* ns, bool prelude);
    FunctionProto* compileStatements(const std::vector<StmtPtr>& statements, const std::string& name,
                                     const std::string& directory, const std::string& fileKey, bool shell, Namespace* ns,
                                     bool predeclared = false);

    // ---- Units kept compiled (D145) ----
    // What each module is, for putting a kept unit's modules back: a file's key, "folder:" and
    // the folder's path for the module its files share, "<worker>" and a worker's script; and
    // the index of each.
    std::vector<std::string> moduleKeys_;
    std::unordered_map<std::string, int> moduleByKey_;
    void noteModule(int index, const std::string& key);
    // What compiling each file depended on, noted while gathering: the files it imports (a
    // folder's, all of them), and the other files gathered from its folder, whose names it sees.
    std::unordered_map<std::string, uint64_t> textHashes_;
    std::unordered_map<std::string, uint64_t> skeletons_;  // the hash of what other files see of each
    std::unordered_map<std::string, std::set<std::string>> fileUses_;
    std::unordered_map<std::string, std::string> directoryOf_;
    std::unordered_map<std::string, std::set<std::string>> filesByDirectory_;
    std::vector<std::pair<std::string, uint64_t>> compiledFrom(const std::vector<std::string>& files) const;
    // Whether `to` is among the files compiling `from` depends on, as compiledFrom finds them.
    bool dependsOn(const std::string& from, const std::string& to) const;
    // Units compiled this time, kept once the whole program is; only within compileFile.
    struct ToKeep {
        std::string unit;
        std::vector<std::string> files;
        std::vector<int> modules;
    };
    std::vector<ToKeep> toKeep_;
    bool keepingUnits_ = false;
    // 1: the unit was put back; 0: it was not, and nothing was done; 2: its names were declared
    // already, but it has to be compiled after all.
    int reuseUnit(const std::string& unit, const std::vector<std::string>& files, const std::vector<int>& modules,
                  const std::vector<const std::vector<StmtPtr>*>& statements, const std::vector<std::string>& names,
                  const std::string& directory, Namespace* ns);
    int putBack(const std::string& image, const std::vector<std::string>& files, const std::vector<int>& modules,
                const std::vector<const std::vector<StmtPtr>*>& statements, const std::vector<std::string>& names,
                const std::string& directory, Namespace* ns);
    void numberNames(std::unordered_map<int, std::string>& slotKeys, std::unordered_map<int, std::string>& libraries) const;
    bool unitImage(const ToKeep& unit, const std::unordered_map<int, std::string>& slotKeys,
                   const std::unordered_map<int, std::string>& libraries, std::string& head,
                   std::vector<const FunctionProto*>& protos);
    static std::string preludeSource();
    bool usePrelude(const std::string& prelude, const std::string& image);
    void keepUnits();
    int slotFor(const std::string& key);
    // Before checking: the units whose code will be put back, whose bodies are then not checked
    // again; and, for one that could not be after all, its bodies checked before it is compiled.
    std::map<std::string, std::vector<std::string>> foldersGathered_;  // a folder imported whole, and its files
    std::unordered_map<std::string, KeptUnit> planned_;
    std::unordered_map<std::string, std::vector<std::string>> plannedFiles_;
    std::unordered_map<std::string, uint64_t> summaries_;  // each file's, as this compile has it
    std::vector<std::string> dependencies(const std::vector<std::string>& files) const;
    void settleReuse();
    void planReuse();
    // `kept`: the unit's entry in the cache, as keptUnit read it.
    void planUnit(const std::string& unit, const std::vector<std::string>& files, KeptUnit kept);
    void checkSkipped(const std::vector<std::string>& files);
    void reportNew(size_t before);
    int libraryNumber(const std::string& library, const std::string& file, int line);
    std::unordered_map<std::string, std::unique_ptr<Namespace>> namespaces_;  // by folder
    Namespace* builtins_ = nullptr;                                           // the prelude and the built-in names
    std::unordered_map<std::string, std::unordered_map<std::string, GlobalName>> privates_;  // by file: its private names
    std::unordered_map<std::string, std::vector<ShellImport>> fileImports_;  // by file: what it imports, for inline bodies (D59)
    std::unordered_map<std::string, std::string> fileNames_;  // by file: the name its stack trace lines give (a body put in place)
    std::unordered_map<int, std::string> globalClassTypes_;                   // global slot -> the class it is declared as
    std::unordered_map<std::string, std::vector<std::string>> enums_;         // enum (see enumKey) -> its entries

    // Assignments to globals, checked against `val` once everything has been compiled.
    std::vector<std::tuple<const GlobalName*, std::string, std::string, int, int>> globalAssignments_;  // name, file, line, col
    std::unordered_map<std::string, int> modules_;  // canonical path -> module index
    int classCount_ = 0;  // classes and libraries given a number so far (see Op::MemberAccess)
    std::unordered_map<std::string, int> libraryIds_;  // library -> its number

    // For a file loaded as part of its folder: the modules of that folder, in the order they
    // run. Importing the file alone runs them all, since its classes are set up by the first.
    std::unordered_map<std::string, std::shared_ptr<std::vector<int>>> units_;

    // A file that has been read and parsed but not compiled yet.
    struct Pending;
    Namespace* namespaceOf(const std::string& directory);
    int newSlot(const std::string& name);
    void predeclare(Namespace& ns, const std::string& fileKey, const std::string& file,
                    const std::vector<StmtPtr>& statements, bool shell);
    void loadFolder(const std::string& folder, const std::string& displayPath, const std::string& file, const Node& at,
                    std::vector<int>& modules);

    // `into`: the namespace the file's names go in, if not that of its folder.
    FunctionProto* compileUnit(const std::string& source, const std::string& name, const std::string& directory,
                               const std::string& fileKey, bool shell, Namespace* into = nullptr);
    // The script of a worker, compiled into the program. Returns its module index.
    int loadWorker(const std::string& path, const std::string& fromDirectory, const std::string& file, const Node& at);
    int loadModule(const std::string& path, const std::string& displayPath, const std::string& file, const Node* at);

    // Loads what an `import` names: a file, or every file of a folder. Gives the modules to
    // run, in order, and returns the namespace whose names the importer now sees.
    Namespace* importModule(const std::string& path, const std::string& fromDirectory, const std::string& file,
                            const Node& at, std::vector<int>& modules);
    void checkConstAssignments();
};

}  // namespace ww
