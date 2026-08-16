#pragma once

#include <string>
#include <utility>
#include <vector>

// JSON sin Qt y sin dependencias externas.
//
// Existe para poder sacar `ConnectionStore` de Qt sin cambiar el formato de los ficheros
// que ya están en las máquinas: `config.json` y `trust-store.json` los escribió
// `QJsonDocument` y deben seguir leyéndose y escribiéndose IGUAL.
//
// Por eso la serialización imita a `QJsonDocument::Indented` hasta en sus rarezas:
// cuatro espacios por nivel, claves ordenadas, salto de línea final y —esta es la que
// sorprende— un array vacío escrito como «[\n        ]», no como «[]». Reproducirlo
// importa: si no, cada guardado reescribiría el fichero entero y ensuciaría las copias
// de seguridad sin que haya cambiado nada.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::json {

class Value;

// El objeto es un vector ORDENADO de pares, no un mapa. Dos motivos: un
// `std::map<std::string, Value>` con `Value` todavía incompleto no está garantizado por
// el estándar, y así el orden de las claves —que es parte del formato de salida— queda
// explícito en vez de depender del comparador de un contenedor.
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
    // Int y Double son el MISMO tipo en JSON. Se distinguen dentro para poder escribir
    // «47653» y no «47653.0», que es lo que hace Qt y lo que espera quien lea el fichero
    // a mano.
    bool isNumber() const { return m_type == Type::Int || m_type == Type::Double; }
    bool isBool() const { return m_type == Type::Bool; }

    // Accesos tolerantes: devuelven el valor por omisión si el tipo no es el esperado,
    // igual que hacía la capa de Qt. Un fichero de configuración editado a mano no debe
    // hacer caer la aplicación.
    bool toBool(bool porOmision = false) const;
    long long toInt(long long porOmision = 0) const;
    double toDouble(double porOmision = 0.0) const;
    std::string toString(const std::string& porOmision = {}) const;
    const Array& toArray() const;
    const Object& toObject() const;

    // Búsqueda por clave en un objeto. Devuelve un Value nulo si no está.
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
// Acepta exactamente JSON: sin comas de más, sin comentarios, sin comillas simples. Un
// fichero corrupto debe fallar aquí y no producir una configuración a medias.
bool parse(const std::string& text, Value& out, std::string* error = nullptr);

// Como QJsonDocument::Indented, incluido el salto de línea final.
std::string toIndented(const Value& v);
// Sin espacios ni saltos. Para lo que viaja por el cable, no para los ficheros.
std::string toCompact(const Value& v);

}  // namespace zfsmgr::base::json
