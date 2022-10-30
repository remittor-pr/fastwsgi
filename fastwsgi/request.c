#include "server.h"
#include "request.h"
#include "llhttp.h"
#include "constants.h"
#include "start_response.h"
#include "pyhacks.h"

PyObject* g_base_dict = NULL;

typedef enum {
    SH_LATIN1       = 0x01,
    SH_CONCAT       = 0x02,
    SH_COMMA_DELIM  = 0x04
} set_header_flag_t;

static
int set_header(client_t * client, PyObject * key, const char * value, ssize_t length, int flags)
{
    PyObject * headers = client->request.headers;
    ssize_t vlen = (length >= 0) ? length : strlen(value);
    LOGi("set_header: %s = '%.*s' %s", PyUnicode_AsUTF8(key), (int)vlen, value, (flags & SH_CONCAT) ? "(concat)" : "");
    PyObject * val = NULL;
    PyObject * setval = NULL;
    if (flags & SH_LATIN1) {
        val = PyUnicode_DecodeLatin1(value, vlen, NULL);
    } else {
        val = PyUnicode_FromStringAndSize(value, vlen);  // as UTF-8
    }
    if (!val)
        return -3;

    if (flags & SH_CONCAT) {  // concat PyUnicode value
        PyObject * existing_val = PyDict_GetItem(headers, key);
        if (existing_val) {
            if (vlen == 0) {  // skip concat empty string
                Py_XDECREF(val);
                return 0;
            }
            setval = PyUnicode_Concat(existing_val, val);
        }
    }
    PyDict_SetItem(headers, key, (setval != NULL) ? setval : val);
    Py_XDECREF(val);
    Py_XDECREF(setval);
    return 0;
}

static 
int set_header_v(client_t * client, const char * key, const char * value, ssize_t length, int flags)
{
    if (!key)
        return -11;
    size_t klen = strlen(key);
    if (klen == 0)
        return -12;
    PyObject * pkey = PyUnicode_FromStringAndSize(key, klen);
    int retval = set_header(client, pkey, value, length, flags);
    Py_DECREF(pkey);
    return retval;
}

void close_iterator(PyObject * iterator)
{
    if (iterator != NULL && PyIter_Check(iterator)) {
        PyObject * close = PyObject_GetAttrString(iterator, "close");
        if (close != NULL) {
            PyObject * close_result = PyObject_CallObject(close, NULL);
            Py_XDECREF(close_result);
            Py_XDECREF(close);
        }
    }
}

static
void reset_head_buffer(client_t * client)
{
    client->request.current_key_len = 0;
    client->request.current_val_len = 0;
    xbuf_reset(&client->head);
    client->response.headers_size = 0;
}

static
int reset_wsgi_input(client_t * client)
{
    if (client->request.wsgi_input_size > 1*1024*1024) {
        // Always free huge buffers for incoming data
        Py_CLEAR(client->request.wsgi_input);
    }
    client->request.wsgi_input_size = 0;
    return 0;
}

void free_start_response(client_t * client)
{
    if (client->start_response) {
        Py_CLEAR(client->start_response->headers);
        Py_CLEAR(client->start_response->status);
        Py_CLEAR(client->start_response->exc_info);
        Py_CLEAR(client->start_response);
    }
}

void reset_response_body(client_t * client)
{
    int chunks = client->response.body_chunk_num;
    if (chunks || client->response.wsgi_body) {
        LOGd("reset_response_body: chunks = %d, wsgi_body = %p", chunks, client->response.wsgi_body);
    }
    for (int i = 0; i < chunks; i++) {
        Py_XDECREF(client->response.body[i]);
    }
    client->response.body_chunk_num = 0;
    client->response.body_preloaded_size = 0;
    client->response.body_total_size = 0;

    if (client->response.wsgi_body) {
        if (client->response.body_iterator == client->response.wsgi_body) {
            // ClosingIterator.close() or FileWrapper.close()
            close_iterator(client->response.wsgi_body);
        } else {
            Py_CLEAR(client->response.body_iterator);
        }
    }
    Py_CLEAR(client->response.wsgi_body);
}

