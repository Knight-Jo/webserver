#include "HttpParser.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include "HttpRequest.h"

bool HttpParser::parseRequestLine(const std::string &line, HttpRequest *request)
{
    std::istringstream iss(line);
    std::string method;
    std::string uri;
    std::string version;

    if (!(iss >> method >> uri >> version))
    {
        return false;
    }

    if (version != "HTTP/1.0" && version != "HTTP/1.1")
    {
        return false;
    }

    request->setMethod(method);
    request->setVersion(version);

    std::string path = uri;
    std::string query;
    std::string::size_type pos = uri.find('?');
    if (pos != std::string::npos)
    {
        path = uri.substr(0, pos);
        query = uri.substr(pos + 1);
    }

    request->setPath(path);
    request->setQuery(query);
    parseQueryString(query, request);
    return true;
}

bool HttpParser::parseHeaderLine(const std::string &line, HttpRequest *request)
{
    std::string::size_type pos = line.find(':');
    if (pos == std::string::npos)
    {
        return false;
    }
    std::string key = trim(line.substr(0, pos));
    std::string value = trim(line.substr(pos + 1));
    if (key.empty())
    {
        return false;
    }
    request->addHeader(key, value);
    return true;
}

void HttpParser::parseQueryString(const std::string &query, HttpRequest *request)
{
    if (query.empty())
    {
        return;
    }
    std::string::size_type start = 0;
    while (start < query.size())
    {
        std::string::size_type amp = query.find('&', start);
        if (amp == std::string::npos)
        {
            amp = query.size();
        }
        std::string part = query.substr(start, amp - start);
        std::string::size_type eq = part.find('=');
        if (eq != std::string::npos)
        {
            request->addQueryParam(part.substr(0, eq), part.substr(eq + 1));
        }
        else if (!part.empty())
        {
            request->addQueryParam(part, "");
        }
        start = amp + 1;
    }
}

std::string HttpParser::trim(const std::string &value)
{
    std::string::size_type first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
    {
        ++first;
    }
    std::string::size_type last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
    {
        --last;
    }
    return value.substr(first, last - first);
}

std::string HttpParser::toLower(const std::string &value)
{
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}
