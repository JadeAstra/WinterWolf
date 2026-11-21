#include "value.h"

#include "fmath.h"

#include <cstdlib>
#include <mutex>
#include <new>

#include <charconv>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace ww {

std::atomic<ObjClass*> smallClasses[kMaxSmallClasses];

uint32_t newShape() {
    static std::atomic<uint32_t> next{1};
    uint32_t shape;
    do shape = next.fetch_add(1, std::memory_order_relaxed);
    while (shape == 0);  // 0 is what a place that has found nothing yet remembers
    return shape;
}

bool valueBytesEqual(const ValueLayout& layout, const uint8_t* x, const uint8_t* y, int depth);

bool valuesEqual(const Value& a, const Value& b) {
    if (a.kind == b.kind) {
        switch (a.kind) {
            case ValueKind::Null:
            case ValueKind::Undefined:
                return true;
            case ValueKind::Bool:
                return a.as.b == b.as.b;
            case ValueKind::Int:
            case ValueKind::Char:
            case ValueKind::Byte:
            case ValueKind::FromEnd:
                return a.as.i == b.as.i;
            case ValueKind::Float:
                return a.as.d == b.as.d;
            case ValueKind::Small: {
                // Two values of one small value type, field by field (M6.6 part 5).
                if (a.refIndex != b.refIndex) return false;
                const ObjClass* cls = smallClassOf(a);
                return valueBytesEqual(*cls->layout.load(std::memory_order_acquire), smallBytes(a), smallBytes(b), 0);
            }
            case ValueKind::EnumName:
                return sameText(asString(a), asString(b));
            case ValueKind::Ref:
                // Callers make boxes of these first; here, where they are is all that is compared.
                return a.as.obj == b.as.obj && a.refIndex == b.refIndex && a.refOffset == b.refOffset && a.refDepth == b.refDepth;
            case ValueKind::Obj:
                if (a.as.obj == b.as.obj) return true;
                if (a.as.obj->type == ObjType::String && b.as.obj->type == ObjType::String) {
                    const ObjString* x = asString(a);
                    const ObjString* y = asString(b);
                    return sameText(x, y);
                }
                return false;
        }
        return false;
    }

    // A char and a string of that character alone are equal (D25).
    if (a.kind == ValueKind::Char || b.kind == ValueKind::Char) {
        const Value& c = a.kind == ValueKind::Char ? a : b;
        const Value& s = a.kind == ValueKind::Char ? b : a;
        return s.isString() && asString(s)->length == 1 && asString(s)->at(0) == static_cast<uint32_t>(c.as.i);
    }
    // A byte is the number it holds.
    const bool wholeA = a.kind == ValueKind::Int || a.kind == ValueKind::Byte;
    const bool wholeB = b.kind == ValueKind::Int || b.kind == ValueKind::Byte;
    if (wholeA && wholeB) return a.as.i == b.as.i;
    if (wholeA && b.kind == ValueKind::Float) return static_cast<double>(a.as.i) == b.as.d;
    if (a.kind == ValueKind::Float && wholeB) return a.as.d == static_cast<double>(b.as.i);
    return false;
}

// A key's hash, the same in every run and on every system, as Kotlin's is (8.2d): an int is its
// own hash, and hashCode() folds it to 32 bits as Kotlin's Long does (`5.hashCode()` is 5); a
// float its bits, as Kotlin's Double; a bool Kotlin's 1231 or 1237. A map mixes what it is given
// before it picks a slot (ValueMap::hashOf), so a run of ints spreads as well as ever.
size_t hashValue(const Value& v) {
    switch (v.kind) {
        case ValueKind::Bool:
            return v.as.b ? 1231 : 1237;
        case ValueKind::Int:
        case ValueKind::Byte:
            return static_cast<size_t>(v.as.i);
        case ValueKind::Char:
            return hashCharacter(kEmptyStringHash, static_cast<uint32_t>(v.as.i));  // as the string of it alone
        case ValueKind::FromEnd:
            return static_cast<size_t>(v.as.i) ^ 0x9e3779b97f4a7c15ULL;
        case ValueKind::Float: {
            // Integral floats must hash like the equal int so 1 and 1.0 are the same map key.
            double d = v.as.d;
            if (d >= -9.2e18 && d <= 9.2e18 && std::floor(d) == d) return static_cast<size_t>(static_cast<int64_t>(d));
            return static_cast<size_t>(fmath::canonicalBits(d));  // every NaN the one NaN
        }
        case ValueKind::Obj:
            if (v.as.obj->type == ObjType::String) return asString(v)->hash();
            return std::hash<const Obj*>()(v.as.obj);
        case ValueKind::Small: return keyHash(v, 0);
        default:
            return 0;
    }
}

namespace {
const int kMaxKeyDepth = 32;

// The objects whose contents make them what they are as a key.
bool hasContentKey(const Value& v) {
    if (!v.isObj()) return false;
    switch (v.as.obj->type) {
        case ObjType::List:
        case ObjType::Range:
        case ObjType::Bytes: return true;
        case ObjType::Instance: return asInstance(v)->cls->isData;
        case ObjType::Value: return true;
        // A key only frozen (D26); anything else asks by what it holds.
        case ObjType::Map:
        case ObjType::Set: return true;
        default: return false;
    }
}

Value objectAt(const uint8_t* at) {
    Obj* obj;
    std::memcpy(&obj, at, sizeof obj);
    return obj != nullptr ? Value::object(obj) : Value::null();
}

Value valueAt(const uint8_t* at) {
    Value v;
    std::memcpy(&v, at, sizeof v);
    return v;
}

double floatAt(const uint8_t* at) {
    double d;
    std::memcpy(&d, at, sizeof d);
    return d;
}
}  // namespace

bool keysEqual(const Value& a, const Value& b, int depth);
size_t keyHash(const Value& v, int depth);

// The fields of two values of one value type, compared as keys are, from their bytes in the
// type's layout (M6.6 part 2); a value held in place in another, field by field in turn.
bool valueBytesEqual(const ValueLayout& layout, const uint8_t* x, const uint8_t* y, int depth) {
    for (const ValueField& field : layout.fields) {
        const uint8_t* p = x + field.offset;
        const uint8_t* q = y + field.offset;
        switch (field.kind) {
            case ValueField::Int:
                if (std::memcmp(p, q, 8) != 0) return false;
                break;
            case ValueField::Bool:
                if (*p != *q) return false;
                break;
            case ValueField::Narrow:
                if (!valuesEqual(readNarrow(field.narrow, p), readNarrow(field.narrow, q))) return false;
                break;
            case ValueField::Float:
                if (!(floatAt(p) == floatAt(q))) return false;
                break;
            case ValueField::Object:
                if (!keysEqual(objectAt(p), objectAt(q), depth + 1)) return false;
                break;
            case ValueField::Any:
                if (!keysEqual(valueAt(p), valueAt(q), depth + 1)) return false;
                break;
            case ValueField::Nested:
                if (!valueBytesEqual(*field.cls->layout.load(std::memory_order_acquire), p, q, depth + 1)) return false;
                break;
        }
    }
    return true;
}

