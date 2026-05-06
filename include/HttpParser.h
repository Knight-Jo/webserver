#pragma once

#include <string>

class HttpRequest;

class HttpParser
{
public:
    static bool parseRequestLine(const std::string &line, HttpRequest *request);
    static bool parseHeaderLine(const std::string &line, HttpRequest *request);
    static void parseQueryString(const std::string &query, HttpRequest *request);
    static std::string trim(const std::string &value);
    static std::string toLower(const std::string &value);
};
