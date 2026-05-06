#pragma once

#include <stddef.h>

#include "HttpRequest.h"

class Buffer;

class HttpContext
{
public:
    enum class ParseState
    {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,
        kGotAll
    };

    HttpContext();
    bool parseRequest(Buffer *buf, bool *badRequest);
    bool gotAll() const { return state_ == ParseState::kGotAll; }
    void reset();

    HttpRequest &request() { return request_; }
    const HttpRequest &request() const { return request_; }

private:
    static const char *findCRLF(const char *start, const char *end);

    ParseState state_;
    HttpRequest request_;
    size_t bodyBytesNeeded_;
};