namespace {
// A part's hash taken into the whole's. The part's bits are spread first, as an int is its own
// hash: Point(1, 2) and Point(2, 1), or a run of small values, would otherwise land close.
size_t mixHash(size_t seed, size_t next) {
    next ^= next >> 33;
    next *= 0xff51afd7ed558ccdULL;
    next ^= next >> 33;
    return seed ^ (next + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

// What a value's or a data object's hash starts from: its class's name, not where the class is
// in memory, so that the hash is the same every run (8.2d).
size_t classSeed(const ObjClass* cls) { return cls->name->hash(); }

size_t valueBytesHash(const ValueLayout& layout, const uint8_t* x, int depth) {
    size_t h = 23;
    for (const ValueField& field : layout.fields) {
        const uint8_t* p = x + field.offset;
        switch (field.kind) {
            case ValueField::Int: {
                int64_t i;
                std::memcpy(&i, p, sizeof i);
                h = mixHash(h, hashValue(Value::integer(i)));
                break;
            }
            case ValueField::Bool: h = mixHash(h, *p ? 1231 : 1237); break;
            case ValueField::Narrow: {
                const Value v = readNarrow(field.narrow, p);
                h = mixHash(h, hashValue(v.isFloat() && v.as.d == 0.0 ? Value::number(0.0) : v));
                break;
            }
            case ValueField::Float: h = mixHash(h, hashValue(Value::number(floatAt(p) == 0.0 ? 0.0 : floatAt(p)))); break;
            case ValueField::Object: h = mixHash(h, keyHash(objectAt(p), depth + 1)); break;
            case ValueField::Any: h = mixHash(h, keyHash(valueAt(p), depth + 1)); break;
            case ValueField::Nested:
                h = mixHash(h, valueBytesHash(*field.cls->layout.load(std::memory_order_acquire), p, depth + 1));
                break;
        }
    }
    return h;
}
}  // namespace

namespace {
// An item of a list as a key sees it (M6.6 part 3): its value, or, for a value type's item,
// where its bytes are, whether the list keeps it packed or in a box.
struct KeyItem {
    Value value;
    const uint8_t* bytes = nullptr;
    const ObjClass* cls = nullptr;
};

KeyItem keyItem(const ObjList* list, size_t i) {
    if (!list->packed()) {
        const Value v = list->values()[i];
        if (v.isObjType(ObjType::Value)) return KeyItem{v, asValue(v)->data(), asValue(v)->cls};
        return KeyItem{v};
    }
    if (list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1) != 0) return KeyItem{Value::null()};
    const uint8_t* at = list->pack->block.data() + i * list->pack->width;
    switch (list->pack->store) {
        case ListStore::Ints: {
            int64_t v;
            std::memcpy(&v, at, sizeof v);
            return KeyItem{Value::integer(v)};
        }
        case ListStore::Floats: return KeyItem{Value::number(floatAt(at))};
        case ListStore::Narrow: return KeyItem{readNarrow(list->pack->narrow, at)};
        case ListStore::Bools: return KeyItem{Value::boolean(*at != 0)};
        default: return KeyItem{Value::null(), at, list->pack->itemClass};
    }
}

// A value of a value type in any of its forms: a box, one kept in the value itself (whose
// bytes are in `v`, which has to outlive them), or a Ref; its class and where its bytes are.
bool valueForm(const Value& v, const ObjClass*& cls, const uint8_t*& bytes) {
    if (v.kind == ValueKind::Small) {
        cls = smallClassOf(v);
        bytes = smallBytes(v);
        return true;
    }
    if (v.kind == ValueKind::Ref) {
        bytes = refBytes(v);
        if (bytes == nullptr) return false;
        cls = refClass(v);
        return true;
    }
    if (v.isObjType(ObjType::Value)) {
        cls = asValue(v)->cls;
        bytes = asValue(v)->data();
        return true;
    }
    return false;
}

bool keyItemsEqual(const KeyItem& a, const KeyItem& b, int depth) {
    const ObjClass* ca = a.cls;
    const ObjClass* cb = b.cls;
    const uint8_t* pa = a.bytes;
    const uint8_t* pb = b.bytes;
    if (ca == nullptr && a.value.isSmall()) valueForm(a.value, ca, pa);
    if (cb == nullptr && b.value.isSmall()) valueForm(b.value, cb, pb);
    if (ca != nullptr || cb != nullptr)
        return ca == cb && valueBytesEqual(*ca->layout.load(std::memory_order_acquire), pa, pb, depth + 1);
    return keysEqual(a.value, b.value, depth + 1);
}

size_t keyItemHash(const KeyItem& a, int depth) {
    if (a.cls != nullptr)
        return mixHash(classSeed(a.cls), valueBytesHash(*a.cls->layout.load(std::memory_order_acquire), a.bytes, depth));
    return keyHash(a.value, depth + 1);
}
}  // namespace

// Whether the running thread may look into an object: always, except in a parallel section
// for one that another core owns.
static bool readableHere(const Value& v) {
    return v.as.obj->core == 0 || v.as.obj->core == currentCore || (currentReadable != nullptr && currentReadable->test(v.as.obj->core));
}

// Whether a value is of a kind whose class may have an == and a hashCode of its own: an object
// or a value. Asked first, so that keys of other kinds never look at the thread's VM.
static inline bool mayOwnKeys(const Value& v) {
    return v.kind == ValueKind::Small || v.kind == ValueKind::Ref ||
           (v.kind == ValueKind::Obj && (v.as.obj->type == ObjType::Instance || v.as.obj->type == ObjType::Value));
}

// The class of an object or a value, where the class may have an == and a hashCode of its own
// (D26); null otherwise.
static const ObjClass* keyClassOf(const Value& v) {
    const ObjClass* cls = nullptr;
    if (v.kind == ValueKind::Small) {
        cls = smallClassOf(v);
    } else if (v.kind == ValueKind::Ref) {
        if (refBytes(v) != nullptr) cls = refClass(v);
    } else if (v.isObjType(ObjType::Instance)) {
        cls = asInstance(v)->cls;
    } else if (v.isObjType(ObjType::Value)) {
        cls = asValue(v)->cls;
    }
    return cls != nullptr && cls->ownKeys != 1 ? cls : nullptr;
}

bool keysEqual(const Value& a, const Value& b, int depth) {
    // An object whose class has its own == (D26).
    if (mayOwnKeys(a) && keyMethods != nullptr) {
        if (const ObjClass* cls = keyClassOf(a)) {
            bool same = false;
            if (keyMethods->equalOf(cls, a, b, same)) return same;
        }
    }
    // A plain key looked for with such an object, or found beside one: the object's == says, as
    // `5 == BigInt.of(5)` asks BigInt's (2026-11-19).
    if (mayOwnKeys(b) && !mayOwnKeys(a) && keyMethods != nullptr) {
        if (const ObjClass* cls = keyClassOf(b)) {
            bool same = false;
            if (keyMethods->equalOf(cls, b, a, same)) return same;
        }
    }
    // Values of value types, whatever form each is in (M6.6 part 5).
    if ((a.isSmall() || b.isSmall() || a.isRef() || b.isRef()) && depth < kMaxKeyDepth) {
        const ObjClass *ca = nullptr, *cb = nullptr;
        const uint8_t *pa = nullptr, *pb = nullptr;
        if (!valueForm(a, ca, pa) || !valueForm(b, cb, pb)) return false;
        return ca == cb && valueBytesEqual(*ca->layout.load(std::memory_order_acquire), pa, pb, depth);
    }
    if (!hasContentKey(a) || !hasContentKey(b) || depth >= kMaxKeyDepth) return valuesEqual(a, b);
    // What another core is working on is a key as itself only: its contents are not looked at.
    if (!readableHere(a) || !readableHere(b)) return valuesEqual(a, b);
    if (a.as.obj == b.as.obj) return true;
    if (a.as.obj->type != b.as.obj->type) return false;
    switch (a.as.obj->type) {
        case ObjType::List: {
            const ObjList* x = asList(a);
            const ObjList* y = asList(b);
            if (x->size() != y->size()) return false;
            for (size_t i = 0; i < x->size(); i++)
                if (!keyItemsEqual(keyItem(x, i), keyItem(y, i), depth)) return false;
            return true;
        }
        case ObjType::Range: {
            const ObjRange* x = asRange(a);
            const ObjRange* y = asRange(b);
            return x->start == y->start && x->end == y->end && x->step == y->step && x->inclusive == y->inclusive;
        }
        case ObjType::Bytes:
            return asBytes(a)->data == asBytes(b)->data;
        case ObjType::Map: {
            const ValueMap& x = static_cast<const ObjMap*>(a.as.obj)->map;
            const ValueMap& y = static_cast<const ObjMap*>(b.as.obj)->map;
            if (x.size() != y.size()) return false;
            for (const ValueMap::Entry& entry : x.entries()) {
                if (!entry.live) continue;
                const Value* other = y.find(entry.key);
                if (other == nullptr || !keysEqual(entry.value, *other, depth + 1)) return false;
            }
            return true;
        }
        case ObjType::Set: {
            const ValueMap& x = static_cast<const ObjSet*>(a.as.obj)->items;
            const ValueMap& y = static_cast<const ObjSet*>(b.as.obj)->items;
            if (x.size() != y.size()) return false;
            for (const ValueMap::Entry& entry : x.entries())
                if (entry.live && y.find(entry.key) == nullptr) return false;
            return true;
        }
        case ObjType::Value: {
            const ObjValue* x = asValue(a);
            const ObjValue* y = asValue(b);
            return x->cls == y->cls && valueBytesEqual(*x->cls->layout.load(std::memory_order_acquire), x->data(), y->data(), depth);
        }
        case ObjType::Instance: {
            const ObjInstance* x = asInstance(a);
            const ObjInstance* y = asInstance(b);
            if (x->cls != y->cls) return false;
            for (const ObjString* part : x->cls->components) {
                Value left = Value::null(), right = Value::null();
                x->read(part, left);
                y->read(part, right);
                if (!keysEqual(left, right, depth + 1)) return false;
            }
            return true;
        }
        default:
            return false;
    }
}

size_t keyHash(const Value& v, int depth) {
    // An object whose class has its own hashCode (D26).
    if (mayOwnKeys(v) && keyMethods != nullptr) {
        if (const ObjClass* cls = keyClassOf(v)) {
            size_t own = 0;
            if (keyMethods->hashOf(cls, v, own)) return own;
        }
    }
    if (v.isSmall() || v.isRef()) {
        const ObjClass* cls = nullptr;
        const uint8_t* bytes = nullptr;
        if (!valueForm(v, cls, bytes)) return 0;
        return mixHash(classSeed(cls), valueBytesHash(*cls->layout.load(std::memory_order_acquire), bytes, depth));
    }
    if (!hasContentKey(v) || depth >= kMaxKeyDepth || !readableHere(v)) return hashValue(v);
    auto mix = [](size_t seed, size_t next) { return mixHash(seed, next); };
    switch (v.as.obj->type) {
        case ObjType::List: {
            size_t h = 17;
            const ObjList* list = asList(v);
            for (size_t i = 0; i < list->size(); i++) h = mix(h, keyItemHash(keyItem(list, i), depth));
            return h;
        }
        case ObjType::Range: {
            const ObjRange* r = asRange(v);
            return mix(mix(static_cast<size_t>(r->start), static_cast<size_t>(r->end)), static_cast<size_t>(r->step) + (r->inclusive ? 1 : 0));
        }
        case ObjType::Bytes: {
            size_t h = 31;
            for (uint8_t b : asBytes(v)->data) h = h * 131 + b;
            return h;
        }
        case ObjType::Map:
        case ObjType::Set: {
            // Whatever the order: the entries' hashes added.
            const bool map = v.as.obj->type == ObjType::Map;
            const ValueMap& m = map ? static_cast<const ObjMap*>(v.as.obj)->map : static_cast<const ObjSet*>(v.as.obj)->items;
            size_t h = map ? 0x6d6170 : 0x736574;
            for (const ValueMap::Entry& entry : m.entries())
                if (entry.live) h += mix(keyHash(entry.key, depth + 1), map ? keyHash(entry.value, depth + 1) : 0);
            return h;
        }
        case ObjType::Value: {
            const ObjValue* box = asValue(v);
            return mix(classSeed(box->cls), valueBytesHash(*box->cls->layout.load(std::memory_order_acquire), box->data(), depth));
        }
        case ObjType::Instance: {
            const ObjInstance* instance = asInstance(v);
            size_t h = classSeed(instance->cls);
            for (const ObjString* part : instance->cls->components) {
                Value field;
                h = mix(h, instance->read(part, field) ? keyHash(field, depth + 1) : 0);
            }
            return h;
        }
        default:
            return hashValue(v);
    }
}

bool stableKey(const Value& v) {
    if (v.kind == ValueKind::Small || v.kind == ValueKind::Ref) {
        const ObjClass* cls = nullptr;
        const uint8_t* bytes = nullptr;
        return !valueForm(v, cls, bytes) || !cls->unstableKey;
    }
    if (!v.isObj()) return true;
    switch (v.as.obj->type) {
        case ObjType::List:
        case ObjType::Map:
        case ObjType::Set:
        case ObjType::Bytes: return v.as.obj->frozen == 1;
        case ObjType::Instance: return !asInstance(v)->cls->unstableKey;
        case ObjType::Value: return !asValue(v)->cls->unstableKey;
        default: return true;
    }
}

std::string formatFloat(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, d);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

std::string quoteString(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\0': out += "\\0"; break;
            default: out += c; break;
        }
    }
    out += '"';
    return out;
}

