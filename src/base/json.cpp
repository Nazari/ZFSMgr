#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace zfsmgr::base::json {
namespace {

const Value& valorNulo() {
    static const Value v;
    return v;
}

const Array& arrayVacio() {
    static const Array a;
    return a;
}

const Object& objetoVacio() {
    static const Object o;
    return o;
}

void escribeUtf8(unsigned cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// --- Análisis

class Parser {
public:
    explicit Parser(const std::string& t) : m_t(t) {}

    bool run(Value& out, std::string& err) {
        saltaBlancos();
        if (!valor(out, err)) {
            return false;
        }
        saltaBlancos();
        if (m_i != m_t.size()) {
            err = "sobra texto tras el valor JSON";
            return false;
        }
        return true;
    }

private:
    const std::string& m_t;
    std::size_t m_i{0};

    void saltaBlancos() {
        while (m_i < m_t.size()
               && (m_t[m_i] == ' ' || m_t[m_i] == '\t' || m_t[m_i] == '\n' || m_t[m_i] == '\r')) {
            ++m_i;
        }
    }

    bool literal(const char* txt) {
        const std::size_t n = std::char_traits<char>::length(txt);
        if (m_t.compare(m_i, n, txt) != 0) {
            return false;
        }
        m_i += n;
        return true;
    }

    bool cadena(std::string& out, std::string& err) {
        if (m_i >= m_t.size() || m_t[m_i] != '"') {
            err = "se esperaba una cadena";
            return false;
        }
        ++m_i;
        out.clear();
        while (m_i < m_t.size()) {
            const unsigned char c = static_cast<unsigned char>(m_t[m_i]);
            if (c == '"') {
                ++m_i;
                return true;
            }
            if (c < 0x20) {
                err = "carácter de control sin escapar dentro de una cadena";
                return false;
            }
            if (c != '\\') {
                out.push_back(m_t[m_i]);
                ++m_i;
                continue;
            }
            ++m_i;
            if (m_i >= m_t.size()) {
                err = "escape a medias al final del texto";
                return false;
            }
            const char e = m_t[m_i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp, err)) {
                        return false;
                    }
                    // Pares suplentes: un carácter fuera del plano básico se escribe
                    // como DOS escapes \u, y unirlos es cosa de quien analiza.
                    if (cp >= 0xD800 && cp <= 0xDBFF && m_i + 1 < m_t.size() && m_t[m_i] == '\\'
                        && m_t[m_i + 1] == 'u') {
                        const std::size_t guarda = m_i;
                        m_i += 2;
                        unsigned bajo = 0;
                        if (hex4(bajo, err) && bajo >= 0xDC00 && bajo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (bajo - 0xDC00);
                        } else {
                            m_i = guarda;  // no era un par: se deja el alto tal cual
                        }
                    }
                    escribeUtf8(cp, out);
                    break;
                }
                default:
                    err = std::string("escape desconocido: \\") + e;
                    return false;
            }
        }
        err = "cadena sin cerrar";
        return false;
    }

    bool hex4(unsigned& out, std::string& err) {
        if (m_i + 4 > m_t.size()) {
            err = "escape \\u incompleto";
            return false;
        }
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = m_t[m_i + static_cast<std::size_t>(k)];
            unsigned d = 0;
            if (c >= '0' && c <= '9') { d = static_cast<unsigned>(c - '0'); }
            else if (c >= 'a' && c <= 'f') { d = static_cast<unsigned>(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { d = static_cast<unsigned>(c - 'A' + 10); }
            else {
                err = "dígito no hexadecimal en un escape \\u";
                return false;
            }
            out = (out << 4) | d;
        }
        m_i += 4;
        return true;
    }

    bool numero(Value& out, std::string& err) {
        const std::size_t ini = m_i;
        if (m_i < m_t.size() && m_t[m_i] == '-') {
            ++m_i;
        }
        if (m_i >= m_t.size() || !std::isdigit(static_cast<unsigned char>(m_t[m_i]))) {
            err = "número mal formado";
            return false;
        }
        // JSON no admite ceros a la izquierda.
        if (m_t[m_i] == '0') {
            ++m_i;
        } else {
            while (m_i < m_t.size() && std::isdigit(static_cast<unsigned char>(m_t[m_i]))) {
                ++m_i;
            }
        }
        bool esEntero = true;
        if (m_i < m_t.size() && m_t[m_i] == '.') {
            esEntero = false;
            ++m_i;
            if (m_i >= m_t.size() || !std::isdigit(static_cast<unsigned char>(m_t[m_i]))) {
                err = "faltan dígitos tras el punto decimal";
                return false;
            }
            while (m_i < m_t.size() && std::isdigit(static_cast<unsigned char>(m_t[m_i]))) {
                ++m_i;
            }
        }
        if (m_i < m_t.size() && (m_t[m_i] == 'e' || m_t[m_i] == 'E')) {
            esEntero = false;
            ++m_i;
            if (m_i < m_t.size() && (m_t[m_i] == '+' || m_t[m_i] == '-')) {
                ++m_i;
            }
            if (m_i >= m_t.size() || !std::isdigit(static_cast<unsigned char>(m_t[m_i]))) {
                err = "faltan dígitos en el exponente";
                return false;
            }
            while (m_i < m_t.size() && std::isdigit(static_cast<unsigned char>(m_t[m_i]))) {
                ++m_i;
            }
        }
        const std::string txt = m_t.substr(ini, m_i - ini);
        if (esEntero) {
            errno = 0;
            char* fin = nullptr;
            const long long v = std::strtoll(txt.c_str(), &fin, 10);
            // Un entero que no cabe en 64 bits se guarda como decimal, que es lo que
            // hace Qt: prefiere perder precisión antes que rechazar el fichero.
            if (errno == 0 && fin && *fin == '\0') {
                out = Value(v);
                return true;
            }
        }
        out = Value(std::strtod(txt.c_str(), nullptr));
        return true;
    }

    bool valor(Value& out, std::string& err) {
        if (m_i >= m_t.size()) {
            err = "texto JSON vacío";
            return false;
        }
        const char c = m_t[m_i];
        if (c == '{') { return objeto(out, err); }
        if (c == '[') { return array(out, err); }
        if (c == '"') {
            std::string s;
            if (!cadena(s, err)) {
                return false;
            }
            out = Value(std::move(s));
            return true;
        }
        if (c == 't') {
            if (!literal("true")) { err = "se esperaba true"; return false; }
            out = Value(true);
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) { err = "se esperaba false"; return false; }
            out = Value(false);
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) { err = "se esperaba null"; return false; }
            out = Value();
            return true;
        }
        return numero(out, err);
    }

    bool objeto(Value& out, std::string& err) {
        ++m_i;  // '{'
        Object obj;
        saltaBlancos();
        if (m_i < m_t.size() && m_t[m_i] == '}') {
            ++m_i;
            out = Value(std::move(obj));
            return true;
        }
        while (true) {
            saltaBlancos();
            std::string key;
            if (!cadena(key, err)) {
                return false;
            }
            saltaBlancos();
            if (m_i >= m_t.size() || m_t[m_i] != ':') {
                err = "falta ':' tras la clave";
                return false;
            }
            ++m_i;
            saltaBlancos();
            Value v;
            if (!valor(v, err)) {
                return false;
            }
            // Clave repetida: gana la última, como Qt.
            const auto it = std::lower_bound(
                obj.begin(), obj.end(), key,
                [](const std::pair<std::string, Value>& p, const std::string& k) {
                    return p.first < k;
                });
            if (it != obj.end() && it->first == key) {
                it->second = std::move(v);
            } else {
                obj.insert(it, {key, std::move(v)});
            }
            saltaBlancos();
            if (m_i < m_t.size() && m_t[m_i] == ',') {
                ++m_i;
                continue;
            }
            if (m_i < m_t.size() && m_t[m_i] == '}') {
                ++m_i;
                out = Value(std::move(obj));
                return true;
            }
            err = "se esperaba ',' o '}' en un objeto";
            return false;
        }
    }

    bool array(Value& out, std::string& err) {
        ++m_i;  // '['
        Array arr;
        saltaBlancos();
        if (m_i < m_t.size() && m_t[m_i] == ']') {
            ++m_i;
            out = Value(std::move(arr));
            return true;
        }
        while (true) {
            saltaBlancos();
            Value v;
            if (!valor(v, err)) {
                return false;
            }
            arr.push_back(std::move(v));
            saltaBlancos();
            if (m_i < m_t.size() && m_t[m_i] == ',') {
                ++m_i;
                continue;
            }
            if (m_i < m_t.size() && m_t[m_i] == ']') {
                ++m_i;
                out = Value(std::move(arr));
                return true;
            }
            err = "se esperaba ',' o ']' en un array";
            return false;
        }
    }
};

