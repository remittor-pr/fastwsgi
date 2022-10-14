#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"
#include "uv-common.h"
#include "llhttp.h"

#include "server.h"
#include "request.h"
#include "constants.h"

PyObject* wsgi_app;
char* host;
int port;
int backlog;

uv_tcp_t server;
uv_loop_t* loop;
uv_os_fd_t file_descriptor;

struct sockaddr_in addr;

static const char* BAD_REQUEST = "HTTP/1.1 400 Bad Request\r\n\r\n";
static const char* INTERNAL_ERROR = "HTTP/1.1 500 Internal Server Error\r\n\r\n";

void logger(char* message) {
    if (LOGGING_ENABLED)
        fprintf(stdout, ">>> %s\n", message);
}

void close_cb(uv_handle_t* handle) {
    logger("disconnected");
    client_t * client = (client_t *)handle->data;
    Py_XDECREF(client->request.headers);
    if (client->response.buffer.base)
        free(client->response.buffer.base);
    free(client);
}

void shutdown_cb(uv_shutdown_t* req, int status) {
    uv_handle_t* handle = (uv_handle_t*)req->handle;
    if (!uv_is_closing(handle))
        uv_close(handle, close_cb);
    free(req);
}

void close_connection(uv_stream_t* handle) {
    uv_shutdown_t* shutdown = malloc(sizeof(uv_shutdown_t));
    uv_shutdown(shutdown, handle, shutdown_cb);
}

void write_cb(uv_write_t* req, int status) {
    if (status) {
        fprintf(stderr, "Write error %s\n", uv_strerror(status));
    }
    write_req_t* write_req = (write_req_t*)req;
    free(write_req->buf.base);
    free(write_req);
}

void send_error(write_req_t* req, uv_stream_t* handle, const char* error_string) {
    char* error = malloc(strlen(error_string) + 1);
    strcpy(error, error_string);
    req->buf = uv_buf_init(error, strlen(error));
    uv_write((uv_write_t*)req, handle, &req->buf, 1, write_cb);
    close_connection(handle);
}

void send_response(write_req_t* req, uv_stream_t* handle, client_t* client) {
    uv_buf_t * resbuf = &client->response.buffer;
    req->buf = uv_buf_init(resbuf->base, resbuf->len);
    uv_write((uv_write_t*)req, handle, &req->buf, 1, write_cb);
    if (!client->request.state.keep_alive)
        close_connection(handle);
}


void read_cb(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
    int continue_read = 0;
    client_t* client = (client_t*)handle->data;
    llhttp_t * parser = &client->request.parser;

    write_req_t* req = (write_req_t*)malloc(sizeof(write_req_t));

    if (nread > 0) {
        enum llhttp_errno err = llhttp_execute(parser, buf->base, nread);
        if (err == HPE_OK) {
            logger("Successfully parsed");
            if (client->response.buffer.len > 0)
                send_response(req, handle, client);
            else if (client->request.state.error)
                send_error(req, handle, INTERNAL_ERROR);
            else
                continue_read = 1;
        }
        else {
            fprintf(stderr, "Parse error: %s %s\n", llhttp_errno_name(err), client->request.parser.reason);
            send_error(req, handle, BAD_REQUEST);
        }
    }
    if (nread < 0) {
        uv_read_stop(handle);

        if (nread == UV_ECONNRESET) {
            close_connection(handle);
        }
        else if (nread != UV_EOF) {
            fprintf(stderr, "Read error %s\n", uv_err_name(nread));
            close_connection(handle);
        }
    }

    if (buf->base)
        free(buf->base);

    if (PyErr_Occurred())
        PyErr_Clear();
}

void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    logger("Allocating buffer");
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}

void connection_cb(uv_stream_t* server, int status) {
    if (status < 0) {
        fprintf(stderr, "Connection error %s\n", uv_strerror(status));
        return;
    }

    client_t* client = calloc(1, sizeof(client_t));

    uv_tcp_init(loop, &client->handle);
    uv_tcp_nodelay(&client->handle, 0);
    uv_tcp_keepalive(&client->handle, 1, 60);

    struct sockaddr sockname;
    struct sockaddr_in* addr = (struct sockaddr_in*)&sockname;
    int socklen = sizeof(sockname);

    uv_tcp_getsockname((uv_tcp_t*)&client->handle, &sockname, &socklen);
    uv_ip4_name(addr, client->remote_addr, sizeof(client->remote_addr));

    client->handle.data = client;

    if (uv_accept(server, (uv_stream_t*)&client->handle) == 0) {
        llhttp_init(&client->request.parser, HTTP_REQUEST, &parser_settings);
        client->request.parser.data = client;
        uv_read_start((uv_stream_t*)&client->handle, alloc_cb, read_cb);
    }
    else {
        uv_close((uv_handle_t*)&client->handle, close_cb);
    }
}

void signal_handler(uv_signal_t* req, int signum) {
    if (signum == SIGINT) {
        uv_stop(loop);
        uv_signal_stop(req);
        exit(0);
    }
}

int main() {
    loop = uv_default_loop();

    configure_parser_settings();
    init_constants();
    init_request_dict();

    uv_ip4_addr(host, port, &addr);

    uv_tcp_init_ex(loop, &server, AF_INET);

    uv_fileno((const uv_handle_t*)&server, &file_descriptor);

    int enabled = 1;
#ifdef _WIN32
    //uv__socket_sockopt((uv_handle_t*)&server, SO_REUSEADDR, &enabled);
#else
    int so_reuseport = 15;  // SO_REUSEPORT
    uv__socket_sockopt((uv_handle_t*)&server, so_reuseport, &enabled);
#endif

    int err = uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
    if (err) {
        fprintf(stderr, "Bind error %s\n", uv_strerror(err));
        return 1;
    }

    err = uv_listen((uv_stream_t*)&server, backlog, connection_cb);
    if (err) {
        fprintf(stderr, "Listen error %s\n", uv_strerror(err));
        return 1;
    }

    uv_signal_t signal;
    uv_signal_init(loop, &signal);
    uv_signal_start(&signal, signal_handler, SIGINT);

    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
    free(loop);
    return 0;
}

PyObject* run_server(PyObject* self, PyObject* args) {
    PyArg_ParseTuple(args, "Osiii", &wsgi_app, &host, &port, &backlog, &LOGGING_ENABLED);
    main();
    Py_RETURN_NONE;
}