std::string quoteCharacter(uint32_t c) {
    switch (c) {
        case '\'': return "'\\''";
        case '\\': return "'\\\\'";
        case '\n': return "'\\n'";
        case '\t': return "'\\t'";
        case '\r': return "'\\r'";
        case 0: return "'\\0'";
        default: {
            std::string out = "'";
            appendUtf8(out, c);
            return out + "'";
        }
    }
}

// ---- Table ----

std::atomic<uint64_t> tableKeysChanged{0};

void Table::set(ObjString* key, Value value) {
    if ((count_ + 1) * 4 > capacity_ * 3) grow();

    uint32_t mask = capacity_ - 1;
    uint32_t i = key->hash() & mask;
    for (;;) {
        Entry& e = entries_[i];
        if (e.key == key) {
            e.value = value;
            return;
        }
        if (e.key == nullptr) {
            e.key = key;
            e.value = value;
            count_++;
            tableKeysChanged.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        i = (i + 1) & mask;
    }
}

bool Table::remove(const ObjString* key) {
    if (count_ == 0) return false;
    const uint32_t mask = capacity_ - 1;
    uint32_t i = key->hash() & mask;
    for (;;) {
        if (entries_[i].key == nullptr) return false;
        if (entries_[i].key == key) break;
        i = (i + 1) & mask;
    }
    // The entries after it, up to a gap, move back where their own place allows, so that each
    // is still found from its place with no gap on the way (backward-shift deletion).
    uint32_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (entries_[j].key == nullptr) break;
        const uint32_t home = entries_[j].key->hash() & mask;
        const bool staysPut = i <= j ? (i < home && home <= j) : (i < home || home <= j);
        if (!staysPut) {
            entries_[i] = entries_[j];
            i = j;
        }
    }
    entries_[i] = Entry{};
    count_--;
    tableKeysChanged.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Table::grow() {
    uint32_t oldCapacity = capacity_;
    Entry* old = entries_;

    // The new entries first: if there is no memory for them, the table is left as it was.
    Entry* fresh = new Entry[oldCapacity == 0 ? 4 : oldCapacity * 2];
    capacity_ = oldCapacity == 0 ? 4 : oldCapacity * 2;
    entries_ = fresh;
    count_ = 0;

    for (uint32_t i = 0; i < oldCapacity; i++)
        if (old[i].key != nullptr) set(old[i].key, old[i].value);
    delete[] old;
}

void Table::addAll(const Table& other) {
    for (uint32_t i = 0; i < other.capacity_; i++)
        if (other.entries_[i].key != nullptr) set(other.entries_[i].key, other.entries_[i].value);
}

void Table::addMissing(const Table& other) {
    for (uint32_t i = 0; i < other.capacity_; i++)
        if (other.entries_[i].key != nullptr && find(other.entries_[i].key) == nullptr)
            set(other.entries_[i].key, other.entries_[i].value);
}

size_t countCharacters(std::string_view s) {
    size_t count = 0;
    for (unsigned char c : s) count += (c & 0xC0) != 0x80;  // every byte that is not a continuation
    return count;
}

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

namespace {

size_t utf8Size(uint32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }

// The character starting at text[i], moving i past it; a byte that does not start a valid
// one is U+FFFD and moves i by one.
uint32_t decodeUtf8(std::string_view text, size_t& i) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
        i++;
        return lead;
    }
    const size_t size = lead >= 0xF0 && lead <= 0xF4 ? 4 : lead >= 0xE0 ? (lead <= 0xEF ? 3 : 0) : lead >= 0xC2 ? 2 : 0;
    if (size == 0 || i + size > text.size()) {
        i++;
        return 0xFFFD;
    }
    uint32_t c = size == 2 ? lead & 0x1F : size == 3 ? lead & 0x0F : lead & 0x07;
    for (size_t k = 1; k < size; k++) {
        const unsigned char next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) {
            i++;
            return 0xFFFD;
        }
        c = (c << 6) | (next & 0x3F);
    }
    // Too long a spelling, a surrogate, or past the last character.
    if ((size == 3 && c < 0x800) || (size == 4 && c < 0x10000) || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
        i++;
        return 0xFFFD;
    }
    i += size;
    return c;
}

