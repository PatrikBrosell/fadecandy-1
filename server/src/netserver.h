/*
 * Open Pixel Control and HTTP server for Fadecandy
 * 
 * Copyright (c) 2013 Micah Elizabeth Scott
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once
#include <stdint.h>
#include <list>
#include "tinythread.h"
#include "libwebsockets.h"
#include "opc.h"


class NetServer {
public:
    NetServer(OPC::callback_t messageCallback, void *context, bool verbose = false);

    // Start the event loop on a separate thread
    bool start(const char *host, int port);

private:
    enum ClientState {
        CLIENT_STATE_PROTOCOL_DETECT = 0,
        CLIENT_STATE_OPEN_PIXEL_CONTROL,
        CLIENT_STATE_HTTP
    };

    // In-memory database of static files to serve over HTTP
    struct HTTPDocument {
        const char *path;
        const char *body;
        const char *contentType;
    };

    struct Client {
        ClientState state;

        // HTTP response state
        const char *httpBody;

        // Low-level OPC and protocol detection receive buffer
        unsigned bufferLength;
        uint8_t buffer[2 * sizeof(OPC::Message)];
    };

    OPC::callback_t mMessageCallback;
    void *mUserContext;
    tthread::thread *mThread;
    bool mVerbose;

    static HTTPDocument httpDocumentList[];

    static void threadFunc(void *arg);

    // HTTP Server
    static int httpCallback(libwebsocket_context *context, libwebsocket *wsi,
        enum libwebsocket_callback_reasons reason, void *user, void *in, size_t len);
    int httpBegin(libwebsocket_context *context, libwebsocket *wsi, Client &client, const char *path);
    int httpWrite(libwebsocket_context *context, libwebsocket *wsi, Client &client);
    static bool httpPathEqual(const char *a, const char *b);

    // Open Pixel Control server
    int opcRead(libwebsocket_context *context, libwebsocket *wsi, Client &client, uint8_t *in, size_t len);
};
