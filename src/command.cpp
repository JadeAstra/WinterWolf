// The `ww` command: runs, compiles and disassembles WinterWolf programs, and hosts the shell.
// What a built program runs is in run.cpp; this, with the compiler, is in a part of the
// executable that `ww build` leaves out of what it writes (M6.5 part 5).

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache.h"
#include "checker.h"
#include "compiler.h"
#include "run.h"
#include "vm.h"
#include "profile.h"
#include "native/native.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "section_compiler.h"  // last: this file's own code goes in its section (D161)

#ifdef _WIN32
// The C runtime's tables of what runs as the program starts: the
// initializers of C++ globals and of C globals. (Those of thread_local variables, run as each
// thread starts, are not public; such a variable in those files is made on first use too.)
extern "C" {
using StartFunction = void(__cdecl*)();
extern StartFunction __xc_a[], __xc_z[];
extern StartFunction __xi_a[], __xi_z[];
}
#endif

using namespace ww;

namespace ww {
int docCommand(const std::vector<std::string>& args);  // src/doc.cpp
// The libraries ww.exe carries (lib/, packed by cmake/embed_lib.cmake, 2026-11-18).
extern const unsigned char kLibSource[];
extern const size_t kLibFileEnds[];
extern const char* const kLibFileNames[];
extern const size_t kLibFileCount;
extern const char kLibHash[];
// The standard library's sources ww.exe carries (cmake/embed.cmake), for `ww lib --std`.
extern const unsigned char kStdSource[];
extern const size_t kStdFileEnds[];
extern const size_t kStdFileCount;
extern const char* const kStdFileNames[];
}

namespace {

// Reads a whole file as bytes; false if it cannot be opened.
bool readBytes(const std::filesystem::path& path, std::string& out) { return readWholeFile(path, out); }

// What of the start-up code of this executable is in one of the named sections, or nothing.
// A built program has them left out of its file, so nothing in them may run before main, nor
// as a thread starts (D161): each file of theirs makes its globals on first use.
std::string startupCodeIn(const std::vector<std::string>& names) {
#ifdef _WIN32
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    auto inNamed = [&](const void* code) -> std::string {
        const uintptr_t rva = reinterpret_cast<uintptr_t>(code) - reinterpret_cast<uintptr_t>(base);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            const IMAGE_SECTION_HEADER& s = sections[i];
            if (rva < s.VirtualAddress || rva >= s.VirtualAddress + s.Misc.VirtualSize) continue;
            char name[9] = {};
            std::memcpy(name, s.Name, 8);
            return std::find(names.begin(), names.end(), std::string(name)) != names.end() ? std::string(name) : std::string();
        }
        return {};
    };
    const std::pair<StartFunction*, StartFunction*> tables[] = {{__xc_a, __xc_z}, {__xi_a, __xi_z}};
    for (const auto& table : tables) {
        for (StartFunction* entry = table.first; entry < table.second; entry++) {
            if (*entry == nullptr) continue;
            std::string section = inNamed(reinterpret_cast<const void*>(*entry));
            if (!section.empty()) {
                char at[32];
                std::snprintf(at, sizeof at, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(*entry) - reinterpret_cast<uintptr_t>(base)));
                return "start-up code at " + std::string(at) + " in the image" + " (in the section " + section + ")" +
                       " (a global of a file there made as the program starts: make it on first use)";
            }
        }
    }
#else
    (void)names;
#endif
    return {};
}

// The build's check of the sections a built program leaves out (D161), on the link maps of
// ww.exe, of the core and the window linked alone, and of the core alone: every function
// those need is in ww.exe in a section they keep, not one left out. A template or inline
// function that files on both sides use is made once, from either; this catches the one made
// on the side left out. What is wrong, or nothing.
std::string checkMaps(const std::string& wwMap, const std::string& withWindowMap, const std::string& plainMap) {
    // A map's sections by their number, and its symbols with the number of the section each is in.
    struct Map {
        std::unordered_map<std::string, std::string> sectionNamed;  // "0003" -> ".wwc"
        std::unordered_map<std::string, std::string> sectionOf;     // symbol -> "0003"
    };
    auto read = [](const std::string& path, Map& map) {
        std::ifstream file(path);
        if (!file) return false;
        std::string line;
        bool symbols = false;
        while (std::getline(file, line)) {
            if (line.find("Publics by Value") != std::string::npos) symbols = true;
            std::istringstream words(line);
            std::string place, second, third;
            if (!(words >> place) || place.size() != 13 || place[4] != ':') continue;
            if (!(words >> second)) continue;
            const std::string number = place.substr(0, 4);
            if (!symbols) {
                // " 0003:00000000 000e5720H .wwc   CODE": the first part of a section names it
                if (words >> third && third[0] == '.' && place.substr(5) == "00000000") {
                    const std::string name = third.substr(0, third.find('$'));
                    if (!map.sectionNamed.count(number)) map.sectionNamed[number] = name;
                }
                continue;
            }
            if (second[0] == '$') continue;  // labels inside functions
            map.sectionOf.emplace(second, number);
        }
        return true;
    };
    Map ww, withWindow, plain;
    if (!read(wwMap, ww) || !read(withWindowMap, withWindow) || !read(plainMap, plain)) return "a link map cannot be read";
    auto check = [&](const Map& needs, const std::vector<std::string>& leftOut, const std::vector<std::string>& allowed) -> std::string {
        for (const auto& [symbol, number] : needs.sectionOf) {
            if (std::find(allowed.begin(), allowed.end(), symbol) != allowed.end()) continue;
            auto where = ww.sectionOf.find(symbol);
            if (where == ww.sectionOf.end()) continue;
            auto name = ww.sectionNamed.find(where->second);
            if (name == ww.sectionNamed.end()) continue;
            if (std::find(leftOut.begin(), leftOut.end(), name->second) != leftOut.end())
                return symbol + " is needed by what a built program keeps, but ww.exe has it in " + name->second;
        }
        return {};
    };
    if (std::string wrong = check(withWindow, {".wwc", ".wws"}, {}); !wrong.empty()) return wrong;
    // The core calls the window's two entry points only when its section is there.
    return check(plain, {".wwc", ".wws", ".wwt"},
                 {"?registerJade@ww@@YAXAEAVVM@1@@Z", "?registerText@ww@@YAXAEAVVM@1@@Z"});
}

// Leaves the bytes of the named sections out of an executable's image (M6.5 part 5, D161):
// each keeps its place in memory, empty, so no address moves; the sections after it in the
// file move up, and the debug directory's places in the file with them. False if the image is
// not one this can read.
bool leaveOutSections(std::string& image, const std::vector<std::string>& names) {
#ifdef _WIN32
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    auto* bytes = reinterpret_cast<unsigned char*>(image.data());
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size())
        return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const unsigned count = nt->FileHeader.NumberOfSections;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<unsigned char*>(sections + count) > bytes + image.size()) return false;
    // A signed file's signature would no longer fit it; none is made here.
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].Size != 0) return false;

    // Where the debug directory's entries are in the file, before anything moves.
    std::vector<IMAGE_DEBUG_DIRECTORY*> debug;
    const IMAGE_DATA_DIRECTORY& debugDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    for (unsigned i = 0; i < count && debugDir.Size != 0; i++) {
        const IMAGE_SECTION_HEADER& s = sections[i];
        if (debugDir.VirtualAddress < s.VirtualAddress || debugDir.VirtualAddress + debugDir.Size > s.VirtualAddress + s.SizeOfRawData)
            continue;
        const size_t at = s.PointerToRawData + (debugDir.VirtualAddress - s.VirtualAddress);
        for (size_t e = 0; e + sizeof(IMAGE_DEBUG_DIRECTORY) <= debugDir.Size; e += sizeof(IMAGE_DEBUG_DIRECTORY))
            debug.push_back(reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(bytes + at + e));
    }
    std::vector<uint32_t> debugAt;  // their places in the file, kept apart while the bytes move
    for (const IMAGE_DEBUG_DIRECTORY* entry : debug) debugAt.push_back(entry->PointerToRawData);

    // The sections to leave out, last in the file first, so that each cut leaves the earlier
    // ones where they are.
    std::vector<unsigned> cut;
    for (unsigned i = 0; i < count; i++) {
        char name[9] = {};
        std::memcpy(name, sections[i].Name, 8);
        if (std::find(names.begin(), names.end(), std::string(name)) != names.end() && sections[i].SizeOfRawData != 0) cut.push_back(i);
    }
    std::sort(cut.begin(), cut.end(), [&](unsigned a, unsigned b) { return sections[a].PointerToRawData > sections[b].PointerToRawData; });

    std::vector<uint32_t> placed(count);
    for (unsigned i = 0; i < count; i++) placed[i] = sections[i].PointerToRawData;
    std::vector<std::pair<uint32_t, uint32_t>> cuts;  // in the file as it was: where, and how much
    for (unsigned i : cut) {
        const uint32_t from = sections[i].PointerToRawData, size = sections[i].SizeOfRawData;
        if (static_cast<size_t>(from) + size > image.size()) return false;
        cuts.emplace_back(from, size);
        for (unsigned j = 0; j < count; j++)
            if (placed[j] > from) placed[j] -= size;
        for (uint32_t& at : debugAt)
            if (at > from) at -= size;
        placed[i] = 0;
    }
    // The headers first, while they are where they were; then the bytes.
    for (unsigned i = 0; i < count; i++) sections[i].PointerToRawData = placed[i];
    for (unsigned i : cut) sections[i].SizeOfRawData = 0;
    for (size_t e = 0; e < debug.size(); e++) debug[e]->PointerToRawData = debugAt[e];
    for (const auto& [from, size] : cuts) image.erase(from, size);
    return true;