void putUnit(std::string& out, size_t at, uint8_t width, uint32_t c) {
    if (width == 1) {
        out[at] = static_cast<char>(c);
    } else if (width == 2) {
        const uint16_t unit = static_cast<uint16_t>(c);
        std::memcpy(&out[2 * at], &unit, 2);
    } else {
        std::memcpy(&out[4 * at], &c, 4);
    }
}

}  // namespace

namespace {

// Characters of type From written out as type To, at least as wide.
template <typename From, typename To>
void widenUnits(const char* in, size_t count, char* out) {
    for (size_t k = 0; k < count; k++) {
        From c;
        std::memcpy(&c, in + k * sizeof(From), sizeof(From));
        const To wide = c;
        std::memcpy(out + k * sizeof(To), &wide, sizeof(To));
    }
}

// UTF-8 text past its ASCII start, decoded into characters of type Unit.
template <typename Unit>
void decodeInto(std::string_view text, size_t i, char* out) {
    for (size_t k = 0; k < i; k++) {
        const Unit c = static_cast<unsigned char>(text[k]);
        std::memcpy(out + k * sizeof(Unit), &c, sizeof(Unit));
    }
    for (size_t k = i; i < text.size(); k++) {
        const Unit c = static_cast<Unit>(decodeUtf8(text, i));
        std::memcpy(out + k * sizeof(Unit), &c, sizeof(Unit));
    }
}

}  // namespace

// How many of the bytes at the start are ASCII: sixteen at a time, then one at a time.
static size_t asciiPrefix(const char* text, size_t size) {
    size_t i = 0;
    for (; i + 16 <= size; i += 16) {
        uint64_t a, b;
        std::memcpy(&a, text + i, 8);
        std::memcpy(&b, text + i + 8, 8);
        if (((a | b) & 0x8080808080808080ull) != 0) break;
    }
    while (i < size && static_cast<unsigned char>(text[i]) < 0x80) i++;
    return i;
}

