#pragma once

#include <string>
#include <unordered_map>

class HttpRequest
{
public:
    void setMethod(const std::string &method) { method_ = method; }
    void setPath(const std::string &path) { path_ = path; }
    void setQuery(const std::string &query) { query_ = query; }
    void setVersion(const std::string &version) { version_ = version; }
    void setKeepAlive(bool on) { keepAlive_ = on; }
    void setBody(const std::string &body) { body_ = body; }

    const std::string &method() const { return method_; }
    const std::string &path() const { return path_; }
    const std::string &query() const { return query_; }
    const std::string &version() const { return version_; }
    const std::string &body() const { return body_; }
    bool keepAlive() const { return keepAlive_; }

    void addHeader(const std::string &key, const std::string &value);
    std::string getHeader(const std::string &key) const;
    const std::unordered_map<std::string, std::string> &headers() const { return headers_; }

    void addQueryParam(const std::string &key, const std::string &value);
    const std::unordered_map<std::string, std::string> &queryParams() const { return queryParams_; }

private:
    std::string method_;
    std::string path_;
    std::string query_;
    std::string version_;
    std::string body_;
    bool keepAlive_ = false;

    std::unordered_map<std::string, std::string> headers_;
    std::unordered_map<std::string, std::string> queryParams_;
};