// ============== request processing ==================================================

int on_message_begin(llhttp_t * parser)
{
    LOGi("on_message_begin: ------------------------------");
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_BEGIN;
    if (client->head.data == NULL)
        xbuf_init(&client->head, NULL, 2*1024);
    //client->request.keep_alive = 0;
    client->error = 0;
    if (client->response.write_req.client != NULL) {
        client->error = 1;
        LOGc("Received new HTTP request while sending response! Disconnect client!");
        return -1;
    }
    Py_CLEAR(client->request.headers);  // wsgi_input: refcnt 2 -> 1
    // Sets up base request dict for new incoming requests
    // https://www.python.org/dev/peps/pep-3333/#specification-details
    client->request.headers = PyDict_Copy(g_base_dict);
    client->request.http_content_length = -1; // not specified
    client->request.chunked = 0;
    client->request.expect_continue = 0;
    reset_wsgi_input(client);
    reset_head_buffer(client);
    free_start_response(client);
    reset_response_body(client);
    client->response.wsgi_content_length = -1;
    return 0;
}

int on_url(llhttp_t * parser, const char * data, size_t length)
{
    LOGd("%s: (len = %d)", __func__, (int)length);
    if (length > 0) {
        client_t * client = (client_t *)parser->data;
        xbuf_add(&client->head, data, length);
    }
    return 0;
}

int on_url_complete(llhttp_t * parser)
{
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_URL;
    xbuf_t * buf = &client->head;
    LOGi("%s: \"%s\"", __func__, buf->data);
    char * path = buf->data;
    ssize_t path_len = buf->size;
    char * query = strchr(buf->data, '?');
    if (query) {        
        *query++ = 0;
        ssize_t query_len = strlen(query);
        if (query_len > 0) {
            set_header(client, g_cv.QUERY_STRING, query, query_len, 0);
        }
        path_len = strlen(path);
    }
    set_header(client, g_cv.PATH_INFO, path, path_len, 0);
    reset_head_buffer(client);
    return 0;
}

int on_header_field(llhttp_t * parser, const char * data, size_t length)
{
    LOGd_IF(length == 0, "%s: <empty>", __func__);
    if (length == 0)
        return 0;

    LOGd("%s: '%.*s'", __func__, (int)length, data);
    client_t * client = (client_t *)parser->data;
    if (client->request.current_key_len == 0) {
        xbuf_add(&client->head, "HTTP_", 5);
        client->request.current_key_len = 5;
    }
    xbuf_add(&client->head, data, length);
    client->request.current_key_len += length;
    client->request.current_val_len = 0;
    return 0;
}

int on_header_field_complete(llhttp_t * parser)
{
    client_t * client = (client_t *)parser->data;
    xbuf_t * buf = &client->head;
    LOGi("%s: %s", __func__, buf->size ? buf->data + 5 : buf->data);
    char * data = buf->data;
    ssize_t size = buf->size;
    for (ssize_t i = 0; i < size; i++) {
        const char symbol = data[i];
        if (symbol == '_' || (unsigned char)symbol > 127) {
            continue;
        }
        if (symbol == '-') {
            data[i] = '_';
            continue;
        }
        data[i] = toupper(symbol);
    }
    xbuf_add(buf, "\0", 1);  // add empty value
    client->request.current_val_len = 0;
    return 0;
}

