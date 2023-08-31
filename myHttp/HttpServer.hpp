#pragma once
#include <iostream>
#include <pthread.h>
#include <signal.h>
#include "Log.hpp"
#include "TcpServer.hpp"
#include "Task.hpp"
#include "ThreadPool.hpp"

#define PORT 8081

class HttpServer{

private:
    int port;
    bool stop;

public:
    HttpServer(int _port = PORT) : port(_port),  stop(false)
    {}
    void InitServer()
    {
        signal(SIGPIPE, SIG_IGN);
        // tcp_server = TcpServer::getinstance(port);
    }
    void Loop()
    {
        LOG(INFO, "Loop begin");
        while (!stop)
        {
            TcpServer *tsrv = TcpServer::getinstance(port);
            struct sockaddr_in peer;
            socklen_t len = sizeof(peer);
            int sock = accept(tsrv->Sock(), (struct sockaddr*)&peer, &len);
            if(sock < 0)
            {
                continue;
            }
            LOG(INFO, "Get a new link");
            Task task(sock);
            ThreadPool::getinstance()->PushTask(task);
        }
    }
    ~HttpServer()
    {}
};