#else
    (void)image;
    (void)names;
    return true;
#endif
}


// ---- Help (2026-11-18) ----
//
// Every command and option, each once, with what `ww --help` lists of it and what
// `ww --help <name>` says of it, so that the two never differ. `of` is the command an option
// belongs to ("" for a command, or for an option taken before any command); `section` groups the
// list: 0 commands, 1 their options, 2 before any command, 3 for building ww itself, 4 settings.
struct HelpEntry {
    const char* name;
    const char* of;
    int section;
    const char* usage;
    const char* brief;  // the list's form of a command: its usage, shortened
    const char* summary;
    const char* details;
};

const HelpEntry kHelp[] = {
    // -- Commands --
    {"shell", "", 0, "ww   (or ww shell)", "ww", "start the interactive shell",
     "Starts the shell: each line is WinterWolf, run as it is entered, its value shown. What a line declares (a val, a\n"
     "function, a class) is kept for the lines after it, and an import brings a library in as in a file."},
    {"run", "", 0, "ww <file> [args...]   (or ww run <file> [args...])", "ww <file> [args...]", "run a source (.ww), Snippy (.snippy, .py) or bytecode (.wwc) file",
     "Runs the file: a source file (.ww) is compiled first, with what it imports, and the program compiled is kept\n"
     "in this machine's cache, so that its next run, with nothing changed, starts at once. A Snippy file (.snippy or\n"
     ".py; Snippy is a Python-like scripting language) is made into WinterWolf first, then compiled and kept the\n"
     "same way, the Snippy files it imports with it. A bytecode file (.wwc,\n"
     "made by ww build) is run as it is. What follows the file is the program's, in Program.arguments.\n"
     "An import is looked for next to the file, then in a lib folder beside ww.exe, then among the libraries\n"
     "ww.exe carries (Jade); one that cannot be found is said at once."},
    {"build", "", 0, "ww build <file> [-o <out>] [--exe] [--windowed] [--icon <file.ico>]", "ww build <file> [options]", "compile a source file to a bytecode file or a program",
     "Compiles the file and what it imports. By itself, into a bytecode file (<file>.wwc), which ww runs; with --exe,\n"
     "--windowed or --icon, or -o naming an .exe, into a program of its own, which runs with no ww: it carries what\n"
     "it needs of ww and leaves the rest out (the compiler, the libraries' sources, the window code where the program\n"
     "opens none). See ww --help --exe, --windowed, --icon and -o."},
    {"check", "", 0, "ww check [--std] <file>", "ww check [--std] <file>", "check a source file's types, running nothing",
     "Compiles the file and what it imports and checks their types, running nothing, and lists every problem\n"
     "found, each as a compile error is said (file:line:column: error: ...). The exit code is 0 where there is none."},
    {"test", "", 0, "ww test [<file or folder>...]", "ww test [<file or folder>...]", "run the tests (@Test) of the files or folders",
     "Runs the functions and methods written @Test in the files named, or in the .ww files of the folders named (the\n"
     "current folder where none is named), with @BeforeTest, @AfterTest, @BeforeAll and @AfterAll around them and\n"
     "@Timeout stopping one that runs too long; says how each went, and how many passed and failed."},
    {"dis", "", 0, "ww dis <file>", "ww dis <file>", "list the bytecode of a source or bytecode file",
     "Lists the bytecode of every function of the file, compiled first where it is a source file: for looking at what\n"
     "the compiler made."},
    {"lib", "", 0, "ww lib [<folder>] [--replace]   or   ww lib --std <folder> [--replace]", "ww lib [<folder>] [options]",
     "write the libraries ww carries (Jade) out as files, to change them",
     "ww lib writes the libraries ww.exe carries (Jade) as files into the lib folder beside ww.exe, or into the folder\n"
     "named. In the lib folder beside ww.exe they are used in place of those inside it, whole, so someone with only\n"
     "ww.exe may change them; ww --version then says so, and deleting the folder goes back to those inside. In\n"
     "another folder they are only to be read. A library already there is left as it is, its files perhaps changed,\n"
     "unless --replace. ww lib --std <folder> writes the standard library's files instead, into <folder>/std, to be\n"
     "read only: ww.exe can't and won't use them. See ww --help --replace and --std."},
    {"doc", "", 0, "ww doc <file>... [-o <page.html>] [--template <page.html>] [--title <text>] [--missing]", "ww doc <file>... [options]",
     "a reference page of the files' declarations, from their comments",
     "Makes an HTML page of the files' declarations (classes, functions, fields), each described by the comment\n"
     "written above it, and writes it to standard output, or to the file -o names. See ww --help --template,\n"
     "--title, --missing and -o."},
    {"--version", "", 0, "ww --version   (or -v)", "ww --version", "the version, and where each library comes from",
     "Says ww's version, and for each library ww.exe carries (Jade) whether a program takes it from inside ww.exe\n"
     "(\"built in\") or from a lib folder beside it, which is used in its place (see ww --help lib)."},
    {"--help", "", 0, "ww --help [<command or option>]   (or -h, or help)", "ww --help [<command or option>]", "this list, or more on one command or option",
     "With nothing after it, lists every command and option, one line each. With a command or an option after it,\n"
     "says more of that one: ww --help build, ww --help --exe, ww --help settings."},
    // -- Options of commands --
    {"-o", "build", 1, "ww build <file> -o <out>", "", "where the bytecode file or the program is written",
     "The file written. Without -o, beside the source, named as it is with .wwc (or the system's program extension,\n"
     ".exe on Windows, for a program). Naming an .exe asks for a program, as --exe does."},
    {"--exe", "build", 1, "ww build <file> --exe", "", "a program of its own, which runs with no ww",
     "Compiles the file into a program of its own (an .exe on Windows), which runs on a machine with no ww: it carries\n"
     "the part of ww it needs, the standard library compiled, and leaves out the compiler, the libraries' sources and,\n"
     "where the program opens no window, the window code."},
    {"--windowed", "build", 1, "ww build <file> --windowed", "", "a program that opens no console window (Windows)",
     "As --exe, marked as a windowed program, so that starting it opens no console window beside its own: for a\n"
     "program that imports Jade and opens a window. Its standard output goes nowhere."},
    {"--icon", "build", 1, "ww build <file> --icon <file.ico>", "", "the program's icon (Windows)",
     "As --exe, with the icon in the .ico file as the program's, as Explorer and the task bar show it."},
    {"--std", "check", 1, "ww check --std <file>", "", "the standard library's own problems listed too",
     "Lists what the check finds in the standard library as well as in the program: for working on the standard\n"
     "library itself."},
    {"--std", "lib", 1, "ww lib --std <folder> [--replace]", "", "the standard library's files, to be read only",
     "Writes the standard library's files, as they were when ww.exe was built, into <folder>/std, with a README.txt\n"
     "saying what this says: they are for reading only, and ww.exe can't and won't use them. It uses the copy\n"
     "compiled into it whatever is changed there; changing the standard library means building ww.exe again. Never\n"
     "into the lib folder beside ww.exe."},
    {"--replace", "lib", 1, "ww lib [<folder>] --replace", "", "write over a library already there",
     "Writes ww's own files over those of a library already in the folder (ww lib leaves one there as it is, its\n"
     "files perhaps changed). Files added to the library, which ww does not carry, are kept."},
    {"-o", "doc", 1, "ww doc <file>... -o <page.html>", "", "where the page is written",
     "The file the page is written to; without -o, standard output."},
    {"--template", "doc", 1, "ww doc <file>... --template <page.html>", "", "the page put into another, where it says",
     "Puts the reference where the page named says <!-- ww doc -->, the rest of that page as it is: for a page with\n"
     "a style and a story of its own around the reference."},
    {"--title", "doc", 1, "ww doc <file>... --title <text>", "", "the page's title",
     "The title of the page made (\"WinterWolf reference\" where none is given)."},
    {"--missing", "doc", 1, "ww doc <file>... --missing", "", "list the declarations with no comment",
     "Lists each declaration that has no comment above it (file:line and its name), and how many of all have one."},
    // -- Before any command --
    {"--interpret", "", 2, "ww --interpret <any command>", "", "run by the VM alone, with no native code",
     "Written before any command (ww --interpret <file>, ww --interpret test ...): the program is run by the VM alone,\n"
     "with no native code made for its loops and functions. For comparing, and for finding whether a problem is\n"
     "native code's. WW_NATIVE=0 does the same (ww --help settings)."},
    {"--profile", "", 2, "ww --profile[=<file>] <file> [args...]", "", "where the program spends its time, sampled",
     "Written before the command (ww --profile game.ww, ww --profile=out.txt test ...): every millisecond the program\n"
     "is asked where it is, and at its next loop turn or call the calls under way are counted. As the program ends\n"
     "the report is written to the file (ww-profile.txt where none is named): the functions by their own time (the\n"
     "code in them) and by their total time (with what they call), and the lines by their own time. The stacks are\n"
     "written beside it (<file>.folded) for a flame graph, which speedscope.app or flamegraph.pl draw. Only code\n"
     "running counts, on any of the program's threads; a thread asleep or waiting is not counted. A sample waits for\n"
     "the next loop turn or call, so a short function that does neither has its time counted mostly in its caller.\n"
     "WW_PROFILE=<file> does the same; WW_PROFILE_INTERVAL=<microseconds> samples more or less often."},
    // -- For building ww itself --
    {"--native-check", "", 3, "ww --native-check", "", "check native code (the build runs it)",
     "Checks the stencils native code is made of, rebuilt from the table ww.exe carries, against the hash taken when\n"
     "it was built, shapes each, and runs regions made of them beside C++'s answers. The build runs it."},
    {"--check-sections", "", 3, "ww --check-sections <exe> <map> <map>", "", "check what a built program leaves out (the build runs it)",
     "Checks that nothing a built program leaves out runs as it starts, and that the link maps of ww.exe and of the\n"
     "build's proof programs put every part in the section a built program keeps or leaves out as it should."},
    {"--attach-prelude", "", 3, "ww --attach-prelude <copy.exe>", "", "attach the compiled standard library (the build runs it)",
     "Writes a copy of ww.exe with the standard library compiled and attached, so that no program compiles it again.\n"
     "The build's last step, which then puts the copy in ww.exe's place."},
    // -- Settings --
    {"settings", "", 4, "WW_... environment variables", "", "WW_CACHE, WW_NATIVE, WW_CORES, WW_TIMES and the rest",
     "Read as ww starts, from environment variables:\n"
     "  WW_CACHE=<folder>            where compiled programs and the libraries' files are kept (otherwise\n"
     "                               %LOCALAPPDATA%\\WinterWolf\\cache on Windows, ~/.cache/ww elsewhere)\n"
     "  WW_NATIVE=0                  no native code, as ww --interpret\n"
     "  WW_NATIVE_BACKGROUND=0       native code made on the program's own thread, not on another core\n"
     "  WW_CORES=<n>                 how many cores a parallel section uses\n"
     "  WW_TIMES=1                   how long the program took to be ready to run, and its parts, on standard error\n"
     "  WW_PROFILE=<file>            where the program spends its time, written to the file, as ww --profile=<file>\n"
     "  WW_PROFILE_INTERVAL=<us>     how often --profile samples, in microseconds (1000)\n"
     "For testing ww itself: WW_GC_STRESS=1 (a collection at every safe point), WW_THREAD_STRESS=1 (threads change\n"
     "at every safe point), WW_CHECK_NATIVE_RESULTS=1 (every C++ function's result checked against its declaration),\n"
     "WW_COPY_READS=1 (every read from a list of values a copy), WW_CRASH_STACK=1 (on Windows, a crash writes the\n"
     "calls the crashing thread was in to standard error, for cdb's ln to name). Others, for working on native code,\n"
     "are described where src/ reads them."},
};