int on_header_value(llhttp_t * parser, const char * data, size_t length)
{
    LOGd_IF(length == 0, "%s: <empty>", __func__);
    if (length == 0)
        return 0;

    LOGd("%s: '%.*s'", __func__, (int)length, data);
    client_t * client = (client_t *)parser->data;
    xbuf_t * buf = &client->head;
    if (client->request.current_val_len == 0) {
        if (client->request.current_key_len == 0) {
            client->error = 1;
            LOGc("%s: Headers has an unnamed value!", __func__);
            return -1;  // critical error
        }
        if (client->request.current_key_len + 1 != (size_t)buf->size) {
            client->error = 1;
            LOGc("%s: internal error (1)", __func__);
            return -1;  // critical error
        }
    }
    xbuf_add(buf, data, length);
    client->request.current_val_len += length;
    return 0;
}

int on_header_value_complete(llhttp_t * parser)
{
    client_t * client = (client_t *)parser->data;
    xbuf_t * buf = &client->head;
    size_t key_len = client->request.current_key_len;
    size_t val_len = client->request.current_val_len;
    char * key = buf->data;
    char * val = buf->data + key_len + 1;
    LOGi("%s: '%s'", __func__, val);
    if (key_len == 0) {
        client->error = 1;
        LOGc("%s: Headers has an unnamed value!", __func__);
        reset_head_buffer(client);
        return -1;  // critical error
    }
    if (key_len == 19 && strncmp(key, "HTTP_CONTENT_LENGTH", 19) == 0) {
        client->request.http_content_length = 0; // field "Content-Length" present
        key += 5;  // exclude prefix "HTTP_"
    }
    if (key_len == 17 && strncmp(key, "HTTP_CONTENT_TYPE", 17) == 0) {
        key += 5;  // exclude prefix "HTTP_"
    }
    if (key_len == 22 && strncmp(key, "HTTP_TRANSFER_ENCODING", 22) == 0) {
        if (val_len == 7 && strncmp(val, "chunked", 7) == 0) {
            client->request.chunked = 1;
        } else {
            client->error = 1;
            LOGc("%s: Header \"Transfer-Encoding\" contain unsupported value = '%s'", __func__, val);
            // https://peps.python.org/pep-3333/#other-http-features
            LOGc("%s: FastWSGI cannot decompress content!", __func__);
            return -1;  // critical error
        }
        key = NULL;  // skip chunked flag
        // Let's skip the chunks flag, since the FastWSGI server completely downloads the body into memory
        // and immediately gives body to the WSGI application.
    }
    if (key_len == 11 && strncmp(key, "HTTP_EXPECT", 11) == 0) {
        if (val_len != 12 || strncmp(val, "100-continue", 12) != 0) {
            client->error = 1;
            LOGc("%s: Header \"Expect\" contain unsupported value = '%s'", __func__, val);
            // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Expect
            return -1;  // critical error
        }
        client->request.expect_continue = 1;
        key = NULL;  // hide Expect header
    }
    if (key)
        set_header_v(client, key, val, val_len, 0);

    reset_head_buffer(client);
    return 0;
}

int on_headers_complete(llhttp_t * parser)
{
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_HEADERS;
    uint64_t clen = parser->content_length;
    LOGi("%s: %s", __func__, (client->request.chunked) ? "(chunked)" : "");
    reset_head_buffer(client);
    if (clen > g_srv.max_content_length) {
        LOGc("Received HTTP headers with \"Content-Length\" = %llu (expected <= %llu)", clen, g_srv.max_content_length);
        if (client->request.expect_continue) {
            client->error = HTTP_STATUS_EXPECTATION_FAILED;
            return 1; // Assume that request has no body, and proceed to parsing the next message!
        }
        client->error = 1;
        return -1;  // error
    }
    if (client->request.http_content_length >= 0) {
        client->request.http_content_length = (int)clen;
    }
    if (client->request.expect_continue) {
        x_send_status(client, HTTP_STATUS_CONTINUE);
        client->request.expect_continue = 0;
    }
    return 0;
}

