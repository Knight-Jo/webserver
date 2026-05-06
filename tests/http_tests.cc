#include <assert.h>
#include <cstring>
#include <string>

#include "Buffer.h"
#include "HttpContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpRouter.h"

static void testRequestParsingGet()
{
    Buffer buf;
    const char *req = "GET /hello?x=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    buf.append(req, std::strlen(req));

    HttpContext context;
    bool bad = false;
    bool done = context.parseRequest(&buf, &bad);
    assert(done);
    assert(!bad);

    const HttpRequest &request = context.request();
    assert(request.method() == "GET");
    assert(request.path() == "/hello");
    assert(request.query() == "x=1");
    assert(request.getHeader("Host") == "localhost");
    assert(!request.keepAlive());
}

static void testRequestParsingPost()
{
    Buffer buf;
    const char *req = "POST /submit HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nhello=world";
    buf.append(req, std::strlen(req));

    HttpContext context;
    bool bad = false;
    bool done = context.parseRequest(&buf, &bad);
    assert(done);
    assert(!bad);

    const HttpRequest &request = context.request();
    assert(request.method() == "POST");
    assert(request.path() == "/submit");
    assert(request.body() == "hello=world");
    assert(request.keepAlive());
}

static void testRouter()
{
    HttpRouter router;
    router.addRoute("GET", "/ping", [](const HttpRequest &, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        response->setBody("pong");
    });

    HttpRequest req;
    req.setMethod("POST");
    req.setPath("/ping");

    HttpResponse resp;
    bool handled = router.route(req, &resp);
    assert(handled);
    assert(resp.statusCode() == 405);
    assert(resp.toString().find("Allow:") != std::string::npos);
}

static void testResponseSerialization()
{
    HttpResponse resp;
    resp.setStatus(200, "OK");
    resp.setHeader("Content-Type", "text/plain");
    resp.setBody("hi");

    std::string out = resp.toString();
    assert(out.find("HTTP/1.1 200 OK") != std::string::npos);
    assert(out.find("Content-Length: 2") != std::string::npos);
}

int main()
{
    testRequestParsingGet();
    testRequestParsingPost();
    testRouter();
    testResponseSerialization();
    return 0;
}