std::string helpOverview() {
    std::ostringstream out;
    out << "WinterWolf " WW_VERSION " - an object-oriented scripting language\n";
    auto line = [&](const std::string& left, const char* summary) {
        out << left << std::string(left.size() < 38 ? 38 - left.size() : 1, ' ') << summary << "\n";
    };
    // Each command, with the options it takes under it (2026-11-18, the user's way round).
    const char* const titles[] = {"Commands", "", "Before any command", "For building ww itself", "Settings"};
    for (int section = 0; section <= 4; section++) {
        if (section == 1) continue;
        out << "\n" << titles[section] << ":\n";
        for (const HelpEntry& e : kHelp) {
            if (e.section != section) continue;
            line("  " + std::string(section == 0 ? e.brief : section == 4 ? "ww --help settings" : e.name), e.summary);
            if (section != 0) continue;
            for (const HelpEntry& option : kHelp)
                if (option.section == 1 && std::string(option.of) == e.name) line("      " + std::string(option.name), option.summary);
        }
    }
    out << "\nMore on any of them: ww --help <command or option>, as in ww --help build, ww --help --exe.\n";
    return out.str();
}

// `ww --help <name>`: each entry of that name (an option two commands take is said of each).
int helpOn(const std::string& asked) {
    std::string name = asked;
    if (name == "-h" || name == "help") name = "--help";
    if (name == "-v") name = "--version";
    if (name == "ww") name = "shell";
    if (name.rfind("WW_", 0) == 0) name = "settings";
    bool found = false;
    for (const HelpEntry& e : kHelp) {
        if (name != e.name) continue;
        if (found) std::cout << "\n";
        found = true;
        std::cout << e.usage << "\n";
        if (*e.of != '\0') std::cout << "(an option of ww " << e.of << ")\n";
        std::cout << "\n" << e.details << "\n";
    }
    if (found) return 0;
    std::cerr << "ww: there is no command or option '" << asked << "'; ww --help lists them all\n";
    return kExitUsage;
}

// The files of the libraries ww.exe carries written under `folder` (Jade/Block.ww and so on),
// each as it was in lib/ when ww was built; false where one could not be written.
bool writeLibraryFiles(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    size_t from = 0;
    for (size_t i = 0; i < kLibFileCount; i++) {
        const fs::path file = folder / fs::u8path(kLibFileNames[i]);
        fs::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(kLibSource + from), static_cast<std::streamsize>(kLibFileEnds[i] - from));
        if (!out) return false;
        from = kLibFileEnds[i];
    }
    return true;
}

// The names of the libraries ww.exe carries: the folders directly inside lib/ (Jade).
std::vector<std::string> builtInLibraryNames() {
    std::vector<std::string> names;
    for (size_t i = 0; i < kLibFileCount; i++) {
        const std::string path = kLibFileNames[i];
        const std::string top = path.substr(0, path.find('/'));
        if (std::find(names.begin(), names.end(), top) == names.end()) names.push_back(top);
    }
    return names;
}

// Said wherever the standard library's files are written out (2026-11-18, the user's words: that
// they are just for reading, and that ww.exe can't and won't use them).
const char* const kStdReadOnly =
    "these files of the standard library are for reading only: ww.exe can't and won't use them. It uses the copy "
    "compiled into it when it was built, whatever is changed here; changing the standard library means building "
    "ww.exe again.";

// The standard library's files, as they were in src/std when ww was built, written into
// `folder`, with a README.txt saying they are only to be read; false where one could not be.
bool writeStdFiles(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(folder, ec);
    size_t from = 0;
    for (size_t i = 0; i < kStdFileCount; i++) {
        // Each file is followed by a line end of the packing's own (embed.cmake), not the file's.
        std::ofstream out(folder / fs::u8path(kStdFileNames[i]), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(kStdSource + from), static_cast<std::streamsize>(kStdFileEnds[i] - 1 - from));
        if (!out) return false;
        from = kStdFileEnds[i];
    }
    std::ofstream readme(folder / "README.txt", std::ios::binary | std::ios::trunc);
    readme << "WinterWolf " WW_VERSION ": " << kStdReadOnly << "\n";
    return static_cast<bool>(readme);
}

// `ww lib --std <folder> [--replace]`: the standard library's files written into <folder>/std,
// to be read; never where ww looks for libraries, and ww never uses them (kStdReadOnly).
int libStdCommand(const std::string& named, bool replace) {
    namespace fs = std::filesystem;
    if (named.empty()) {
        std::cerr << "ww: usage: ww lib --std <folder> [--replace]: the standard library's files, written into <folder>/std to be read\n";
        return kExitUsage;
    }
    std::error_code ec;
    const fs::path to = fs::path(named) / "std";
    if (fs::equivalent(fs::path(named), executablePath().parent_path() / "lib", ec)) {
        std::cerr << "ww: the lib folder beside ww.exe is where ww looks for libraries; write the standard library's files somewhere else\n";
        return kExitUsage;
    }
    if (!replace && fs::exists(to, ec)) {
        std::cerr << "ww: " << to.string() << " is there already, and is left as it is; ww lib --std " << named << " --replace writes ww's own over it\n";
        return kExitCannotWrite;
    }
    if (!writeStdFiles(to)) {
        std::cerr << "ww: cannot write the standard library's files into " << to.string() << "\n";
        return kExitCannotWrite;
    }
    std::cout << "the standard library written to " << to.string() << "\n";
    std::cerr << "ww: warning: " << kStdReadOnly << "\n";
    return 0;
}

// `ww lib [<folder>] [--replace]` (2026-11-18): the libraries ww.exe carries written out as files,
// into the lib folder beside ww.exe unless another is named, so that someone who has only
// ww.exe may change them: a lib folder beside ww.exe is used in place of what it carries
// (libraryPaths). A library already there is left as it is, its files someone may have changed,
// unless --replace says to write ww's over them (files added to it are kept either way).
int libCommand(const std::vector<std::string>& args) {
    namespace fs = std::filesystem;
    bool replace = false, standard = false;
    std::string named;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "--replace") replace = true;
        else if (args[i] == "--std") standard = true;
        else if (named.empty() && args[i].rfind("--", 0) != 0) named = args[i];
        else {
            std::cerr << "ww: usage: ww lib [<folder>] [--replace], or ww lib --std <folder> [--replace]\n";
            return kExitUsage;
        }
    }
    if (standard) return libStdCommand(named, replace);
    const fs::path beside = executablePath().parent_path() / "lib";
    const fs::path to = named.empty() ? beside : fs::path(named);
    std::error_code ec;
    if (!replace) {
        std::vector<std::string> there;
        for (const std::string& library : builtInLibraryNames())
            if (fs::exists(to / library, ec)) there.push_back((to / library).string());
        if (!there.empty()) {
            std::cerr << "ww: " << there[0] << (there.size() > 1 ? " and others are" : " is")
                      << " there already, and is left as it is (its files may have been changed); "
                      << "ww lib --replace writes ww's own over them\n";
            return kExitCannotWrite;
        }
    }
    if (!writeLibraryFiles(to)) {
        std::cerr << "ww: cannot write the libraries into " << to.string() << "\n";
        return kExitCannotWrite;
    }
    const bool inEffect = fs::equivalent(to, beside, ec);
    for (const std::string& library : builtInLibraryNames())
        std::cout << library << " written to " << (to / library).string() << "\n";
    if (inEffect)
        std::cout << "a program now takes them from there, in place of those built into ww.exe (ww --version says so); "
                  << "delete the folder to go back to those\n";
    else
        std::cout << "a program takes them from there only once they are in a lib folder beside ww.exe\n";
    return 0;
}

