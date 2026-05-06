#include "HttpRequest.h"
#include "HttpParser.h"

void HttpRequest::addHeader(const std::string &key, const std::string &value)
{
    headers_[HttpParser::toLower(key)] = value;
}

std::string HttpRequest::getHeader(const std::string &key) const
{
    auto it = headers_.find(HttpParser::toLower(key));
    if (it == headers_.end())
    {
        return std::string();
    }
    return it->second;
}

void HttpRequest::addQueryParam(const std::string &key, const std::string &value)
{
    queryParams_[key] = value;
}