int on_body(llhttp_t * parser, const char * body, size_t length)
{
    LOGi("%s: len = %d", __func__, (int)length);
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_BODY;
    if (length == 0)
        return 0;

    PyObject* wsgi_input = client->request.wsgi_input;
    if (wsgi_input == NULL) {
        wsgi_input = PyObject_CallMethodObjArgs(g_cv.module_io, g_cv.BytesIO, NULL);
        client->request.wsgi_input = wsgi_input;  // object cached
        client->request.wsgi_input_size = 0;
    }
    else if (client->request.wsgi_input_size == 0) {
        PyObject* result = PyObject_CallMethodObjArgs(wsgi_input, g_cv.seek, g_cv.i0, NULL);
        Py_DECREF(result);
    }
    uint64_t clen = (uint64_t)client->request.wsgi_input_size + (uint64_t)length;
    if (clen > g_srv.max_content_length) {
        client->error = 1;
        LOGc("Received too large body of HTTP request: size = %llu (expected <= %llu)", clen, g_srv.max_content_length);
        return -1;  // critical error
    }
    
    bytesio_t * bio = get_bytesio_object(wsgi_input);
    if (bio) {
        Py_ssize_t rv = io_BytesIO_write_bytes(bio, body, length);
        if (rv < 0 || rv != (Py_ssize_t)length) {
            client->error = 1;
            LOGf("Failed write bytes directly to wsgi_input Stream! (ret = %d, expected = %d)", (int)rv, (int)length);
        }
        goto fin;
    }

    PyObject* body_content = PyBytes_FromStringAndSize(body, length);
    if (client->request.wsgi_input_size == 0) {
        LOGREPR(LL_TRACE, body_content);  // output only first chunk
    }
    PyObject* result = PyObject_CallMethodObjArgs(wsgi_input, g_cv.write, body_content, NULL);
    if (!PyLong_CheckExact(result)) {
        client->error = 1;
        LOGf("Cannot write PyBytes to wsgi_input stream! (ret = None)");
    } else {
        size_t rv = PyLong_AsSize_t(result);
        if (rv != length) {
            client->error = 1;
            LOGf("Failed write PyBytes to wsgi_input stream! (ret = %d, expected = %d)", (int)rv, (int)length);
        }
    }
    Py_XDECREF(result);
    Py_XDECREF(body_content);

fin:
    client->request.wsgi_input_size += (int)length;
    return (client->error) ? -1 : 0;
}

int on_message_complete(llhttp_t * parser)
{
    LOGi("%s", __func__);
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_END;
    PyObject * headers = client->request.headers;

    if (llhttp_should_keep_alive(parser)) {
        client->request.keep_alive = 1;
    } else {
        client->request.keep_alive = 0;
    }
    if (client->error) {
        if (client->request.expect_continue && client->error == HTTP_STATUS_EXPECTATION_FAILED)
            return 0;
        return -1;
    }

    if (client->request.http_content_length >= 0) {
        const int clen = client->request.http_content_length;
        const int ilen = client->request.wsgi_input_size;
        if (ilen != clen) {
            client->error = 1;
            LOGc("Received body with size %d not equal specified 'Content-Length' = %d", ilen, clen);
            return -1;
        }
    }

    PyObject * wsgi_input = NULL;
    if (client->request.wsgi_input_size > 0) {
        PyObject* result;
        wsgi_input = client->request.wsgi_input;
        // Truncate input byte stream to real request content length
        result = PyObject_CallMethodObjArgs(wsgi_input, g_cv.truncate, NULL);
        Py_DECREF(result);
        // Sets the input byte stream position back to 0
        result = PyObject_CallMethodObjArgs(wsgi_input, g_cv.seek, g_cv.i0, NULL);
        Py_DECREF(result);
    } else {
        client->request.wsgi_input_size = 0;
        if (client->request.wsgi_input_empty == NULL) {
            wsgi_input = PyObject_CallMethodObjArgs(g_cv.module_io, g_cv.BytesIO, NULL);
            client->request.wsgi_input = wsgi_input;  // object cached
        } else { 
            wsgi_input = client->request.wsgi_input_empty;
        }
    }
    PyDict_SetItem(client->request.headers, g_cv.wsgi_input, wsgi_input); // wsgi_input: refcnt 1 -> 2

    //LOGd("build_wsgi_environ");
    char buf[128];
    
    const char* method = llhttp_method_name(parser->method);
    set_header(client, g_cv.REQUEST_METHOD, method, -1, 0);

    const char* protocol = parser->http_minor == 1 ? "HTTP/1.1" : "HTTP/1.0";
    set_header(client, g_cv.SERVER_PROTOCOL, protocol, -1, 0);

    if (client->remote_addr[0])
        set_header(client, g_cv.REMOTE_ADDR, client->remote_addr, -1, 0);

    if (client->request.chunked && client->request.http_content_length < 0) {
        sprintf(buf, "%d", client->request.wsgi_input_size);
        set_header(client, g_cv.CONTENT_LENGTH, buf, -1, 0);
        // Insertion of this header is allowed because the header "Transfer-Encoding" FastWSGI server removes!
        // https://peps.python.org/pep-3333/#other-http-features
    }

    client->request.load_state = LS_OK;
    return 0;
}

