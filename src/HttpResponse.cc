#include "HttpResponse.h"

#include <sstream>

void HttpResponse::setHeader(const std::string &key, const std::string &value)
{
    headers_[key] = value;
}

std::string HttpResponse::toString() const
{
    std::ostringstream out;
    out << "HTTP/1.1 " << statusCode_ << " " << statusMessage_ << "\r\n";

    if (headers_.find("Content-Length") == headers_.end())
    {
        out << "Content-Length: " << body_.size() << "\r\n";
    }

    if (headers_.find("Connection") == headers_.end())
    {
        out << "Connection: " << (closeConnection_ ? "close" : "keep-alive") << "\r\n";
    }

    for (const auto &pair : headers_)
    {
        out << pair.first << ": " << pair.second << "\r\n";
    }

    out << "\r\n";
    out << body_;
    return out.str();
}