// The libraries ww.exe carries, written out once into a folder of this machine's cache named by
// their hash (lib-<hash>), so that everything that reads a library's files (an import, a
// folder's files, a kept program's look at what it was made from) reads them there as it would
// any others. Written to a folder of this process's own and moved into place whole, so that two
// ww's starting at once never see half of one. Empty where there is no cache folder or it cannot
// be written; a folder of another hash, from another ww, is let go a month after it was made.
std::string builtInLibraries() {
    static const std::string folder = []() -> std::string {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = cacheRootFolder();
        if (root.empty() || kLibFileCount == 0) return {};
        const std::string name = "lib-" + std::string(kLibHash).substr(0, 16);
        const fs::path dir = root / name;
        if (fs::exists(dir / ".whole", ec)) return dir.string();
#ifdef _WIN32
        const unsigned long pid = GetCurrentProcessId();
#else
        const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
        const fs::path temporary = root / (name + "." + std::to_string(pid) + ".tmp");
        fs::remove_all(temporary, ec);
        fs::create_directories(temporary, ec);
        bool written = !ec && writeLibraryFiles(temporary);
        if (written) written = static_cast<bool>(std::ofstream(temporary / ".whole", std::ios::binary) << kLibHash);
        if (written) fs::rename(temporary, dir, ec);
        if (!written || ec) {
            fs::remove_all(temporary, ec);
            if (!fs::exists(dir / ".whole", ec)) return {};  // another ww may have put the same in place meanwhile
        }
        // The folders other ww's wrote, of other hashes, a month old; and any left half written.
        const auto now = fs::file_time_type::clock::now();
        for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
            const std::string entry = e.path().filename().string();
            if (entry.rfind("lib-", 0) != 0 || entry == name || !e.is_directory(ec)) continue;
            const bool half = entry.size() > 4 && entry.compare(entry.size() - 4, 4, ".tmp") == 0;
            const auto made = fs::last_write_time(half ? e.path() : e.path() / ".whole", ec);
            if (ec || now - made > (half ? std::chrono::hours(1) : std::chrono::hours(24 * 30))) fs::remove_all(e.path(), ec);
        }
        return dir.string();
    }();
    return folder;
}

// Where the libraries that come with WinterWolf, such as Jade, are looked for: a "lib" folder
// beside the executable first, where there is one, whose libraries take the place of those ww
// carries (a newer Jade without building ww again; `ww --version` says so); then those ww carries
// (builtInLibraries). Nowhere else, so that a folder someone else can write to is never searched.
std::vector<std::string> libraryPaths() {
    std::filesystem::path exe = executablePath();
    std::vector<std::string> paths;
    std::error_code ec;
    if (!exe.empty() && std::filesystem::is_directory(exe.parent_path() / "lib", ec)) paths.push_back((exe.parent_path() / "lib").string());
    if (const std::string inside = builtInLibraries(); !inside.empty()) paths.push_back(inside);
    return paths;
}

bool stdinIsTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}


#ifdef _WIN32
// Marks a Windows executable image as a windowed program, so that starting it opens no console.
bool makeWindowed(std::string& image) {
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return false;
    uint32_t peOffset = 0;
    std::memcpy(&peOffset, image.data() + 0x3C, sizeof peOffset);
    const size_t subsystemAt = static_cast<size_t>(peOffset) + 4 + 20 + 68;  // signature, file header, into the optional header
    if (subsystemAt + 2 > image.size() || std::memcmp(image.data() + peOffset, "PE\0\0", 4) != 0) return false;
    image[subsystemAt] = 2;  // IMAGE_SUBSYSTEM_WINDOWS_GUI
    image[subsystemAt + 1] = 0;
    return true;
}

// The pictures of an .ico file in the form an executable stores them: each picture as a
// resource of its own, and a directory that lists them.
struct IconResources {
    std::string directory;
    std::vector<std::string> pictures;
};

// Takes an .ico file apart; false if `ico` is not one.
bool readIcon(const std::string& ico, IconResources& out) {
    auto u16 = [&](size_t at) {
        return static_cast<uint32_t>(static_cast<unsigned char>(ico[at])) |
               static_cast<uint32_t>(static_cast<unsigned char>(ico[at + 1])) << 8;
    };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };

    // The file starts with a reserved zero, the type (1 for icons, 2 for cursors) and the
    // number of pictures, followed by a 16-byte entry for each picture.
    const size_t kHeader = 6, kEntry = 16;
    if (ico.size() < kHeader || u16(0) != 0 || u16(2) != 1) return false;
    const size_t count = u16(4);
    if (count == 0 || ico.size() < kHeader + count * kEntry) return false;

    out.directory.assign(ico, 0, kHeader);
    for (size_t i = 0; i < count; i++) {
        const size_t entry = kHeader + i * kEntry;
        const uint64_t size = u32(entry + 8), offset = u32(entry + 12);
        if (size == 0 || offset + size > ico.size()) return false;
        out.pictures.push_back(ico.substr(static_cast<size_t>(offset), static_cast<size_t>(size)));

        // An entry in an executable is the one from the file with the picture's position in
        // the file replaced by the number of its resource.
        const size_t id = i + 1;
        out.directory.append(ico, entry, 12);
        out.directory.push_back(static_cast<char>(id & 0xFF));
        out.directory.push_back(static_cast<char>(id >> 8));
    }
    return true;
}

// Stores the icon in the executable at `exe`, as the icon Windows shows for the file. This
// rewrites the file, so it has to happen before the compiled program is attached.
bool storeIcon(const std::filesystem::path& exe, const IconResources& icon) {
    HANDLE update = BeginUpdateResourceW(exe.c_str(), FALSE);
    if (update == nullptr) return false;

    const WORD language = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    auto store = [&](const wchar_t* type, size_t id, const std::string& data) {
        return UpdateResourceW(update, type, MAKEINTRESOURCEW(id), language, const_cast<char*>(data.data()),
                               static_cast<DWORD>(data.size())) != 0;
    };
    const wchar_t* const pictureType = MAKEINTRESOURCEW(3);     // RT_ICON
    const wchar_t* const directoryType = MAKEINTRESOURCEW(14);  // RT_GROUP_ICON
    bool stored = store(directoryType, 1, icon.directory);
    for (size_t i = 0; stored && i < icon.pictures.size(); i++) stored = store(pictureType, i + 1, icon.pictures[i]);

    return EndUpdateResourceW(update, stored ? FALSE : TRUE) != 0 && stored;
}
#endif

// Loads `path` into `program`, compiling it unless it is already bytecode.
// Returns 0 on success, otherwise the exit code after reporting the problem.
// Runs `work`, adding the time it takes to `total`.
template <typename Work>
static bool timed(double& total, Work work) {
    const auto start = std::chrono::steady_clock::now();
    const bool result = work();
    total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}

// A kept program, read as a bytecode file is; one that cannot be read counts as not kept.
static bool loadKept(VM& vm, const std::string& bytecode, std::unique_ptr<Program>& owner) {
    auto kept = std::make_unique<Program>();
    try {
        readProgram(*kept, vm, bytecode, true);
    } catch (const std::exception&) {
        return false;
    }
    owner = std::move(kept);
    return true;
}

int loadProgram(const std::string& path, VM& vm, CompilerSession*& session, Program*& program,
                std::unique_ptr<CompilerSession>& sessionOwner, std::unique_ptr<Program>& programOwner) {
    std::string data;
    if (!readFile(path, data)) {
        std::cerr << "ww: cannot read '" << path << "'\n";
        return kExitNoInput;
    }

    try {
        if (looksLikeBytecode(data)) {
            programOwner = std::make_unique<Program>();
            readProgram(*programOwner, vm, data);
            program = programOwner.get();
            session = nullptr;
        } else if (std::string kept; timed(compileTimes.cacheCheck, [&] { return cachedProgram(path, kept); }) &&
                                      timed(compileTimes.cacheRead, [&] { return loadKept(vm, kept, programOwner); })) {
            // Nothing it was made from has changed since it was kept: it starts at once (D145).
            program = programOwner.get();
            session = nullptr;
        }
        if (program != nullptr && session == nullptr && !looksLikeBytecode(data) && namesGlobal(*program, {"Compiler"}, true)) {
            // It compiles code into itself as it runs (Compiler.compile), which needs the compiler's
            // session that made it: compiled from its source again, the units kept put back.
            programOwner.reset();
            program = nullptr;
        }
        if (program == nullptr) {
            sessionOwner = std::make_unique<CompilerSession>(vm);
            sessionOwner->libraryPaths = libraryPaths();
            sessionOwner->compileFile(path);
            session = sessionOwner.get();
            program = &sessionOwner->program;
            const Program* made = program;
            if (!namesGlobal(*made, {"Compiler"}, true)) keepProgram(path, session->sourcesRead(), session->foldersRead(), [made] {
                std::ostringstream image;
                writeProgram(*made, image);
                return image.str();
            });
        }
    } catch (const CompileError& e) {
        std::cerr << e.format() << "\n";
        return kExitCompileError;
    } catch (const std::runtime_error& e) {
        std::cerr << "ww: " << path << ": " << e.what() << "\n";
        return kExitCompileError;
    }
    return 0;
}

