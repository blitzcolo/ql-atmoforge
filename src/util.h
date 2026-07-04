#pragma once
#include <string>
#include <vector>

namespace ql_atmoforge {

// printf-style formatting into std::string
std::string strf(const char* fmt, ...);

// strip trailing \r\n and whitespace (tape files are CRLF)
std::string rstrip(const std::string& s);

// split on runs of whitespace
std::vector<std::string> tokenize(const std::string& line);

// whole file -> string; throws std::runtime_error on failure
std::string read_text_file(const std::string& path);

}  // namespace ql_atmoforge
