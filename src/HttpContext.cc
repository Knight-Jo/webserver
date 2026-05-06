#include "HttpContext.h"

#include <algorithm>
#include <cstdlib>
#include "Buffer.h"
#include "HttpParser.h"

HttpContext::HttpContext()
    : state_(ParseState::kExpectRequestLine)
    , bodyBytesNeeded_(0)
{
}

bool HttpContext::parseRequest(Buffer *buf, bool *badRequest)
{
    if (badRequest)
    {
        *badRequest = false;
    }

    while (true)
    {
        if (state_ == ParseState::kExpectRequestLine)
        {
            const char *crlf = findCRLF(buf->peek(), buf->peek() + buf->readableBytes());
            if (!crlf)
            {
                return false;
            }
            std::string line(buf->peek(), crlf);
            buf->retrieve(static_cast<size_t>(crlf - buf->peek()) + 2);
            if (!HttpParser::parseRequestLine(line, &request_))
            {
                if (badRequest)
                {
                    *badRequest = true;
                }
                return false;
            }
            state_ = ParseState::kExpectHeaders;
        }
        else if (state_ == ParseState::kExpectHeaders)
        {
            const char *crlf = findCRLF(buf->peek(), buf->peek() + buf->readableBytes());
            if (!crlf)
            {
                return false;
            }
            std::string line(buf->peek(), crlf);
            buf->retrieve(static_cast<size_t>(crlf - buf->peek()) + 2);
            if (line.empty())
            {
                std::string connection = request_.getHeader("Connection");
                std::string version = request_.version();
                bool keepAlive = (version == "HTTP/1.1");
                if (!connection.empty())
                {
                    keepAlive = (HttpParser::toLower(connection) != "close");
                }
                request_.setKeepAlive(keepAlive);

                std::string contentLength = request_.getHeader("Content-Length");
                if (!contentLength.empty())
                {
                    char *end = nullptr;
                    unsigned long parsed = std::strtoul(contentLength.c_str(), &end, 10);
                    if (end == contentLength.c_str() || *end != '\0')
                    {
                        if (badRequest)
                        {
                            *badRequest = true;
                        }
                        return false;
                    }
                    bodyBytesNeeded_ = static_cast<size_t>(parsed);
                    if (bodyBytesNeeded_ > 0)
                    {
                        state_ = ParseState::kExpectBody;
                        continue;
                    }
                }
                state_ = ParseState::kGotAll;
                return true;
            }
            if (!HttpParser::parseHeaderLine(line, &request_))
            {
                if (badRequest)
                {
                    *badRequest = true;
                }
                return false;
            }
        }
        else if (state_ == ParseState::kExpectBody)
        {
            if (buf->readableBytes() < bodyBytesNeeded_)
            {
                return false;
            }
            std::string body = buf->retrieveAsString(bodyBytesNeeded_);
            request_.setBody(body);
            state_ = ParseState::kGotAll;
            return true;
        }
        else
        {
            return true;
        }
    }
}

void HttpContext::reset()
{
    state_ = ParseState::kExpectRequestLine;
    request_ = HttpRequest();
    bodyBytesNeeded_ = 0;
}

const char *HttpContext::findCRLF(const char *start, const char *end)
{
    const char crlf[] = "\r\n";
    const char *found = std::search(start, end, crlf, crlf + 2);
    if (found == end)
    {
        return nullptr;
    }
    return found;
}