// `ww check`: compiles the file and what it imports, then checks their types (M6.5).
// Each problem is printed as a compile error is; nothing runs.
int checkFile(const std::string& path, bool standardLibrary) {
    std::string data;
    if (!readFile(path, data)) {
        std::cerr << "ww: cannot read '" << path << "'\n";
        return kExitNoInput;
    }
    VM vm;
    Checker checker;
    checker.reportStandardLibrary = standardLibrary;
    CompilerSession session(vm, &checker);
    session.stopAtTypeErrors = false;  // every one is listed below
    session.libraryPaths = libraryPaths();
    try {
        session.compileFile(path);
    } catch (const CompileError& e) {
        std::cerr << e.format() << "\n";
        return kExitCompileError;
    }
    checker.run();
    for (const CheckDiagnostic& d : checker.diagnostics)
        std::cout << d.file << ":" << d.line << ":" << d.col << ": error: " << d.message << "\n";
    return checker.diagnostics.empty() ? 0 : kExitCompileError;
}

// With WW_TIMES set: how long the program took to be ready to run, and its parts (D145).
static void showTimes(const char* what) {
    if (std::getenv("WW_TIMES") == nullptr) return;
    const double ready = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - executableStarted).count();
#ifdef _WIN32
    // From when the system made the process, its loading included: the start a user waits for.
    FILETIME created, exited, kernel, user, now;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimePreciseAsFileTime(&now);
        auto ticks = [](const FILETIME& t) { return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        std::fprintf(stderr, "ww: %s %.1f ms after the process was made\n", what, (ticks(now) - ticks(created)) / 10000.0);
    }
#endif
    if (compileTimes.cacheRead > 0)
        std::fprintf(stderr, "ww: %s %.1f ms after ww.exe started its work, from the program kept: its files looked at %.1f, "
                     "it read in %.1f\n", what, ready, compileTimes.cacheCheck, compileTimes.cacheRead);
    else
        std::fprintf(stderr, "ww: %s %.1f ms after ww.exe started its work: parsing %.1f, checking %.1f, compiling %.1f "
                     "(units put back %d, compiled %d; the standard library %s)\n",
                     what, ready, compileTimes.parse, compileTimes.check, compileTimes.compile, compileTimes.unitsReused,
                     compileTimes.unitsCompiled, compileTimes.preludeAttached ? "attached" : "compiled");
    if (compileTimes.cacheRead <= 0)
        std::fprintf(stderr, "ww: and besides: the kept program looked at %.1f, the kept units read %.1f, loading %.1f; "
                     "of compiling, putting back %.1f\n",
                     compileTimes.cacheCheck, compileTimes.unitsRead, compileTimes.load, compileTimes.putBack);
}

int runFile(const std::string& path, std::vector<std::string> scriptArgs) {
    VM vm;
    vm.scriptArgs = std::move(scriptArgs);

    CompilerSession* session = nullptr;
    Program* program = nullptr;
    std::unique_ptr<CompilerSession> sessionOwner;
    std::unique_ptr<Program> programOwner;
    if (int failed = loadProgram(path, vm, session, program, sessionOwner, programOwner)) return failed;

    {
        const auto loading = std::chrono::steady_clock::now();
        vm.load(*program);
        compileTimes.load = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loading).count();
    }
    vm.liveSession = session;
    showTimes("ready to run");
    // WW_TIMES=compile stops here: the budgets are timed on programs that are not run.
    if (const char* times = std::getenv("WW_TIMES"); times != nullptr && std::strcmp(times, "compile") == 0) {
        // What is kept is written as a program runs; here, with nothing to run, it is waited for.
        const auto waited = std::chrono::steady_clock::now();
        finishKeeping();
        std::fprintf(stderr, "ww: waited %.1f ms for the cache to be written\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waited).count());
        return 0;
    }
    setSelfCommand({executablePath().string(), std::filesystem::absolute(path).string()});
    const int code = runProgram(vm, *program, program->entry);
    finishKeeping();
    return code;
}

int buildFile(const std::string& path, std::string outPath, bool asExecutable, bool windowed,
              const std::string& iconPath) {
    std::string data;
    if (!readFile(path, data)) {
        std::cerr << "ww: cannot read '" << path << "'\n";
        return kExitNoInput;
    }
    if (looksLikeBytecode(data)) {
        std::cerr << "ww: '" << path << "' is already a bytecode file\n";
        return kExitUsage;
    }

#ifdef _WIN32
    IconResources icon;
    if (!iconPath.empty()) {
        std::string ico;
        if (!readBytes(iconPath, ico)) {
            std::cerr << "ww: cannot read '" << iconPath << "'\n";
            return kExitNoInput;
        }
        if (!readIcon(ico, icon)) {
            std::cerr << "ww: '" << iconPath << "' is not an icon file; --icon takes an .ico file\n";
            return kExitUsage;
        }
    }
#else
    if (!iconPath.empty()) {
        std::cerr << "ww: --icon is only available on Windows\n";
        return kExitUsage;
    }
#endif

#ifdef _WIN32
    const char* executableExtension = ".exe";
#else
    const char* executableExtension = "";
#endif
    if (outPath.empty())
        outPath = std::filesystem::path(path).replace_extension(asExecutable ? executableExtension : ".wwc").string();
    if (asExecutable && std::filesystem::path(outPath) == std::filesystem::path(path)) outPath += ".out";

    VM vm;
    CompilerSession session(vm);
    session.libraryPaths = libraryPaths();
    try {
        session.compileFile(path);
    } catch (const CompileError& e) {
        std::cerr << e.format() << "\n";
        return kExitCompileError;
    }
    showTimes("compiled");

    // A program that compiles code into itself as it runs (Compiler.compile) carries what it was
    // compiled from, for the compiler's view of it to be made again, and keeps all of itself:
    // that code may name anything.
    const bool compilesItself = namesGlobal(session.program, {"Compiler"}, true);
    if (compilesItself) {
        session.keepSourcesIn(session.program);
    } else {
        // What the program can never reach is left out of it (M6.5 part 5).
        const LeftOut leftOut = leaveOutUnreachable(session.program, VM::globalsFoundByName(), vm.membersFoundByName());
        if (std::getenv("WW_TIMES") != nullptr)
            std::cerr << "ww: left out " << leftOut.declarations << " declarations; the modules' own code "
                      << leftOut.codeBefore << " bytes, now " << leftOut.codeAfter << "\n";
    }

    if (!asExecutable) {
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (out) writeProgram(session.program, out);
        out.flush();
        if (!out) {
            std::cerr << "ww: cannot write '" << outPath << "'\n";
            return kExitCannotWrite;
        }
        return 0;
    }

    // A program of its own: this executable, with the compiled program attached.
    std::ostringstream compiled;
    writeProgram(session.program, compiled);
    std::string bytecode = compiled.str();

    std::string image;
    if (!readBytes(executablePath(), image)) {
        std::cerr << "ww: cannot read the ww executable to build from\n";
        return kExitCannotWrite;
    }
    int64_t existing = bundledProgramSize(image);
    if (existing >= 0) image.resize(image.size() - kBundleTrailer - static_cast<size_t>(existing));
    // What a built program never needs is left out of it: the compiler side and the standard
    // library's source, and Jade's window with the text engine when nothing left in the program
    // names them (M6.5 part 5, D161). A program that compiles scripts as it runs (Script) keeps
    // all of it, and the compiled standard library its scripts are compiled against (the
    // scripting plan, part 1): a script may use anything.
    const bool scripts = namesGlobal(session.program, {"__script"}) || compilesItself;
    if (!scripts) stripPrelude(image);  // the program carries its own compiled copy of what it uses
    std::vector<std::string> unneeded;
    if (!scripts) unneeded = {".wwc", ".wws"};
    if (!scripts && !namesGlobal(session.program, {"__jade", "__text"})) unneeded.push_back(".wwt");
    if (const std::string inside = startupCodeIn(unneeded); !inside.empty()) {
        std::cerr << "ww: internal error: " << inside << "\n";
        return kExitCannotWrite;
    }
    if (!leaveOutSections(image, unneeded)) {
        std::cerr << "ww: cannot make a program from this executable (its file is not as expected)\n";
        return kExitCannotWrite;
    }

    if (windowed) {
#ifdef _WIN32
        if (!makeWindowed(image)) {
            std::cerr << "ww: cannot make a windowed program from this executable\n";
            return kExitCannotWrite;
        }
#endif
    }

    char sizeBytes[8];
    for (int i = 0; i < 8; i++) sizeBytes[i] = static_cast<char>((static_cast<uint64_t>(bytecode.size()) >> (8 * i)) & 0xFF);

    auto writeRuntime = [&] {
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        out.write(image.data(), static_cast<std::streamsize>(image.size()));
        out.flush();
        return static_cast<bool>(out);
    };
    if (!writeRuntime()) {
        std::cerr << "ww: cannot write '" << outPath << "' (is that program still running?)\n";
        return kExitCannotWrite;
    }

#ifdef _WIN32
    if (!iconPath.empty()) {
        // A virus scanner looking at the file just written can make this fail for a moment,
        // so it is tried a few times, each time on a fresh copy.
        bool stored = storeIcon(outPath, icon);
        for (int attempt = 0; !stored && attempt < 5; attempt++) {
            Sleep(200);
            stored = writeRuntime() && storeIcon(outPath, icon);
        }
        if (!stored) {
            std::error_code ignored;
            std::filesystem::remove(outPath, ignored);
            std::cerr << "ww: cannot put the icon into '" << outPath << "'\n";
            return kExitCannotWrite;
        }
    }
#endif

    std::ofstream out(outPath, std::ios::binary | std::ios::app);
    out.write(bytecode.data(), static_cast<std::streamsize>(bytecode.size()));
    out.write(sizeBytes, sizeof sizeBytes);
    out.write(kBundleMarker, sizeof kBundleMarker);
    out.flush();
    if (!out) {
        std::error_code ignored;
        std::filesystem::remove(outPath, ignored);
        std::cerr << "ww: cannot write '" << outPath << "'\n";
        return kExitCannotWrite;
    }
    out.close();

#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(outPath, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                              std::filesystem::perms::others_exec, std::filesystem::perm_options::add, ec);
#endif
    return 0;
}

