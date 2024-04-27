#pragma once

#include <string>
#include <typeinfo>
#include <typeindex>
#include <iostream>
#include <memory>

#include <cxxabi.h>

class Demangle
{
    const char* name{};
public:
    Demangle(const char* n) : name{n} {}
    Demangle(const std::string& n) : name{n.c_str()} {}
    Demangle(const std::type_info& t) : name{t.name()} {}
    Demangle(const std::type_index& t) : name{t.name()} {}
    template <typename Stream, typename = Stream::char_type>
    friend Stream& operator<<(Stream& stream, const Demangle& d)
    {
#ifdef __GNUG__
        size_t len{};
        int status{};
        std::unique_ptr<char, void (*)(void *)> text{abi::__cxa_demangle(d.name, nullptr, &len, &status), std::free};
        if (status == 0)
        {
            stream << text.get();
        }
#else
        stream << d.name;
#endif
        return stream;
    }
};
