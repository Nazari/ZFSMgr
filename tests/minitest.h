// A tiny test runner with QtTest's macro names, so tests written for QtTest run without Qt.
//
//   QVERIFY(cond)       fails the test when cond is false
//   QVERIFY2(cond, msg) the same, with a message
//   QCOMPARE(a, b)      fails the test when a != b, printing both when they can be printed
//   QSKIP("why")        ends the test as skipped
//
// Each test is a member function of a class with init() and cleanup(), run in order around
// every test, as QtTest does. main() lists the tests: minitest::run<Class>({{"name", &Class::name}, …}).
#pragma once

#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace minitest {

struct Failed {
    std::string what;
};
struct Skipped {
    std::string why;
};

template <typename T, typename = void>
struct Printable : std::false_type {};
template <typename T>
struct Printable<T,
                 std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
std::string show(const T& v) {
    if constexpr (Printable<T>::value) {
        std::ostringstream o;
        o << v;
        return o.str();
    } else {
        return "(not printable)";
    }
}

template <typename A, typename B>
void compare(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (a == b) {
        return;
    }
    std::ostringstream o;
    o << file << ":" << line << ": QCOMPARE(" << ea << ", " << eb << ")\n    actual:   " << show(a)
      << "\n    expected: " << show(b);
    throw Failed{o.str()};
}

inline void verify(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        throw Failed{std::string(file) + ":" + std::to_string(line) + ": QVERIFY(" + expr + ")"};
    }
}

template <typename T>
struct Case {
    const char* name;
    void (T::*fn)();
};

template <typename T>
int run(std::initializer_list<Case<T>> cases) {
    int passed = 0, failed = 0, skipped = 0;
    for (const Case<T>& c : cases) {
        T t;
        try {
            t.init();
            (t.*c.fn)();
            std::printf("PASS   : %s\n", c.name);
            ++passed;
        } catch (const Skipped& s) {
            std::printf("SKIP   : %s  %s\n", c.name, s.why.c_str());
            ++skipped;
        } catch (const Failed& f) {
            std::printf("FAIL!  : %s\n  %s\n", c.name, f.what.c_str());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("FAIL!  : %s  exception: %s\n", c.name, e.what());
            ++failed;
        }
        t.cleanup();
    }
    std::printf("Totals: %d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

}  // namespace minitest

#define QVERIFY(cond) ::minitest::verify(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define QCOMPARE(a, b) ::minitest::compare((a), (b), #a, #b, __FILE__, __LINE__)
#define QSKIP(why)              \
    throw ::minitest::Skipped { \
        why                     \
    }
#define QVERIFY2(cond, msg)                                                                  \
    ::minitest::verify(static_cast<bool>(cond), (std::string(#cond) + "  " + (msg)).c_str(), \
                       __FILE__, __LINE__)