Characters Characters::fromUtf8(std::string text) {
    const size_t ascii = asciiPrefix(text.data(), text.size());
    if (ascii == text.size()) return Characters{std::move(text), 1};

    // Once to find how many characters there are and the width they need, once to keep them.
    size_t count = ascii;
    uint32_t widest = 0;
    for (size_t i = ascii; i < text.size(); count++) widest = std::max(widest, decodeUtf8(text, i));
    Characters out;
    out.width = widest < 0x100 ? 1 : widest < 0x10000 ? 2 : 4;
    out.units.resize(count * out.width);
    if (out.width == 1) decodeInto<uint8_t>(text, ascii, out.units.data());
    else if (out.width == 2) decodeInto<uint16_t>(text, ascii, out.units.data());
    else decodeInto<uint32_t>(text, ascii, out.units.data());
    return out;
}

void Characters::widen(const char* in, size_t count, uint8_t from, uint8_t to, std::string& out) {
    if (from == to) {
        out.append(in, count * from);
        return;
    }
    const size_t start = out.size();
    out.resize(start + count * to);
    char* at = out.data() + start;
    if (from == 1 && to == 2) widenUnits<uint8_t, uint16_t>(in, count, at);
    else if (from == 1) widenUnits<uint8_t, uint32_t>(in, count, at);
    else widenUnits<uint16_t, uint32_t>(in, count, at);
}

void Characters::appendUtf8Text(std::string_view text) {
    if (asciiPrefix(text.data(), text.size()) == text.size()) {
        append(text.data(), text.size(), 1);
        return;
    }
    append(fromUtf8(std::string(text)));
}

namespace {

// The hash of `length` characters of type Unit.
template <typename Unit>
uint32_t hashUnits(const char* data, size_t length) {
    uint32_t hash = kEmptyStringHash;
    for (size_t i = 0; i < length; i++) {
        Unit c;
        std::memcpy(&c, data + i * sizeof(Unit), sizeof(Unit));
        hash = hashCharacter(hash, c);
    }
    return hash;
}

// The widest of `length` characters of type Unit; for bytes, all their bits together, which is
// below 128 when all of them are. No hash in the same loop, so the compiler takes many at once.
template <typename Unit>
uint32_t widestUnit(const char* data, size_t length) {
    const Unit* units = reinterpret_cast<const Unit*>(data);
    uint32_t widest = 0;
    if constexpr (sizeof(Unit) == 1) {
        for (size_t i = 0; i < length; i++) widest |= units[i];
    } else {
        for (size_t i = 0; i < length; i++) widest = std::max<uint32_t>(widest, units[i]);
    }
    return widest;
}

}  // namespace

ObjString::Made ObjString::make(Characters characters) {
    const size_t length = characters.size();
    const char* data = characters.units.data();
    const uint32_t widest = characters.width == 1   ? widestUnit<uint8_t>(data, length)
                            : characters.width == 2 ? widestUnit<uint16_t>(data, length)
                                                    : widestUnit<uint32_t>(data, length);
    const uint8_t width = widest < 0x100 ? 1 : widest < 0x10000 ? 2 : 4;
    if (width < characters.width) {
        // Kept at the narrowest width that holds it, so that equal strings are kept alike.
        std::string narrow(length * width, '\0');
        for (size_t i = 0; i < length; i++) putUnit(narrow, i, width, characters[i]);
        characters.units = std::move(narrow);
    }
    return Made{std::move(characters.units), width, widest < 0x80, length};
}

ObjString::Made ObjString::join(const ObjString* first, const ObjString* second) {
    const uint8_t width = std::max(first->width, second->width);
    std::string units;
    units.reserve((first->length + second->length) * width);
    Characters::widen(first->units().data(), first->length, first->width, width, units);
    Characters::widen(second->units().data(), second->length, second->width, width, units);
    return Made{std::move(units), width, first->ascii && second->ascii, first->length + second->length};
}

ObjString::ObjString(Made made)
    : Obj(ObjType::String), width(made.width), ascii(made.ascii), length(made.length), units_(std::move(made.units)) {}

uint32_t ObjString::makeHash() const {
    const char* data = units_.data();
    const uint32_t hash = width == 1 ? hashUnits<uint8_t>(data, length) : width == 2 ? hashUnits<uint16_t>(data, length) : hashUnits<uint32_t>(data, length);
    hash_.store((uint64_t{1} << 32) | hash, std::memory_order_relaxed);
    return hash;
}

const std::string& ObjString::makeUtf8() const {
    auto text = std::make_unique<std::string>();
    text->reserve(length * 2);
    for (size_t i = 0; i < length; i++) appendUtf8(*text, at(i));
    const std::string* expected = nullptr;
    if (utf8_.compare_exchange_strong(expected, text.get(), std::memory_order_acq_rel)) return *text.release();
    return *expected;  // another core made it first
}

size_t ObjString::utf8Offset(size_t index) const {
    if (ascii) return index;
    size_t offset = 0;
    for (size_t i = 0; i < index && i < length; i++) offset += utf8Size(at(i));
    return offset;
}

int64_t ObjString::find(const ObjString* part, size_t from, bool last) const {
    // A character of `part` too wide for this string's width is not in it.
    if (part->width > width || part->length > length) return -1;
    const size_t m = part->length;
    std::string widened;
    const std::string* needle = &part->units();
    if (part->width != width) {
        Characters::widen(part->units().data(), m, part->width, width, widened);
        needle = &widened;
    }
    const size_t bytes = m * width;
    auto matches = [&](size_t i) { return std::memcmp(units_.data() + i * width, needle->data(), bytes) == 0; };
    if (last && width == 1) {
        const size_t at = std::string_view(units_).rfind(*needle, from);
        return at == std::string_view::npos ? -1 : static_cast<int64_t>(at);
    }
    if (last) {
        for (size_t i = std::min(from, length - m) + 1; i-- > 0;)
            if (matches(i)) return static_cast<int64_t>(i);
        return -1;
    }
    if (width == 1) {
        const size_t at = std::string_view(units_).find(*needle, from);
        return at == std::string_view::npos ? -1 : static_cast<int64_t>(at);
    }
    for (size_t i = from; i + m <= length; i++)
        if (matches(i)) return static_cast<int64_t>(i);
    return -1;
}