// --- Serialización

void escapaCadena(const std::string& s, std::string& out) {
    out.push_back('"');
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // Lo no ASCII va tal cual, en UTF-8: es lo que escribe Qt, y
                    // escaparlo cambiaría todos los ficheros ya guardados.
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void escribeNumero(const Value& v, std::string& out) {
    if (v.type() == Value::Type::Int) {
        out += std::to_string(v.toInt());
        return;
    }
    const double d = v.toDouble();
    if (!std::isfinite(d)) {
        out += "null";  // JSON no tiene infinito ni NaN
        return;
    }
    // La representación MÁS CORTA que vuelve a dar el mismo double, que es lo que
    // escribe Qt. Con «%.17g» fijo salía 867811.87520846119 donde Qt pone
    // 867811.8752084612: el mismo número, pero el fichero cambiaría en cada guardado.
    // Lo detectó el sorteo, no la referencia dorada: la configuración real no tiene
    // ningún decimal.
    char buf[40];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) {
            break;
        }
    }
    out += buf;
}

void serializa(const Value& v, std::string& out, int nivel, bool indentado) {
    const std::string sangriaDentro = indentado ? std::string(static_cast<std::size_t>((nivel + 1) * 4), ' ') : std::string();
    const std::string sangriaCierre = indentado ? std::string(static_cast<std::size_t>(nivel * 4), ' ') : std::string();
    const char* salto = indentado ? "\n" : "";
    const char* trasDosPuntos = indentado ? " " : "";

    switch (v.type()) {
        case Value::Type::Null: out += "null"; return;
        case Value::Type::Bool: out += v.toBool() ? "true" : "false"; return;
        case Value::Type::Int:
        case Value::Type::Double: escribeNumero(v, out); return;
        case Value::Type::String: escapaCadena(v.toString(), out); return;
        case Value::Type::Array: {
            const Array& a = v.toArray();
            out += "[";
            out += salto;
            for (std::size_t i = 0; i < a.size(); ++i) {
                out += sangriaDentro;
                serializa(a[i], out, nivel + 1, indentado);
                if (i + 1 < a.size()) {
                    out += ",";
                }
                out += salto;
            }
            // Un array VACÍO sale igualmente como «[\n<sangría>]»: es lo que hace Qt, y
            // aquí se replica a propósito.
            out += sangriaCierre;
            out += "]";
            return;
        }
        case Value::Type::Object: {
            const Object& o = v.toObject();
            out += "{";
            out += salto;
            for (std::size_t i = 0; i < o.size(); ++i) {
                out += sangriaDentro;
                escapaCadena(o[i].first, out);
                out += ":";
                out += trasDosPuntos;
                serializa(o[i].second, out, nivel + 1, indentado);
                if (i + 1 < o.size()) {
                    out += ",";
                }
                out += salto;
            }
            out += sangriaCierre;
            out += "}";
            return;
        }
    }
}

}  // namespace

