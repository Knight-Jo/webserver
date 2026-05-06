#include "HttpRouter.h"

#include <sstream>
#include "HttpRequest.h"
#include "HttpResponse.h"

void HttpRouter::addRoute(const std::string &method, const std::string &path, const HttpHandler &handler)
{
    routes_[path][method] = handler;
}

bool HttpRouter::route(const HttpRequest &request, HttpResponse *response) const
{
    auto pathIt = routes_.find(request.path());
    if (pathIt == routes_.end())
    {
        return false;
    }

    const MethodMap &methods = pathIt->second;
    auto methodIt = methods.find(request.method());
    if (methodIt == methods.end())
    {
        std::ostringstream allow;
        bool first = true;
        for (const auto &pair : methods)
        {
            if (!first)
            {
                allow << ", ";
            }
            allow << pair.first;
            first = false;
        }
        response->setStatus(405, "Method Not Allowed");
        response->setHeader("Allow", allow.str());
        response->setBody("Method Not Allowed");
        return true;
    }

    methodIt->second(request, response);
    return true;
}
