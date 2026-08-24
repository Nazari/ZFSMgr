#pragma once

#include <string>
#include <utility>
#include <vector>

// JSON without Qt and without external dependencies.
//
// It exists so that `ConnectionStore` could be pulled out of Qt without changing the format
// of the files already sitting on people's machines: `config.json` and `trust-store.json`
// were written by `QJsonDocument` and must go on being read and written THE SAME.
//
// That is why the serialisation imitates `QJsonDocument::Indented` down to its oddities:
// four spaces per level, sorted keys, a trailing newline and —this is the surprising one— an
// empty array written as «[\n        ]», not as «[]». Reproducing it matters: otherwise every
// save would rewrite the whole file and dirty the backups without anything having changed.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::json {

class Value;

// The object is an ORDERED vector of pairs, not a map. Two reasons: a
// `std::map<std::string, Value>` with `Value` still incomplete is not guaranteed by the
// standard, and this way the order of the keys —which is part of the output format— is
// explicit instead of depending on a container's comparator.
using Object = std::vector<std::pair<std::string, Value>>;
using Array = std::vector<Value>;

class Value {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Value() = default;
    explicit Value(bool v) : m_type(Type::Bool), m_bool(v) {}
    explicit Value(long long v) : m_type(Type::Int), m_int(v) {}
    explicit Value(int v) : m_type(Type::Int), m_int(v) {}
    explicit Value(double v) : m_type(Type::Double), m_double(v) {}
    explicit Value(std::string v) : m_type(Type::String), m_string(std::move(v)) {}
    explicit Value(const char* v) : m_type(Type::String), m_string(v) {}
    explicit Value(Array v) : m_type(Type::Array), m_array(std::move(v)) {}
    explicit Value(Object v) : m_type(Type::Object), m_object(std::move(v)) {}

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isObject() const { return m_type == Type::Object; }
    bool isArray() const { return m_type == Type::Array; }
    bool isString() const { return m_type == Type::String; }
    // Int and Double are the SAME type in JSON. They are told apart internally so that
    // «47653» can be written and not «47653.0», which is what Qt does and what whoever reads
    // the file by hand expects.
    bool isNumber() const { return m_type == Type::Int || m_type == Type::Double; }
    bool isBool() const { return m_type == Type::Bool; }

    // Forgiving accessors: they return the default value when the type is not the expected
    // one, exactly as the Qt layer did. A configuration file edited by hand must not bring
    // the application down.
    bool toBool(bool porOmision = false) const;
    long long toInt(long long porOmision = 0) const;
    double toDouble(double porOmision = 0.0) const;
    std::string toString(const std::string& porOmision = {}) const;
    const Array& toArray() const;
    const Object& toObject() const;

    // Lookup by key in an object. Returns a null Value when it is not there.
    const Value& operator[](const std::string& key) const;
    bool contains(const std::string& key) const;

    // Inserta o sustituye, MANTENIENDO EL ORDEN por clave.
    void set(const std::string& key, Value v);
    void remove(const std::string& key);
    void push(Value v);

private:
    Type m_type{Type::Null};
    bool m_bool{false};
    long long m_int{0};
    double m_double{0.0};
    std::string m_string;
    Array m_array;
    Object m_object;
};

// Analiza. Devuelve false y describe el fallo en `error` si lo hay.
//
// It accepts exactly JSON: no trailing commas, no comments, no single quotes. A corrupt file
// must fail here rather than produce a half-formed configuration.
bool parse(const std::string& text, Value& out, std::string* error = nullptr);

// Like QJsonDocument::Indented, trailing newline included.
std::string toIndented(const Value& v);
// Sin espacios ni saltos. Para lo que viaja por el cable, no para los ficheros.
std::string toCompact(const Value& v);

}  // namespace zfsmgr::base::json