int disassembleFile(const std::string& path) {
    VM vm;
    CompilerSession* session = nullptr;
    Program* program = nullptr;
    std::unique_ptr<CompilerSession> sessionOwner;
    std::unique_ptr<Program> programOwner;
    if (int failed = loadProgram(path, vm, session, program, sessionOwner, programOwner)) return failed;

    disassemble(*program, std::cout);
    finishKeeping();  // the cache, if it is being written, before the program ends
    return 0;
}

bool isBlank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

int shell() {
    VM vm;
    CompilerSession session(vm);
    session.libraryPaths = libraryPaths();
    bool interactive = stdinIsTerminal();
    if (interactive) std::cout << "WinterWolf " WW_VERSION " - type .exit to quit\n";

    std::string buffer;
    for (;;) {
        if (interactive) std::cout << (buffer.empty() ? "> " : ". ") << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // byte order mark from a pipe
        if (buffer.empty() && (line == ".exit" || line == ".quit")) break;

        buffer += line;
        buffer += '\n';
        if (isBlank(buffer)) {
            buffer.clear();
            continue;
        }

        int module;
        try {
            module = session.compileSource(buffer, "<shell>", ".", true);
        } catch (const CompileError& e) {
            // Input that merely stops too early continues on the next line; a blank line gives up.
            if (e.atEof && !isBlank(line)) continue;
            std::cerr << e.format() << "\n";
            buffer.clear();
            continue;
        }
        buffer.clear();

        vm.load(session.program);
        try {
            Value result = vm.runModule(module);
            if (!result.isNull()) std::cout << vm.repr(result) << "\n";
            vm.runEventLoop(false, 50);  // whatever has been posted and is ready, without waiting
        } catch (WwThrow& thrown) {
            reportUncaught(vm, thrown);
        } catch (const WwExit& exit) {
            return exit.code;
        }
        std::cout.flush();
    }
    return 0;
}

// ---- ww test (D153) ----
//
// Each file is compiled and run as a program of its own; then its functions written @Test are
// called, each with those written @BeforeTest before it and @AfterTest after it, and those
// written @BeforeAll and @AfterAll before and after them all. A class with methods written
// @Test is made anew for each of them, with no arguments, and its own such methods run on it;
// its static @BeforeAll and @AfterAll methods run once. A test passes unless it throws.
//
// Each test, with what runs around it, runs in a sandbox of its own (M6.6.2), made empty for
// it; @BeforeAll and @AfterAll run in one of the file's. @RealHost on a test or a class runs it
// on the real system instead, and @Sandboxed says what its sandbox is to be like. The file's own
// code runs on the real system, as a program's does.

const Annotation* annotationNamed(const Annotations& written, const char* name) {
    for (const Annotation& a : written)
        if (a.name == name) return &a;
    return nullptr;
}

// The functions of a file's top level, or the methods of a class, that ww test runs.
struct TestGroup {
    const ClassStmt* cls = nullptr;
    std::vector<const FunctionDecl*> tests, beforeTest, afterTest, beforeAll, afterAll;
    bool any() const { return !tests.empty(); }
    void note(const FunctionDecl& fn) {
        if (annotationNamed(fn.annotations, "Test")) tests.push_back(&fn);
        if (annotationNamed(fn.annotations, "BeforeTest")) beforeTest.push_back(&fn);
        if (annotationNamed(fn.annotations, "AfterTest")) afterTest.push_back(&fn);
        if (annotationNamed(fn.annotations, "BeforeAll")) beforeAll.push_back(&fn);
        if (annotationNamed(fn.annotations, "AfterAll")) afterAll.push_back(&fn);
    }
};

class TestRun {
public:
    TestRun(VM& vm, CompilerSession& session, Checker& checker, const std::string& path)
        : vm_(vm), session_(session), checker_(checker), path_(path) {}
    int passed = 0, failed = 0;

    void run(int entry) {
        const std::vector<StmtPtr>* statements = session_.statementsOf(path_);
        if (statements == nullptr) return;
        TestGroup top;
        std::vector<TestGroup> classes;
        for (const StmtPtr& s : *statements) {
            if (s->kind == StmtKind::Function) top.note(*static_cast<const FunctionStmt&>(*s).fn);
            if (s->kind != StmtKind::Class) continue;
            TestGroup group;
            group.cls = static_cast<const ClassStmt*>(s.get());
            for (const auto& method : group.cls->methods) group.note(*method);
            if (group.any()) classes.push_back(std::move(group));
        }
        std::cout << path_ << "\n";
        try {
            vm_.runModule(entry);
        } catch (WwThrow& thrown) {
            std::cout << "  FAIL  the file's own code, before its tests\n" << describe(thrown);
            failed++;
            return;
        }
        // What runs around the tests, in the file's sandbox.
        Value fileBox;
        std::string failure;
        if (!enter(HostChoice(), fileBox, failure)) {
            std::cout << "  FAIL  the file's sandbox\n" << failure;
            failed++;
            return;
        }
        TempRoot keepFileBox(vm_, fileBox);
        if (all(top.beforeAll, Value::null(), "@BeforeAll")) {
            for (const FunctionDecl* test : top.tests) one(top, *test, Value::null(), nullptr);
            for (TestGroup& group : classes) {
                const Value cls = vm_.globalAt(session_.globalOf(path_, group.cls->name));
                TempRoot keep(vm_, cls);
                // A class written @RealHost has its own run on the real system too.
                const bool real = annotationNamed(group.cls->annotations, "RealHost") != nullptr;
                Value classBox;
                if (real) enter(choiceOf(group.cls->annotations, nullptr), classBox, failure);
                if (all(group.beforeAll, cls, "@BeforeAll")) {
                    for (const FunctionDecl* test : group.tests) one(group, *test, cls, &top);
                    all(group.afterAll, cls, "@AfterAll");
                }
                if (real) leave(classBox);
            }
            all(top.afterAll, Value::null(), "@AfterAll");
        }
        leave(fileBox);
    }

private:
    VM& vm_;
    CompilerSession& session_;
    Checker& checker_;
    std::string path_;

    // What a test that failed threw, and where.
    std::string describe(WwThrow& thrown) {
        std::string text;
        try {
            text = vm_.stringify(thrown.value);
        } catch (const WwThrow&) {
            text = "an exception whose toString() failed";
        }
        // Where in the program: the standard library's own lines, as assertEquals's, left out.
        std::string out = "        " + text + "\n";
        int shown = 0;
        for (const std::string& line : thrown.trace)
            if (line.find("(<std>:") == std::string::npos && shown++ < 6) out += "          " + line + "\n";
        return out;
    }

    // The function or method itself: a global, or the member of the object or class.
    Value callee(const FunctionDecl& fn, Value on) {
        if (on.isNull()) return vm_.globalAt(session_.globalOf(path_, fn.name));
        return vm_.memberGet(on, vm_.intern(fn.name));
    }

    // Calls one, stopped at a safe point once `timeout` ms have passed; false, and what it
    // threw in `failure`, where it throws.
    bool call(Value fn, const std::vector<Value>& args, int64_t timeout, std::string& failure) {
        std::mutex mutex;
        std::condition_variable wake;
        bool done = false;
        std::thread watch;
        if (timeout > 0) vm_.armTimeLimit(timeout);
        if (timeout > 0)
            watch = std::thread([&] {
                std::unique_lock<std::mutex> hold(mutex);
                if (!wake.wait_for(hold, std::chrono::milliseconds(timeout), [&] { return done; })) vm_.timeLimitPassed(timeout);
            });
        bool ok = true;
        try {
            TempRoot keep(vm_, fn);
            vm_.call(fn, static_cast<int>(args.size()), args.data());
        } catch (WwThrow& thrown) {
            failure = describe(thrown);
            ok = false;
        } catch (const WwExit& exit) {
            failure = "        exit(" + std::to_string(exit.code) + ") was called\n";
            ok = false;
        }
        if (watch.joinable()) {
            {
                std::lock_guard<std::mutex> hold(mutex);
                done = true;
            }
            wake.notify_all();
            watch.join();
        }
        vm_.clearTimeLimit();
        return ok;
    }