// =================== call WSGI app =============================================

int call_wsgi_app(client_t * client)
{
    PyObject * headers = client->request.headers;
    
    StartResponse * start_response = PyObject_NEW(StartResponse, &StartResponse_Type);
    start_response->called = 0;
    client->start_response = start_response;

    LOGi("calling wsgi application...");
    PyObject * wsgi_body = PyObject_CallFunctionObjArgs(g_srv.wsgi_app, headers, start_response, NULL);
    LOGi("wsgi app return object: '%s'", (wsgi_body) ? Py_TYPE(wsgi_body)->tp_name : "");
    client->response.wsgi_body = wsgi_body;

    if (PyErr_Occurred() || wsgi_body == NULL) {
        return HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }
    return 0;
}

// =================== build response ============================================

int get_info_from_wsgi_response(client_t * client);
int wsgi_body_pleload(client_t * client, PyObject * wsgi_body);

int process_wsgi_response(client_t * client)
{
    int err = 0;
    client->response.wsgi_content_length = -1;
    PyObject * wsgi_body = client->response.wsgi_body;

    const char* body_type = Py_TYPE(wsgi_body)->tp_name;
    if (body_type == NULL)
        body_type = "<unknown_type_name>";

    if (PyBytes_CheckExact(wsgi_body)) {
        LOGd("wsgi_body: is PyBytes (size = %d)", (int)PyBytes_GET_SIZE(wsgi_body));
    } else {
        if (PyIter_Check(wsgi_body)) {
            LOGd("wsgi_body: is ITERATOR '%s'", body_type);
            client->response.body_iterator = wsgi_body;
        } else {
            LOGd("wsgi_body: type = '%s'", body_type);
            PyObject * iter = PyObject_GetIter(wsgi_body);
            if (iter == NULL) {
                PYTYPE_ERR("wsgi_body: has not iterable type = '%s'", body_type);
                err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
                goto fin;
            }
            client->response.body_iterator = iter;
        }
    }
    err = get_info_from_wsgi_response(client);
    if (err) {
        LOGc("response header 'Content-Length' contain incorrect value!");
        PYTYPE_ERR("response headers contain incorrect value!");
        err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        goto fin;
    }
    int len = wsgi_body_pleload(client, wsgi_body);
    if (len < 0) {
        LOGc("wsgi_body_pleload return error = %d", err);
        err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        goto fin;
    }
    err = 0;

fin:
    if (err) {
        reset_head_buffer(client);
        reset_response_body(client);
        free_start_response(client);
        Py_CLEAR(client->request.headers);
        return err;
    }
    return 0;
}

