#pragma once

#include <string>
#include <unordered_map>

class HttpResponse
{
public:
    HttpResponse() = default;

    void setStatus(int code, const std::string &message)
    {
        statusCode_ = code;
        statusMessage_ = message;
    }

    void setHeader(const std::string &key, const std::string &value);
    void setBody(const std::string &body) { body_ = body; }
    void setCloseConnection(bool on) { closeConnection_ = on; }

    int statusCode() const { return statusCode_; }
    const std::string &statusMessage() const { return statusMessage_; }
    bool closeConnection() const { return closeConnection_; }

    std::string toString() const;

private:
    int statusCode_ = 200;
    std::string statusMessage_ = "OK";
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
    bool closeConnection_ = false;
};
