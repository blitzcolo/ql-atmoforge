#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ql_atmoforge {

std::string strf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return {};
    if (n < (int)sizeof(buf)) return std::string(buf, n);
    std::string out((size_t)n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(&out[0], out.size(), fmt, ap);
    va_end(ap);
    out.resize((size_t)n);
    return out;
}

std::string rstrip(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e-1] == '\r' || s[e-1] == '\n' || s[e-1] == ' ' || s[e-1] == '\t'))
        --e;
    return s.substr(0, e);
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream is(line);
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

std::string read_text_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace ql_atmoforge