int create_response(client_t * client)
{
    int err = 0;
    LOGi("%s", __func__);
    int flags = RF_HEADERS_PYLIST;
    if (client->request.keep_alive)
        flags |= RF_SET_KEEP_ALIVE;

    int len = build_response(client, flags, 0, client->start_response, NULL, -1);
    if (len <= 0) {
        err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }
    if (err) {
        reset_head_buffer(client);
        reset_response_body(client);
    }
    free_start_response(client);
    Py_CLEAR(client->request.headers);
    return err;
}

int build_response(client_t * client, int flags, int status, const void * headers, const void * body_data, int body_size)
{
    xbuf_t * head = &client->head;
    reset_head_buffer(client);
    PyObject** body = client->response.body;
    StartResponse * response = NULL;

    if (flags & RF_HEADERS_PYLIST) {
        response = (StartResponse *)headers;
        headers = NULL;
    }
    if (response) {
        char scode[4];
        size_t status_len = 0;
        const char * status_code = PyUnicode_AsUTF8AndSize(response->status, &status_len);
        if (status_len < 3)
            return -2;
        memcpy(scode, status_code, 4);
        scode[3] = 0;
        status = atoi(scode);
    }
    if (status == 204 || status == 304) {
        body_size = 0;
        reset_response_body(client);  // forced reset body buffers
    }

    const char * status_name = llhttp_status_name(status);
    if (!status_name)
        return -3;

    char * buf = xbuf_expand(head, 128);
    head->size += sprintf(buf, "HTTP/1.1 %d %s\r\n", status, status_name);

    if (response) {
        Py_ssize_t hsize = PyList_GET_SIZE(response->headers);
        for (Py_ssize_t i = 0; i < hsize; i++) {
            PyObject* tuple = PyList_GET_ITEM(response->headers, i);

            size_t key_len = 0;
            const char * key = PyUnicode_AsUTF8AndSize(PyTuple_GET_ITEM(tuple, 0), &key_len);

            if (key_len == 14 && key[7] == '-')
                if (strcasecmp(key, "Content-Length") == 0)
                    continue;  // skip "Content-Length" header

            size_t value_len = 0;
            const char * value = PyUnicode_AsUTF8AndSize(PyTuple_GET_ITEM(tuple, 1), &value_len);

            xbuf_add(head, key, key_len);
            xbuf_add(head, ": ", 2);
            xbuf_add(head, value, value_len);
            xbuf_add(head, "\r\n", 2);

            LOGi("added header '%s: %s'", key, value);
        }
    }
    else if (headers) {
        xbuf_add_str(head, (const char *)headers);
    }

    if (flags & RF_SET_KEEP_ALIVE) {
        xbuf_add_str(head, "Connection: Keep-Alive\r\n");
    } else {
        xbuf_add_str(head, "Connection: close\r\n");
    }

    if (client->request.parser.method == HTTP_HEAD) {
        // The HEAD response does not contain a body! But may contain "Content-Length"
        reset_response_body(client);
        body_size = client->response.wsgi_content_length;
        goto end;
    }

    if (body_size == 0) {
        reset_response_body(client);
    }
    if (body_data && body_size > 0) {
        reset_response_body(client);
        PyObject * buf = PyBytes_FromStringAndSize((const char *)body_data, body_size);
        body[0] = buf;
        client->response.body_chunk_num = 1;
        client->response.body_preloaded_size = body_size;
        client->response.body_total_size = body_size;
    }
    body_size = client->response.body_total_size;  // FIXME: add support "Transfer-Encoding: chunked"

end:
    if (body_size == 0) {
        xbuf_add_str(head, "Content-Length: 0\r\n");
        LOGi("Added Header 'Content-Length: 0'");
    }
    else if (body_size > 0) {
        char * buf = xbuf_expand(head, 48);
        head->size += sprintf(buf, "Content-Length: %d\r\n", (int)body_size);
        LOGi("Added Header 'Content-Length: %d'", (int)body_size);
    }
    xbuf_add(head, "\r\n", 2);  // end of headers

    LOGt(head->data);
    LOGt_IF(body_size > 0 && client->response.body_chunk_num, PyBytes_AS_STRING(body[0]));
    client->response.headers_size = head->size;
    return head->size;
}