int64_t ObjString::findChar(uint32_t c, size_t from, bool last) const {
    // A string is kept at the narrowest width that holds its characters: one wider is not in it.
    if (length == 0 || (width == 1 && c > 0xFF) || (width == 2 && c > 0xFFFF)) return -1;
    const char* const u = units_.data();
    auto at = [&](size_t i) -> uint32_t {
        if (width == 1) return static_cast<unsigned char>(u[i]);
        if (width == 2) {
            uint16_t unit;
            std::memcpy(&unit, u + 2 * i, 2);
            return unit;
        }
        uint32_t unit;
        std::memcpy(&unit, u + 4 * i, 4);
        return unit;
    };
    if (last) {
        for (size_t i = std::min(from, length - 1) + 1; i-- > 0;)
            if (at(i) == c) return static_cast<int64_t>(i);
        return -1;
    }
    if (from >= length) return -1;
    if (width == 1) {
        const void* found = std::memchr(u + from, static_cast<int>(c), length - from);
        return found == nullptr ? -1 : static_cast<const char*>(found) - u;
    }
    for (size_t i = from; i < length; i++)
        if (at(i) == c) return static_cast<int64_t>(i);
    return -1;
}

namespace {

template <typename A, typename B>
int compareUnits(const ObjString* a, const ObjString* b) {
    const size_t common = std::min(a->length, b->length);
    const char* x = a->units().data();
    const char* y = b->units().data();
    for (size_t i = 0; i < common; i++) {
        A p;
        B q;
        std::memcpy(&p, x + i * sizeof(A), sizeof(A));
        std::memcpy(&q, y + i * sizeof(B), sizeof(B));
        if (p != q) return static_cast<uint32_t>(p) < static_cast<uint32_t>(q) ? -1 : 1;
    }
    return a->length < b->length ? -1 : a->length > b->length;
}

template <typename A>
int compareWith(const ObjString* a, const ObjString* b) {
    return b->width == 1 ? compareUnits<A, uint8_t>(a, b) : b->width == 2 ? compareUnits<A, uint16_t>(a, b) : compareUnits<A, uint32_t>(a, b);
}

// The widest of characters `begin` to `end` of type Unit.
template <typename Unit>
uint32_t widestUnit(const char* data, size_t begin, size_t end) {
    uint32_t widest = 0;
    for (size_t i = begin; i < end; i++) {
        Unit c;
        std::memcpy(&c, data + i * sizeof(Unit), sizeof(Unit));
        widest = std::max<uint32_t>(widest, c);
    }
    return widest;
}

}  // namespace

int compareText(const ObjString* a, const ObjString* b) {
    if (a->width == 1 && b->width == 1) {
        const int order = a->units().compare(b->units());
        return order < 0 ? -1 : order > 0;
    }
    return a->width == 1 ? compareWith<uint8_t>(a, b) : a->width == 2 ? compareWith<uint16_t>(a, b) : compareWith<uint32_t>(a, b);
}

Characters ObjString::slice(size_t begin, size_t end) const {
    if (width == 1) return Characters{units_.substr(begin, end - begin), 1};
    const uint32_t widest = width == 2 ? widestUnit<uint16_t>(units_.data(), begin, end) : widestUnit<uint32_t>(units_.data(), begin, end);
    const uint8_t to = widest < 0x100 ? 1 : widest < 0x10000 ? 2 : 4;
    if (to == width) return Characters{units_.substr(begin * width, (end - begin) * width), width};
    Characters out{std::string((end - begin) * to, '\0'), to};
    for (size_t i = begin; i < end; i++) putUnit(out.units, i - begin, to, at(i));
    return out;
}

// ---- objectMemory ----