    // Where a test runs (M6.6.2): the test's own @RealHost or @Sandboxed, else its class's;
    // without either, a sandbox of Linux's paths and the system's fonts.
    struct HostChoice {
        bool real = false;
        bool windows = false;
        std::string fontsFrom;  // a folder of fonts, relative to the test's file
    };

    HostChoice choiceOf(const Annotations& onTest, const ClassStmt* cls) {
        HostChoice choice;
        for (const Annotations* written : {&onTest, cls != nullptr ? &cls->annotations : nullptr}) {
            if (written == nullptr) continue;
            if (annotationNamed(*written, "RealHost") != nullptr) {
                choice.real = true;
                return choice;
            }
            const Annotation* asked = annotationNamed(*written, "Sandboxed");
            if (asked == nullptr) continue;
            const auto& call = static_cast<const CallExpr&>(*asked->call);
            for (size_t i = 0; i < call.args.size(); i++) {
                const std::string name = i < call.names.size() && !call.names[i].empty() ? call.names[i] : (i == 0 ? "paths" : "fontsFrom");
                const Expr& arg = *call.args[i];
                ConstValue value;
                if (name == "paths" && arg.kind == ExprKind::Member) choice.windows = static_cast<const MemberExpr&>(arg).name == "windows";
                if (name == "fontsFrom" && checker_.folded(arg, value) && value.kind == ConstValue::String && !value.s.empty()) {
                    std::filesystem::path folder(std::u8string(value.s.begin(), value.s.end()));
                    if (folder.is_relative()) folder = std::filesystem::path(std::u8string(path_.begin(), path_.end())).parent_path() / folder;
                    const std::u8string text = folder.lexically_normal().u8string();
                    choice.fontsFrom.assign(text.begin(), text.end());
                }
            }
            return choice;
        }
        return choice;
    }

    // Enters where a test, or what runs around tests, runs: a new sandbox, given in `box`, or
    // the real system, with null there. False, with `failure`, where the sandbox cannot be made.
    bool enter(const HostChoice& choice, Value& box, std::string& failure) {
        box = Value::null();
        if (choice.real) {
            vm_.enterHost(vm_.host);
            return true;
        }
        const Value start = vm_.globalAt(session_.globalOf(path_, "__startTestSandbox"));
        Value args[2] = {Value::boolean(choice.windows), choice.fontsFrom.empty() ? Value::null() : vm_.str(choice.fontsFrom)};
        TempRoot keep(vm_, args[1]);
        try {
            box = vm_.call(start, 2, args);
        } catch (WwThrow& thrown) {
            failure = describe(thrown);
            return false;
        }
        return true;
    }

    // Leaves what enter entered, which ends a sandbox.
    void leave(Value box) {
        if (box.isNull()) {
            vm_.leaveHost();
            return;
        }
        TempRoot keep(vm_, box);
        try {
            vm_.call(vm_.globalAt(session_.globalOf(path_, "__endTestSandbox")), 1, &box);
        } catch (WwThrow&) {
        }
    }

    // The functions run around tests; false where one fails, which is reported.
    bool all(const std::vector<const FunctionDecl*>& fns, Value on, const char* what) {
        for (const FunctionDecl* fn : fns) {
            std::string failure;
            if (!call(callee(*fn, on), {}, 0, failure)) {
                std::cout << "  FAIL  " << what << " " << fn->name << "\n" << failure;
                failed++;
                return false;
            }
        }
        return true;
    }

    // A test's @Timeout, in milliseconds, at least 1: `@Timeout(2.seconds)` (G52). 0 for none.
    int64_t timeoutOf(const FunctionDecl& test) {
        const Annotation* limit = annotationNamed(test.annotations, "Timeout");
        if (limit == nullptr) return 0;
        const auto& call = static_cast<const CallExpr&>(*limit->call);
        if (call.args.empty() || call.args[0]->kind != ExprKind::Member) return 0;
        const auto& written = static_cast<const MemberExpr&>(*call.args[0]);
        const int64_t unit = durationUnit(written.name);
        ConstValue amount;
        const Expr& object = *written.object;
        double count = 0;
        if (object.kind == ExprKind::Literal && static_cast<const LiteralExpr&>(object).type == LiteralExpr::Type::Int) count = static_cast<double>(static_cast<const LiteralExpr&>(object).intValue);
        else if (object.kind == ExprKind::Literal && static_cast<const LiteralExpr&>(object).type == LiteralExpr::Type::Float) count = static_cast<const LiteralExpr&>(object).floatValue;
        else if (checker_.folded(object, amount) && amount.kind == ConstValue::Int) count = static_cast<double>(amount.i);
        else if (checker_.folded(object, amount) && amount.kind == ConstValue::Float) count = amount.d;
        if (unit == 0 || count <= 0) return 0;
        return std::max<int64_t>(1, static_cast<int64_t>(count * static_cast<double>(unit) / 1e6 + 0.5));
    }