// ===================== aux functions ===================================================

int get_info_from_wsgi_response(client_t * client)
{
    StartResponse * response = client->start_response;
    client->response.wsgi_content_length = -1;  // unknown
    Py_ssize_t hsize = PyList_GET_SIZE(response->headers);
    for (Py_ssize_t i = 0; i < hsize; i++) {
        PyObject* tuple = PyList_GET_ITEM(response->headers, i);
        size_t key_len = 0;
        const char * key = PyUnicode_AsUTF8AndSize(PyTuple_GET_ITEM(tuple, 0), &key_len);
        size_t value_len = 0;
        const char * value = PyUnicode_AsUTF8AndSize(PyTuple_GET_ITEM(tuple, 1), &value_len);
        if (key_len == 14 && key[7] == '-' && strcasecmp(key, "Content-Length") == 0) {
            if (value_len == 0)
                return -2;  // error
            if (value_len == 1 && value[0] == '0')
                return 0;
            int clen = atoi(value);
            if (clen == 0 || clen == INT_MAX)
                return -3;  // error
            LOGi("wsgi response: content-length = %d", clen);
            client->response.wsgi_content_length = clen;
        }
    }
    return 0; // without parsing error
}

PyObject* wsgi_iterator_get_next_chunk(client_t * client, int outpyerr)
{
    if (client->response.body_iterator == NULL)
        return NULL;
    PyObject* item;
    while (item = PyIter_Next(client->response.body_iterator)) {
        if (!PyBytes_Check(item)) {
            client->error = 11;
            if (outpyerr)
                PYTYPE_ERR("wsgi_body: ITERATOR: contain item type = '%s' (expected bytes)", Py_TYPE(item)->tp_name);
            Py_DECREF(item);
            return NULL;
        }
        Py_ssize_t size = PyBytes_GET_SIZE(item);
        if (size > 0) {
            LOGd("wsgi_body: ITERATOR: get bytes size = %d", (int)size);
            return item;
        }
        Py_DECREF(item); // skip empty items
    }
    if (PyErr_Occurred()) {
        client->error = 11;
        if (outpyerr) {
            PyErr_Print();
            PyErr_Clear();
        }
    }
    return NULL;
}