namespace objectMemory {

#if defined(__SANITIZE_ADDRESS__)
#define WW_OBJECT_HEAP 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WW_OBJECT_HEAP 1
#endif
#endif

namespace {

constexpr size_t kLargest = 512;           // blocks up to this many bytes come from the lists
constexpr size_t kClasses = kLargest / 16 + 1;
constexpr size_t kChunk = 256 * 1024;      // the memory new blocks are cut from, taken at a time

struct FreeBlock {
    FreeBlock* next;
};

// A thread's lists, and what is left of its chunk. Never freed: when its thread ends it is
// handed to the next thread that wants one, with all that is on it. Made with malloc, as is
// all the pool's own memory: every `new` in the program comes here, so the pool may not use one.
struct Pool {
    FreeBlock* free[kClasses] = {};
    uint8_t* cut = nullptr;
    uint8_t* cutEnd = nullptr;
    Pool* nextOrphan = nullptr;
};

// The pools of threads that have ended, behind a lock that needs no constructor: `new` may
// be called before any of the program's statics are made.
std::atomic_flag orphansLock = ATOMIC_FLAG_INIT;
Pool* orphans = nullptr;

struct OrphansHeld {
    OrphansHeld() {
        while (orphansLock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~OrphansHeld() { orphansLock.clear(std::memory_order_release); }
};

thread_local Pool* pool = nullptr;  // trivial, so that it may be read at any time, even at exit

// Hands this thread's pool on when the thread ends.
struct PoolRelease {
    ~PoolRelease() {
        if (pool == nullptr) return;
        OrphansHeld hold;
        pool->nextOrphan = orphans;
        orphans = pool;
        pool = nullptr;
    }
};
thread_local PoolRelease poolRelease;

Pool& ownPool() {
    if (pool != nullptr) return *pool;
    {
        OrphansHeld hold;
        if (orphans != nullptr) {
            pool = orphans;
            orphans = orphans->nextOrphan;
            pool->nextOrphan = nullptr;
        }
    }
    if (pool == nullptr) {
        void* memory = std::malloc(sizeof(Pool));
        if (memory == nullptr) throw std::bad_alloc();
        pool = ::new (memory) Pool();
    }
    (void)&poolRelease;  // made on this thread, so that its pool is handed on when the thread ends
    return *pool;
}

}  // namespace

void* take(size_t size) {
#ifdef WW_OBJECT_HEAP
    return ::operator new(size);
#else
    if (size > kLargest) {
        void* large = std::malloc(size);
        if (large == nullptr) throw std::bad_alloc();
        return large;
    }
    const size_t k = (size + 15) >> 4;
    Pool& p = ownPool();
    if (FreeBlock* block = p.free[k]) {
        p.free[k] = block->next;
        return block;
    }
    const size_t bytes = k << 4;
    if (static_cast<size_t>(p.cutEnd - p.cut) < bytes) {
        // What is left of the old chunk goes on the lists, a block of 16 at a time.
        while (p.cutEnd - p.cut >= 16) {
            auto* rest = reinterpret_cast<FreeBlock*>(p.cut);
            rest->next = p.free[1];
            p.free[1] = rest;
            p.cut += 16;
        }
        auto* chunk = static_cast<uint8_t*>(std::malloc(kChunk));  // 16-byte aligned, as malloc's are
        if (chunk == nullptr) throw std::bad_alloc();
        p.cut = chunk;
        p.cutEnd = chunk + kChunk;
    }
    void* made = p.cut;
    p.cut += bytes;
    return made;
#endif
}

void give(void* p, size_t size) noexcept {
    if (p == nullptr) return;
#ifdef WW_OBJECT_HEAP
    (void)size;
    ::operator delete(p);
#else
    if (size > kLargest) {
        std::free(p);
        return;
    }
    const size_t k = (size + 15) >> 4;
    Pool& own = ownPool();
    auto* block = static_cast<FreeBlock*>(p);
    block->next = own.free[k];
    own.free[k] = block;
#endif
}

void* takeSized(size_t size) {
    auto* block = static_cast<uint8_t*>(take(size + 16));
    std::memcpy(block, &size, sizeof size);
    return block + 16;
}

void giveSized(void* p) noexcept {
    if (p == nullptr) return;
    uint8_t* block = static_cast<uint8_t*>(p) - 16;
    size_t size;
    std::memcpy(&size, block, sizeof size);
    give(block, size + 16);
}

}  // namespace objectMemory

thread_local uint8_t currentCore = 0;
thread_local const std::bitset<256>* currentReadable = nullptr;
thread_local KeyMethods* keyMethods = nullptr;

// ---- ValueMap ----

// The key's hash, its 64 bits mixed into 32 (Fibonacci hashing): the top bits pick the first
// slot, so a run of ints, or hashes that differ only high up, spread over the index.
uint32_t ValueMap::hashOf(const Value& key) {
    const uint64_t h = keyHash(key);
    return static_cast<uint32_t>((h * 0x9E3779B97F4A7C15ULL) >> 32);
}

uint32_t ValueMap::shiftFor(size_t capacity) {
    uint32_t bits = 0;
    while ((size_t{1} << bits) < capacity) bits++;
    return 32 - bits;
}

std::vector<uint32_t> ValueMap::indexFor(const std::vector<Entry>& entries, size_t capacity, uint32_t shift) {
    std::vector<uint32_t> slots(capacity, 0);
    const size_t mask = capacity - 1;
    for (size_t i = 0; i < entries.size(); i++) {
        if (!entries[i].live) continue;
        size_t at = entries[i].hash >> shift;
        while (slots[at] != 0) at = (at + 1) & mask;
        slots[at] = static_cast<uint32_t>(i + 1);
    }
    return slots;
}

int64_t ValueMap::lookup(const Value& key, uint32_t hash) const {
    // A class's own == may change this map as it runs (D26 lets it read only what cannot change,
    // but a global map is reachable); if it did, the search starts again, as Python's does.
    for (;;) {
        const uint64_t version = version_;
        const size_t mask = slots_.size() - 1;
        bool changed = false;
        for (size_t at = firstSlot(hash);; at = (at + 1) & mask) {
            const uint32_t slot = slots_[at];
            if (slot == 0) return -1;
            if (slot == kGone || entries_[slot - 1].hash != hash) continue;
            const Value stored = entries_[slot - 1].key;  // a copy: == may move the entries
            const bool same = keysEqual(stored, key);
            if (version_ != version) {
                changed = true;
                break;
            }
            if (same) return static_cast<int64_t>(at);
        }
        if (!changed || slots_.empty()) return -1;
    }
}

const Value* ValueMap::find(const Value& key, uint32_t hash) const {
    if (live_ == 0) return nullptr;
    const int64_t at = lookup(key, hash);
    if (at < 0) return nullptr;
    return &entries_[slots_[static_cast<size_t>(at)] - 1].value;
}

int ValueMap::findPlain(const Value& key, const Value*& out) const {
    if (live_ == 0) return 0;
    const uint32_t hash = hashOf(key);
    const size_t mask = slots_.size() - 1;
    for (size_t at = firstSlot(hash);; at = (at + 1) & mask) {
        const uint32_t slot = slots_[at];
        if (slot == 0) return 0;
        if (slot == kGone || entries_[slot - 1].hash != hash) continue;
        const Value& stored = entries_[slot - 1].key;
        if (mayOwnKeys(stored)) return -1;
        if (keysEqual(stored, key, 0)) {
            out = &entries_[slot - 1].value;
            return 1;
        }
    }
}

void ValueMap::set(const Value& key, const Value& value) {
    const uint32_t hash = hashOf(key);
    if (live_ != 0) {
        const int64_t at = lookup(key, hash);
        if (at >= 0) {
            entries_[slots_[static_cast<size_t>(at)] - 1].value = value;
            return;
        }
    }
    addNew(key, value, hash);
}

void ValueMap::addNew(const Value& key, const Value& value, uint32_t hash) {
    // A new key, which must not be able to change (D26).
    if (!stableKey(key) && keyMethods != nullptr) keyMethods->refuseKey(key);
    // Room in the index first, at most three slots in four used: a new index of twice the live
    // entries, at least 8 slots, built from the hashes kept. Then the entry. Should either
    // allocation fail, the map is as it was.
    if (size_t{used_ + 1} * 4 > slots_.size() * 3) {
        size_t capacity = 8;
        while (capacity < size_t{live_ + 1} * 2) capacity *= 2;
        const uint32_t shift = shiftFor(capacity);
        std::vector<uint32_t> slots = indexFor(entries_, capacity, shift);
        slots_ = std::move(slots);
        shift_ = shift;
        used_ = live_;
        version_++;
        // Room for as many entries as the index takes before it grows again: one allocation
        // where the entries growing a step at a time took three for a small map.
        entries_.reserve(capacity * 3 / 4);
    }
    entries_.push_back(Entry{key, value, true, hash});
    const size_t mask = slots_.size() - 1;
    size_t at = firstSlot(hash);
    while (slots_[at] != 0 && slots_[at] != kGone) at = (at + 1) & mask;
    if (slots_[at] == 0) used_++;
    slots_[at] = static_cast<uint32_t>(entries_.size());
    live_++;
    version_++;
}

bool ValueMap::remove(const Value& key, Value* removed) {
    if (live_ == 0) return false;
    const int64_t at = lookup(key, hashOf(key));
    if (at < 0) return false;
    if (removed != nullptr) *removed = entries_[slots_[static_cast<size_t>(at)] - 1].value;

    entries_[slots_[static_cast<size_t>(at)] - 1] = Entry{};
    slots_[static_cast<size_t>(at)] = kGone;
    live_--;
    version_++;

    // Squeeze out removed entries once they outnumber the live ones, the index rebuilt from the
    // hashes kept. Built aside and then swapped in; without the memory to build them, the map
    // simply stays as it is.
    if (entries_.size() > 32 && entries_.size() > size_t{live_} * 2) {
        try {
            std::vector<Entry> compacted;
            compacted.reserve(live_);
            for (const Entry& e : entries_)
                if (e.live) compacted.push_back(e);
            std::vector<uint32_t> slots = indexFor(compacted, slots_.size(), shift_);
            entries_ = std::move(compacted);
            slots_ = std::move(slots);
            used_ = live_;
        } catch (const std::bad_alloc&) {
        }
    }
    return true;
}

void ValueMap::clear() {
    entries_.clear();
    slots_.clear();
    live_ = 0;
    used_ = 0;
    shift_ = 32;
    version_++;
}

// ---- ObjRange ----

int64_t ObjRange::count() const { return rangeCount(start, end, step, inclusive); }

int64_t rangeCount(int64_t start, int64_t end, int64_t step, bool inclusive) {
    // Unsigned arithmetic: the span of a range can exceed INT64_MAX.
    uint64_t span;
    if (step > 0) {
        if (end < start) return 0;
        span = static_cast<uint64_t>(end) - static_cast<uint64_t>(start);
    } else {
        if (end > start) return 0;
        span = static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
    }
    uint64_t magnitude = step > 0 ? static_cast<uint64_t>(step) : 0 - static_cast<uint64_t>(step);
    uint64_t n = inclusive ? span / magnitude + 1 : (span + magnitude - 1) / magnitude;
    return n > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(n);
}

bool ObjRange::contains(int64_t i) const {
    if (step > 0) {
        if (i < start || (inclusive ? i > end : i >= end)) return false;
    } else {
        if (i > start || (inclusive ? i < end : i <= end)) return false;
    }
    uint64_t offset = step > 0 ? static_cast<uint64_t>(i) - static_cast<uint64_t>(start)
                               : static_cast<uint64_t>(start) - static_cast<uint64_t>(i);
    uint64_t magnitude = step > 0 ? static_cast<uint64_t>(step) : 0 - static_cast<uint64_t>(step);
    return offset % magnitude == 0;
}

// ---- Large blocks of list items, kept for reuse (value.h, ItemAllocator) ----

namespace {
constexpr size_t kKeptFrom = size_t{1} << 20;     // a block this big or bigger is kept
constexpr size_t kKeptAtMost = size_t{256} << 20;  // in all
struct KeptBlocks {
    std::mutex lock;
    std::unordered_map<void*, size_t> live;     // each large block a list has: its true size
    std::vector<std::pair<void*, size_t>> kept;  // those kept, oldest first
    size_t keptBytes = 0;
};
KeptBlocks& keptBlocks() {
    static KeptBlocks* made = new KeptBlocks();  // never destroyed: lists may be freed at exit
    return *made;
}
}  // namespace

void* takeItemBlock(size_t bytes, bool zeroed) {
    if (bytes < kKeptFrom) {
        void* p = ::operator new(bytes);
        if (zeroed) std::memset(p, 0, bytes);
        return p;
    }
    KeptBlocks& k = keptBlocks();
    {
        std::lock_guard<std::mutex> hold(k.lock);
        // The smallest kept that holds it, at most twice its size.
        size_t best = k.kept.size();
        for (size_t i = 0; i < k.kept.size(); i++)
            if (k.kept[i].second >= bytes && k.kept[i].second / 2 <= bytes && (best == k.kept.size() || k.kept[i].second < k.kept[best].second)) best = i;
        if (best != k.kept.size()) {
            const auto [p, size] = k.kept[best];
            k.kept.erase(k.kept.begin() + static_cast<std::ptrdiff_t>(best));
            k.keptBytes -= size;
            k.live[p] = size;
            if (zeroed) std::memset(p, 0, bytes);
            return p;
        }
    }
    void* p = zeroed ? std::calloc(bytes, 1) : std::malloc(bytes);
    if (p == nullptr) {
        // Memory short: what is kept goes back, then once more.
        releaseItemBlocks();
        p = zeroed ? std::calloc(bytes, 1) : std::malloc(bytes);
        if (p == nullptr) throw std::bad_alloc();
    }
    std::lock_guard<std::mutex> hold(k.lock);
    k.live[p] = bytes;
    return p;
}

void giveItemBlock(void* p, size_t bytes) noexcept {
    if (p == nullptr) return;
    if (bytes < kKeptFrom) {
        ::operator delete(p);
        return;
    }
    KeptBlocks& k = keptBlocks();
    std::lock_guard<std::mutex> hold(k.lock);
    auto at = k.live.find(p);
    const size_t size = at != k.live.end() ? at->second : bytes;
    if (at != k.live.end()) k.live.erase(at);
    bool kept = false;
    if (size <= kKeptAtMost) {
        try {
            k.kept.emplace_back(p, size);
            k.keptBytes += size;
            kept = true;
        } catch (...) {
        }
    }
    if (!kept) std::free(p);
    // The oldest given back, until what is kept is within its limit.
    size_t drop = 0;
    while (k.keptBytes > kKeptAtMost && drop < k.kept.size()) {
        k.keptBytes -= k.kept[drop].second;
        std::free(k.kept[drop++].first);
    }
    k.kept.erase(k.kept.begin(), k.kept.begin() + static_cast<std::ptrdiff_t>(drop));
}

void releaseItemBlocks() noexcept {
    KeptBlocks& k = keptBlocks();
    std::vector<std::pair<void*, size_t>> freed;
    {
        std::lock_guard<std::mutex> hold(k.lock);
        freed.swap(k.kept);
        k.keptBytes = 0;
    }
    for (const auto& [p, size] : freed) std::free(p);
}

size_t keptItemBytes() noexcept {
    KeptBlocks& k = keptBlocks();
    std::lock_guard<std::mutex> hold(k.lock);
    return k.keptBytes;
}

}  // namespace ww

#ifndef WW_OBJECT_HEAP
// Every other `new` in the program, the standard library's included (the C++ runtime is linked
// in, not shared): the buffers of strings, vectors and maps from the same lists as objects,
// with their size kept in front of them, as an unsized delete needs it.
void* operator new(size_t size) { return ww::objectMemory::takeSized(size); }
void* operator new[](size_t size) { return ww::objectMemory::takeSized(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    try {
        return ww::objectMemory::takeSized(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    try {
        return ww::objectMemory::takeSized(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { ww::objectMemory::giveSized(p); }
void operator delete[](void* p) noexcept { ww::objectMemory::giveSized(p); }
void operator delete(void* p, size_t) noexcept { ww::objectMemory::giveSized(p); }
void operator delete[](void* p, size_t) noexcept { ww::objectMemory::giveSized(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ww::objectMemory::giveSized(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ww::objectMemory::giveSized(p); }
#endif