    // One test, with what runs around it: the file's @BeforeTest functions, then for a test of
    // a class an object of it and its own, and after it the same the other way round.
    void one(const TestGroup& group, const FunctionDecl& test, Value cls, const TestGroup* top) {
        std::string failure;
        std::vector<std::filesystem::path> made;
        std::vector<Value> args;
        const HostChoice choice = choiceOf(test.annotations, group.cls);
        Value box;
        bool ok = enter(choice, box, failure);
        const bool entered = ok;
        TempRoot keepBox(vm_, box);
        for (size_t i = 0; ok && i < test.params.size(); i++) {
            if (i >= test.paramAnnotations.size() || annotationNamed(test.paramAnnotations[i], "TempDir") == nullptr) {
                failure = "        a test takes no parameters but those written @TempDir\n";
                ok = false;
                break;
            }
            if (!choice.real) {
                // In the sandbox: a directory of its own there, which goes with it.
                try {
                    args.push_back(vm_.call(vm_.globalAt(session_.globalOf(path_, "__testTempDir")), 0, nullptr));
                } catch (WwThrow& thrown) {
                    failure = describe(thrown);
                    ok = false;
                }
                continue;
            }
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::temp_directory_path(ec) /
                                        ("ww-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(i));
            std::filesystem::create_directories(dir, ec);
            made.push_back(dir);
            const Value text = vm_.str(dir.string());
            TempRoot keepText(vm_, text);
            const Value pathClass = vm_.globalAt(session_.globalOf(path_, "FilePath"));
            try {
                args.push_back(vm_.call(pathClass, 1, &text));
            } catch (WwThrow& thrown) {
                failure = describe(thrown);
                ok = false;
            }
        }
        std::vector<std::unique_ptr<TempRoot>> keepArgs;
        for (const Value& arg : args) keepArgs.push_back(std::make_unique<TempRoot>(vm_, arg));

        Value object = Value::null();
        auto around = [&](const std::vector<const FunctionDecl*>& fns, Value on) {
            for (const FunctionDecl* fn : fns) {
                if (!ok) return;
                ok = call(callee(*fn, on), {}, 0, failure);
            }
        };
        if (ok && top != nullptr) around(top->beforeTest, Value::null());
        if (ok && group.cls != nullptr) {
            try {
                object = vm_.call(cls, 0, nullptr);
            } catch (WwThrow& thrown) {
                failure = describe(thrown);
                ok = false;
            }
        }
        TempRoot keepObject(vm_, object);
        if (ok) around(group.beforeTest, object);
        if (ok) ok = call(callee(test, object), args, timeoutOf(test), failure);
        // What comes after runs even where the test failed, as long as it began.
        std::string after;
        bool afterOk = true;
        for (const FunctionDecl* fn : group.afterTest)
            if (afterOk) afterOk = call(callee(*fn, object), {}, 0, after);
        if (top != nullptr)
            for (const FunctionDecl* fn : top->afterTest)
                if (afterOk) afterOk = call(callee(*fn, Value::null()), {}, 0, after);
        if (ok && !afterOk) {
            ok = false;
            failure = after;
        }
        if (entered) leave(box);
        for (const std::filesystem::path& dir : made) {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
        const std::string name = group.cls != nullptr ? group.cls->name + "." + test.name : test.name;
        if (ok) {
            passed++;
            std::cout << "  ok    " << name << "\n";
        } else {
            failed++;
            std::cout << "  FAIL  " << name << "\n" << failure;
        }
    }
};

// The files `ww test` is given, and those in the folders it is given whose text has @Test.
std::vector<std::string> testFiles(std::vector<std::string> given) {
    if (given.empty()) given.push_back(".");
    std::vector<std::string> files;
    for (const std::string& path : given) {
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) {
            files.push_back(path);
            continue;
        }
        std::vector<std::string> found;
        for (auto it = std::filesystem::recursive_directory_iterator(path, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".ww") continue;
            std::string text;
            if (readFile(it->path().string(), text) && text.find("@Test") != std::string::npos) found.push_back(it->path().string());
        }
        std::sort(found.begin(), found.end());
        files.insert(files.end(), found.begin(), found.end());
    }
    return files;
}

int testFilesCommand(const std::vector<std::string>& given) {
    int passed = 0, failed = 0, broken = 0;
    auto counts = [&] {
        std::cout << passed + failed << (passed + failed == 1 ? " test: " : " tests: ") << passed << " passed, " << failed << " failed";
        if (broken > 0) std::cout << "; " << broken << (broken == 1 ? " file" : " files") << " could not be compiled";
        std::cout << "\n";
        return failed == 0 && broken == 0 ? 0 : 1;
    };
    const std::vector<std::string> files = testFiles(given);
    for (const std::string& path : files) {
        std::string data;
        if (!readFile(path, data)) {
            std::cerr << "ww: cannot read '" << path << "'\n";
            broken++;
            continue;
        }
        VM vm;
        Checker checker;
        CompilerSession session(vm, &checker);
        session.libraryPaths = libraryPaths();
        int entry;
        try {
            entry = session.compileFile(path);
        } catch (const CompileError& e) {
            std::cerr << e.format() << "\n";
            broken++;
            continue;
        }
        vm.load(session.program);
        vm.liveSession = &session;  // Compiler.compile, as in a run from source
        // A worker a test starts loads its own copy of the program from these bytes, as in a run.
        Program& program = session.program;
        vm.programImage = [&program] {
            std::ostringstream image(std::ios::binary);
            writeProgram(program, image);
            return image.str();
        };
        TestRun run(vm, session, checker, path);
        try {
            run.run(entry);
        } catch (const WwExit& exit) {
            std::cout << "  FAIL  exit(" << exit.code << ") was called\n";
            run.failed++;
        }
        passed += run.passed;
        failed += run.failed;
        finishKeeping();
        if (vm.threadsAlive()) {
            // A thread a test left running cannot have its VM taken apart under it: the run ends
            // here, as a program's does (runProgram).
            const bool more = &path != &files.back();
            if (more) std::cout << "  a thread is still running: the files after this one are not tested\n";
            const int code = counts();
            profile::finish();
            std::cout.flush();
            std::cerr.flush();
            std::_Exit(more ? 1 : code);
        }
    }
    return counts();
}

int dispatch(const std::vector<std::string>& given) {
    // `ww --interpret ...`: everything the VM's own way, no native code made (M9.5).
    std::vector<std::string> args = given;
    // `ww --profile[=file] ...`, or WW_PROFILE=file: where the program spends its time, sampled.
    std::string profileTo;
    if (const char* set = std::getenv("WW_PROFILE"); set != nullptr && *set != '\0' && std::string(set) != "0")
        profileTo = std::string(set) == "1" ? "ww-profile.txt" : set;
    while (!args.empty() && (args[0] == "--interpret" || args[0] == "--profile" || args[0].rfind("--profile=", 0) == 0)) {
        if (args[0] == "--interpret") ww::native::setEnabled(false);
        else profileTo = args[0] == "--profile" ? "ww-profile.txt" : args[0].substr(10);
        args.erase(args.begin());
    }
    if (!profileTo.empty()) {
        int every = 1000;
        if (const char* set = std::getenv("WW_PROFILE_INTERVAL"); set != nullptr && std::atoi(set) > 0) every = std::atoi(set);
        ww::profile::start(profileTo, every);
    }
    if (args.empty()) return shell();

    const std::string& command = args[0];
    if (command == "--help" || command == "-h" || command == "help") {
        if (args.size() == 1) {
            std::cout << helpOverview();
            return 0;
        }
        if (args.size() == 2) return helpOn(args[1]);
        std::cerr << "ww: usage: ww --help [<command or option>]\n";
        return kExitUsage;
    }
    if (command == "--version" || command == "-v") {
        std::cout << "ww " WW_VERSION "\n";
        // Each library ww carries, and where a program takes it from: a lib folder beside ww
        // that has one is used in its place (libraryPaths), which is said, so that a copy left
        // there is never used unawares.
        const std::filesystem::path beside = executablePath().parent_path() / "lib";
        for (const std::string& library : builtInLibraryNames()) {
            std::error_code ec;
            if (std::filesystem::is_directory(beside / library, ec))
                std::cout << library << ": from " << (beside / library).string() << ", in place of the one built in\n";
            else
                std::cout << library << ": built in\n";
        }
        return 0;
    }
    if (command == "shell") return shell();
    if (command == "lib") return libCommand(args);

    if (command == "run") {
        if (args.size() < 2) {
            std::cerr << "ww: 'run' needs a file\n";
            return kExitUsage;
        }
        return runFile(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    }

    if (command == "build") {
        std::string input, output, icon;
        bool asExecutable = false, windowed = false;
        for (size_t i = 1; i < args.size(); i++) {
            if (args[i] == "-o" && i + 1 < args.size()) {
                output = args[++i];
            } else if (args[i] == "--icon" && i + 1 < args.size()) {
                icon = args[++i];
                asExecutable = true;
            } else if (args[i] == "--exe") {
                asExecutable = true;
            } else if (args[i] == "--windowed") {
                asExecutable = windowed = true;
            } else if (input.empty() && !args[i].empty() && args[i][0] != '-') {
                input = args[i];
            } else {
                std::cerr << "ww: usage: ww build <file> [-o <out>] [--exe] [--windowed] [--icon <file.ico>]\n";
                return kExitUsage;
            }
        }
        if (input.empty()) {
            std::cerr << "ww: 'build' needs a file\n";
            return kExitUsage;
        }
        // Asking for an .exe by name is asking for a program.
        if (std::filesystem::path(output).extension() == ".exe") asExecutable = true;
        return buildFile(input, output, asExecutable, windowed, icon);
    }

    if (command == "--check-sections" && args.size() == 4) {
        // The build's check of what a built program leaves out (D161): nothing of it runs as the
        // program starts, and nothing a built program keeps is made there (the link maps).
        std::string wrong = startupCodeIn({".wwc", ".wws", ".wwt"});
        if (wrong.empty()) wrong = checkMaps(args[1], args[2], args[3]);
        if (wrong.empty()) return 0;
        std::cerr << "ww: " << wrong << "\n";
        return 1;
    }

    if (command == "--native-check" && args.size() == 1) {
        // The build's check of native code (M9.5): the stencils this ww.exe carries, shaped and
        // put together into regions that are run, their answers beside C++'s.
        if (!ww::native::available()) {
            std::cout << "ww: no native code in this build\n";
            return 0;
        }
        std::string wrong;
        try {
            wrong = ww::native::check();
        } catch (const std::exception& e) {
            wrong = e.what();
        }
        if (!wrong.empty()) {
            std::cerr << "ww: native code: " << wrong << "\n";
            return 1;
        }
        std::cout << "ww: native code: " << ww::native::stencilCount() << " stencils, checked\n";
        return 0;
    }

    if (command == "--attach-prelude" && args.size() == 2) {
        // The build's last step: a copy of this executable with the standard library compiled
        // and attached (D145, point 3), which the build then puts in its place.
        compilingPrelude = true;
        VM vm;
        std::string image;
        try {
            CompilerSession session(vm);
            image = session.preludeImage();
        } catch (const CompileError& e) {
            std::cerr << e.format() << "\n";
            return kExitCompileError;
        }
        if (image.empty() || !attachPrelude(args[1], image)) {
            std::cerr << "ww: cannot attach the standard library to " << args[1] << "\n";
            return kExitCannotWrite;
        }
        return 0;
    }

    if (command == "check") {
        // `--std` reports what is found in the standard library as well, for working on it.
        bool standardLibrary = false;
        size_t at = 1;
        for (; at + 1 < args.size(); at++) {
            if (args[at] == "--std") standardLibrary = true;
            else break;
        }
        if (at + 1 != args.size()) {
            std::cerr << "ww: usage: ww check <file>\n";
            return kExitUsage;
        }
        return checkFile(args.back(), standardLibrary);
    }

    if (command == "test") return testFilesCommand(std::vector<std::string>(args.begin() + 1, args.end()));

    if (command == "doc") return docCommand(std::vector<std::string>(args.begin() + 1, args.end()));

    if (command == "dis") {
        if (args.size() != 2) {
            std::cerr << "ww: usage: ww dis <file>\n";
            return kExitUsage;
        }
        return disassembleFile(args[1]);
    }

    if (command.size() > 1 && command[0] == '-') {
        std::cerr << "ww: unknown option '" << command << "'; ww --help lists them all\n";
        return kExitUsage;
    }

    return runFile(command, std::vector<std::string>(args.begin() + 1, args.end()));
}

}  // namespace

namespace ww {

int wwCommandMain(const std::vector<std::string>& args) { return dispatch(args); }

// For scripts compiled as a program runs (script.cpp): where their imports of the libraries
// ww carries are looked for, as for the program's own.
std::vector<std::string> commandLibraryPaths() { return libraryPaths(); }

// For Snippy's translator (snippy.cpp): its program loaded as `ww` loads any, kept and all.
int commandLoadProgram(const std::string& path, VM& vm, CompilerSession*& session, Program*& program,
                       std::unique_ptr<CompilerSession>& sessionOwner, std::unique_ptr<Program>& programOwner) {
    return loadProgram(path, vm, session, program, sessionOwner, programOwner);
}

}  // namespace ww