int wsgi_body_pleload(client_t * client, PyObject * wsgi_body)
{
    int err = 0;
    PyObject** body = client->response.body;
    int wsgi_content_length = client->response.wsgi_content_length;

    if (PyBytes_CheckExact(wsgi_body)) {
        body[0] = wsgi_body;
        Py_INCREF(wsgi_body);  // Reasone: wsgi_body inserted into body chunks array
        client->response.body_chunk_num = 1;
        client->response.body_total_size = (int)PyBytes_GET_SIZE(wsgi_body);
        client->response.body_preloaded_size = (int)PyBytes_GET_SIZE(wsgi_body);
        return client->response.body_preloaded_size;
    }
    PyObject* iterator = client->response.body_iterator;
    if (!iterator)
        return -1;

    const char* body_type = Py_TYPE(wsgi_body)->tp_name;
    // wsgi_body types:
    // Flask: ClosingIterator, FileWrapper (buffer_size)
    // Falcon: CloseableStreamIterator (_block_size)
    if (body_type && body_type[0] == 'F' && strcmp(body_type, "FileWrapper") == 0) {
        // FIXME: add custom FileWrapper via wsgi.file_wrapper (sending unlimit body chunks)
        if (PyObject_HasAttrString(wsgi_body, "buffer_size")) {
            if (wsgi_content_length == 0) {
                LOGi("wsgi_body: detect file size = 0");
                return 0;  // zero size file
            }
            if (wsgi_content_length < 0) {
                LOGc("wsgi_body: unknown size of transferred file!");
                return -5;
            }
            if (wsgi_content_length > max_read_file_buffer_size) {
                LOGc("wsgi_body: transferred file is too large! (max len = %d)", max_read_file_buffer_size);
                return -6;
            }
            PyObject * buf_size = PyLong_FromLong(max_read_file_buffer_size);
            int error = PyObject_SetAttrString(wsgi_body, "buffer_size", buf_size);
            Py_DECREF(buf_size);
            if (error) {
                LOGc("wsgi_body: failed to change file read buffer size");
                return -8;
            }
        }
    }
    PyObject* item = NULL;
    int chunks = 0;
    while (item = wsgi_iterator_get_next_chunk(client, 1)) {
        body[chunks++] = item;
        client->response.body_chunk_num = chunks;
        client->response.body_preloaded_size += (int)PyBytes_GET_SIZE(item);
        if (chunks == max_preloaded_body_chunks + 1) {
            // FIXME: add support sending unlimit body chunks
            err = -12;  // overflow!
            break;
        }
    }
    if (client->error) {
        // wsgi_body: incorrect content
        return -13;
    }
    if (err == 0) {
        LOGi("wsgi_body: response body fully loaded! (size = %d)", client->response.body_preloaded_size);
        client->response.body_total_size = client->response.body_preloaded_size;
        return client->response.body_total_size;
    }
    if (client->response.body_preloaded_size == wsgi_content_length) {
        LOGi("wsgi_body: response body fully loaded! (SIZE = %d)", wsgi_content_length);
        client->response.body_total_size = wsgi_content_length;
        return wsgi_content_length;
    }
    // FIXME: add support send body unknown length
    client->error = 1;
    LOGc("wsgi_body: body's chunks overflow (max = %d chunks)", max_preloaded_body_chunks);
    return err;
}

void init_request_dict()
{
    char buf[32];
    sprintf(buf, "%d", g_srv.port);
    PyObject * port = PyUnicode_FromString(buf);
    PyObject * host = PyUnicode_FromString(g_srv.host);
    // only constant values!!!
    g_base_dict = PyDict_New();
    PyDict_SetItem(g_base_dict, g_cv.SCRIPT_NAME, g_cv.empty_string);
    PyDict_SetItem(g_base_dict, g_cv.SERVER_NAME, host);
    PyDict_SetItem(g_base_dict, g_cv.SERVER_PORT, port);
    //PyDict_SetItem(g_base_dict, g_cv.wsgi_input, io_BytesIO);   // not const!!!
    PyDict_SetItem(g_base_dict, g_cv.wsgi_version, g_cv.version);
    PyDict_SetItem(g_base_dict, g_cv.wsgi_url_scheme, g_cv.http_scheme);
    PyDict_SetItem(g_base_dict, g_cv.wsgi_errors, PySys_GetObject("stderr"));
    PyDict_SetItem(g_base_dict, g_cv.wsgi_run_once, Py_False);
    PyDict_SetItem(g_base_dict, g_cv.wsgi_multithread, Py_False);
    PyDict_SetItem(g_base_dict, g_cv.wsgi_multiprocess, Py_True);
    Py_DECREF(port);
    Py_DECREF(host);
}

void configure_parser_settings(llhttp_settings_t * ps)
{
    llhttp_settings_init(ps);
    ps->on_message_begin = on_message_begin;
    ps->on_url = on_url;
    ps->on_url_complete = on_url_complete;
    ps->on_header_field = on_header_field;
    ps->on_header_field_complete = on_header_field_complete;
    ps->on_header_value = on_header_value;
    ps->on_header_value_complete = on_header_value_complete;
    ps->on_headers_complete = on_headers_complete;
    ps->on_body = on_body;
    ps->on_message_complete = on_message_complete;
}