bool Value::toBool(bool porOmision) const {
    return m_type == Type::Bool ? m_bool : porOmision;
}

long long Value::toInt(long long porOmision) const {
    if (m_type == Type::Int) {
        return m_int;
    }
    if (m_type == Type::Double) {
        return static_cast<long long>(m_double);
    }
    return porOmision;
}

double Value::toDouble(double porOmision) const {
    if (m_type == Type::Double) {
        return m_double;
    }
    if (m_type == Type::Int) {
        return static_cast<double>(m_int);
    }
    return porOmision;
}

std::string Value::toString(const std::string& porOmision) const {
    return m_type == Type::String ? m_string : porOmision;
}

const Array& Value::toArray() const {
    return m_type == Type::Array ? m_array : arrayVacio();
}

const Object& Value::toObject() const {
    return m_type == Type::Object ? m_object : objetoVacio();
}

const Value& Value::operator[](const std::string& key) const {
    if (m_type != Type::Object) {
        return valorNulo();
    }
    const auto it = std::lower_bound(
        m_object.begin(), m_object.end(), key,
        [](const std::pair<std::string, Value>& p, const std::string& k) { return p.first < k; });
    return (it != m_object.end() && it->first == key) ? it->second : valorNulo();
}

bool Value::contains(const std::string& key) const {
    if (m_type != Type::Object) {
        return false;
    }
    const auto it = std::lower_bound(
        m_object.begin(), m_object.end(), key,
        [](const std::pair<std::string, Value>& p, const std::string& k) { return p.first < k; });
    return it != m_object.end() && it->first == key;
}

void Value::set(const std::string& key, Value v) {
    if (m_type != Type::Object) {
        m_type = Type::Object;
        m_object.clear();
    }
    const auto it = std::lower_bound(
        m_object.begin(), m_object.end(), key,
        [](const std::pair<std::string, Value>& p, const std::string& k) { return p.first < k; });
    if (it != m_object.end() && it->first == key) {
        it->second = std::move(v);
    } else {
        m_object.insert(it, {key, std::move(v)});
    }
}

void Value::remove(const std::string& key) {
    if (m_type != Type::Object) {
        return;
    }
    const auto it = std::lower_bound(
        m_object.begin(), m_object.end(), key,
        [](const std::pair<std::string, Value>& p, const std::string& k) { return p.first < k; });
    if (it != m_object.end() && it->first == key) {
        m_object.erase(it);
    }
}

void Value::push(Value v) {
    if (m_type != Type::Array) {
        m_type = Type::Array;
        m_array.clear();
    }
    m_array.push_back(std::move(v));
}

bool parse(const std::string& text, Value& out, std::string* error) {
    std::string err;
    Parser p(text);
    if (p.run(out, err)) {
        return true;
    }
    if (error) {
        *error = err;
    }
    out = Value();
    return false;
}

std::string toIndented(const Value& v) {
    std::string out;
    out.reserve(4096);
    serializa(v, out, 0, true);
    out.push_back('\n');  // QJsonDocument::toJson deja salto final
    return out;
}

std::string toCompact(const Value& v) {
    std::string out;
    out.reserve(1024);
    serializa(v, out, 0, false);
    return out;
}

}  // namespace zfsmgr::base::json
