#pragma once

#include <string>
#include <unordered_map>

#include "Callbacks.h"

class HttpRequest;
class HttpResponse;

class HttpRouter
{
public:
    void addRoute(const std::string &method, const std::string &path, const HttpHandler &handler);
    bool route(const HttpRequest &request, HttpResponse *response) const;

private:
    using MethodMap = std::unordered_map<std::string, HttpHandler>;
    std::unordered_map<std::string, MethodMap> routes_;
};
