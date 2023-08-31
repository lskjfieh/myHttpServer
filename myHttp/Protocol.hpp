#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "Util.hpp"
#include "Log.hpp"

#define SEP ": "
#define WEB_ROOT "wwwroot"
#define HOME_PAGE "index.html"
#define HTTP_VERSION "HTTP/1.0"
#define LINE_END "\r\n"
#define PAGE_404 "404.html"

#define OK 200
#define BAD_REQUEST 400
#define NOT_FOUND 404
#define SERVER_ERROR 500



static std::string Code2Desc(int code)
{
    std::string desc;
    switch (code)
    {
    case 200:
        desc = "OK";
        break;
    case 404:
        desc = "Not Found";
        break;
    default:
        break;
    }
    return desc;
}

static std::string Suffix2Desc(const std::string &suffix)
{
    static std::unordered_map<std::string, std::string> suffix2desc = {
        {".html", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".jpg", "application/x-jpg"},
        {".xml", "application/xml"}};
    auto iter = suffix2desc.find(suffix);
    if (iter != suffix2desc.end())
    {
        return iter->second;
    }
    return "text/html";
}

class HttpRequest
{
public:
    std::string request_line;
    std::vector<std::string> request_header;
    std::string blank;
    std::string request_body;

    // 解析完毕之后的结果
    std::string method;
    std::string uri; // path?args
    std::string version;

    std::unordered_map<std::string, std::string> header_kv;
    int content_length;
    std::string path;
    std::string suffix; // 资源后缀
    std::string query_string;

    bool cgi;
    int size;

public:
    HttpRequest() : content_length(0), cgi(false) {}
    ~HttpRequest() {}
};
class HttpResponse
{
public:
    std::string status_line;
    std::vector<std::string> response_header;
    std::string blank;
    std::string response_body;

    int status_code;
    int fd;
    int size;

public:
    HttpResponse() : blank(LINE_END), status_code(OK), fd(-1) {}
    ~HttpResponse() {}
};
// 读取请求，分析请求，构建响应
// IO通信
class EndPoint
{
private:
    int sock;
    HttpRequest http_request;
    HttpResponse http_response;
    bool stop;

private:
    bool RecvHttpRequestLine()
    {
        auto &line = http_request.request_line;
        if (Util::ReadLine(sock, line) > 0)
        {
            line.resize(line.size() - 1);
            LOG(INFO, http_request.request_line);
        }else{
            stop = true;
        }
        return stop;
    }
    bool RecvHttpRequestHeader()
    {
        std::string line;
        while (true)
        {
            line.clear();
            if(Util::ReadLine(sock, line) <= 0)
            {
                stop = true;
                break;
            }
            if (line == "\n")
            {
                http_request.blank = line;
                break;
            }
            line.resize(line.size() - 1);
            http_request.request_header.push_back(line);
            LOG(INFO, line);
        }
        return stop;
    }
    void ParseHttpRequestLine()
    {
        auto &line = http_request.request_line;
        std::stringstream ss(line);
        ss >> http_request.method >> http_request.uri >> http_request.version;
        auto &method = http_request.method;
        std::transform(method.begin(), method.end(), method.begin(), ::toupper);

        // LOG(INFO, http_request.method);
        // LOG(INFO, http_request.uri);
        // LOG(INFO, http_request.version);
    }
    void ParseHttpRequestHeader()
    {
        std::string key;
        std::string value;
        for (auto &iter : http_request.request_header)
        {
            if (Util::CutString(iter, key, value, SEP))
            {
                http_request.header_kv.insert({key, value});
            }
        }
    }
    bool IsNeedRecvHttpRequestBody()
    {
        auto &method = http_request.method;
        if (method == "POST")
        {
            auto &header_kv = http_request.header_kv;
            auto iter = header_kv.find("Content-Length");
            if (iter != header_kv.end())
            {
                LOG(INFO, "Post Method, Content-Lenght: " + iter->second);
                http_request.content_length = atoi(iter->second.c_str());
                return true;
            }
        }
        return false;
    }
    bool RecvHttpRequestBody()
    {
        if (IsNeedRecvHttpRequestBody())
        {
            int content_length = http_request.content_length;
            auto &body = http_request.request_body;

            char ch = 0;
            while (content_length)
            {
                ssize_t s = recv(sock, &ch, 1, 0);
                if (s > 0)
                {
                    body.push_back(ch);
                    content_length--;
                }
                else
                {
                    stop = true;
                    break;
                }
            }
            LOG(INFO, body);
        }
        return stop;
    }
    int ProcessNonCgi()
    {
        http_response.fd = open(http_request.path.c_str(), O_RDONLY);
        if (http_response.fd >= 0)
        {
            LOG(INFO, http_request.path + " open success!");
            return OK;
        }
        return 404;
    }
    int ProcessCgi()
    {
        LOG(INFO, "process cgi method");
        int code = OK;
        // 父进程数据
        auto &method = http_request.method;
        auto &query_string = http_request.query_string;
        auto &body_text = http_request.request_body;
        auto &bin = http_request.path;
        int content_length = http_request.content_length;
        auto &response_body = http_response.response_body;

        std::string query_string_env;
        std::string method_env;
        std::string content_length_env;
        // 站在父进程角度
        int input[2];
        int output[2];

        if (pipe(input) < 0)
        {
            LOG(ERROR, "pipe input error");
            code = SERVER_ERROR;
            return code;
        }
        if (pipe(output) < 0)
        {
            LOG(ERROR, "pipe output error");
            code = SERVER_ERROR;
            return code;
        }

        // 新线程进入到CGI，但是从头到尾只有httpserver，因此不能直接进行程序替换
        pid_t pid = fork();
        if (pid == 0)
        {
            // child
            //
            close(input[0]);
            close(output[1]);
            // exec*
            // 站在子进程角度
            // input[1]: 写出 -> 1 -> input[1]
            // output[1]: 读入 -> 0 -> output[0]

            method_env = "METHOD=";
            method_env += method;
            putenv((char *)method_env.c_str());

            if (method == "GET")
            {
                query_string_env = "QUERY_STRING=";
                query_string_env += query_string;
                putenv((char *)query_string_env.c_str());
                LOG(INFO, "Get Method, Add Query_String Env");
            }
            else if (method == "POST")
            {
                content_length_env = "CONTENT_LENGTH=";
                content_length_env += std::to_string(content_length);
                putenv((char *)content_length_env.c_str());
                LOG(INFO, "Post Method, Add Content_Length Env");
            }
            else
            {
                // do nothing
            }
            dup2(output[0], 0);
            dup2(input[1], 1);
            execl(bin.c_str(), bin.c_str(), nullptr);
            exit(1);
        }
        else if (pid < 0)
        {
            // error
            LOG(ERROR, "fork error");
            return 404;
        }
        else
        {
            // std::cout << "debug: "
            //           << "Use CGI Model" << std::endl;
            close(input[1]);
            close(output[0]);
            if (method == "POST")
            {
                const char *start = body_text.c_str();
                int total = 0;
                int size = 0;
                while (total < content_length && (size = write(output[1], start + total, body_text.size() - total)) > 0)
                {
                    total += size;
                }
            }
            char ch = 0;
            while (read(input[0], &ch, 1) > 0)
            {
                response_body.push_back(ch);
            }
            int status = 0;
            pid_t ret = waitpid(pid, &status, 0);
            if (ret == pid)
            {
                if (WIFEXITED(status))
                {
                    if (WEXITSTATUS(status) == 0)
                    {
                        code = OK;
                    }
                    else
                    {
                        code = BAD_REQUEST;
                    }
                }
                else
                {
                    code = SERVER_ERROR;
                }
            }
            close(input[0]);
            close(output[1]);
        }
        return code;
    }
    void HandlerError(std::string page)
    {
        http_request.cgi = false;
        // 返回对应的404页面
        http_response.fd = open(page.c_str(), O_RDONLY);
        if (http_response.fd > 0)
        {
            struct stat st;
            stat(page.c_str(), &st);
            http_request.size = st.st_size;

            std::string line = "Content-Type: text/html";
            line += LINE_END;
            http_response.response_header.push_back(line);

            line = "Content-Length: ";
            line += std::to_string(st.st_size);
            line += LINE_END;
            http_response.response_header.push_back(line);
        }
    }
    void BuildOkResponse()
    {
        std::string line = "Content-Type: ";
        line += Suffix2Desc(http_request.suffix);
        line += LINE_END;
        http_response.response_header.push_back(line);

        line = "Content-Length: ";
        if (http_request.cgi)
        {
            line += std::to_string(http_response.response_body.size());
        }
        else
        {
            line += std::to_string(http_request.size);
        }

        line += LINE_END;
        http_response.response_header.push_back(line);
    }
    void BuildHttpResponseHelper()
    {
        // 请求已经全部读完，可以直接构建响应
        // http_request;
        // http_response;
        auto &code = http_response.status_code;

        // 构建状态行
        auto &status_line = http_response.status_line;
        status_line += HTTP_VERSION;
        status_line += " ";
        status_line += std::to_string(code);
        status_line += " ";
        status_line += Code2Desc(code);
        status_line += LINE_END;

        // 构建响应正文，可能包括响应报头
        std::string path = WEB_ROOT;
        path += "/";
        switch (code)
        {
        case OK:
            BuildOkResponse();
            break;

        case NOT_FOUND:
            path += PAGE_404;
            HandlerError(path);
            break;
        case BAD_REQUEST:
            path += PAGE_404;
            HandlerError(path);
            break;
        case SERVER_ERROR:
            path += PAGE_404;
            HandlerError(path);
            break;
        // case 500:
        //     HandlerError(PAGE_500);
        //     break;

        default:
            break;
        }
    }

public:
    EndPoint(int _sock) : sock(_sock), stop(false)
    {}
    bool IsStop()
    {
        return stop;
    }
    void RecvHttpRequest()
    {
        if( (!RecvHttpRequestLine()) && (!RecvHttpRequestHeader()) ){
            ParseHttpRequestLine();
            ParseHttpRequestHeader();
            RecvHttpRequestBody();
        }       
    }

    void BuildHttpResponse()
    {
        std::string _path;
        struct stat st;
        int size = 0;
        std::size_t found = 0; // 查找后缀
        auto &code = http_response.status_code;
        if (http_request.method != "GET" && http_request.method != "POST")
        {
            // 非法请求
            LOG(WARNING, "method is not right");
            code = BAD_REQUEST;
            goto END;
        }
        if (http_request.method == "GET")
        {
            size_t pos = http_request.uri.find('?');
            if (pos != std::string::npos)
            {
                Util::CutString(http_request.uri, http_request.path, http_request.query_string, "?");
                http_request.cgi = true;
            }
            else
            {
                http_request.path = http_request.uri;
            }
        }
        else if (http_request.method == "POST")
        {
            // POST
            http_request.cgi = true;
            http_request.path = http_request.uri;
        }
        else
        {
            // Do Nothing
        }

        _path = http_request.path;
        http_request.path = WEB_ROOT;
        http_request.path += _path;
        if (http_request.path[http_request.path.size() - 1] == '/')
        {
            http_request.path += HOME_PAGE;
        }

        if (stat(http_request.path.c_str(), &st) == 0)
        {
            // 说明资源存在
            if (S_ISDIR(st.st_mode))
            {
                // 说明请求的是一个目录，不被允许的，需要做下一步相关处理
                // 虽然是一个目录，但不会以一个/结尾
                http_request.path += "/";
                http_request.path += HOME_PAGE;
                stat(http_request.path.c_str(), &st); // 更新属性
            }
            if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH))
            {
                // 特殊处理
                http_request.cgi = true;
            }
            http_request.size = st.st_size;
        }
        else
        {
            // 说明资源不存在
            LOG(WARNING, http_request.path + " Not Found.");
            code = NOT_FOUND;
            goto END;
        }

        found = http_request.path.rfind(".");
        if (found == std::string::npos)
        {
            http_request.suffix = ".html";
        }
        else
        {
            http_request.suffix = http_request.path.substr(found);
        }
        if (http_request.cgi)
        {
            code = ProcessCgi();
        }
        else
        {
            // 1. 目标网页一定存在
            // 2. 返回并不是单单返回网页，需构建HTTP响应
            code = ProcessNonCgi(); // 简单的网页返回，返回静态网页
        }
        // std::cout << "debug: " << http_request.path << std::endl;
    END:

        BuildHttpResponseHelper();
        // return;
    }
    void SendHttpResponse()
    {
        send(sock, http_response.status_line.c_str(), http_response.status_line.size(), 0);
        for (auto iter : http_response.response_header)
        {
            send(sock, iter.c_str(), iter.size(), 0);
        }
        send(sock, http_response.blank.c_str(), http_response.blank.size(), 0);

        if (http_request.cgi)
        {
            auto &response_body = http_response.response_body;
            size_t size = 0;
            size_t total = 0;
            const char *start = response_body.c_str();
            while (total < response_body.size() && (size = send(sock, start + total, response_body.size() - total, 0)) > 0)
            {
                total += size;
            }
        }
        else
        {
            std::cout << "-------------" << std::endl;
            sendfile(sock, http_response.fd, nullptr, http_request.size);
            close(http_response.fd);
        }
    }
    ~EndPoint()
    {
        close(sock);
    }
};
// #define DEBUG 1
class CallBack
{
public:
    CallBack()
    {}
    void operator()(int sock)
    {
        HandlerRequest(sock);
    }
    void HandlerRequest(int sock)
    {
        LOG(INFO, "Hander Request Begin");

#ifdef DEBUG

        char buffer[4096];
        recv(sock, buffer, sizeof(buffer), 0);
        std::cout << "--------------------------begin---------------------------" << std::endl;
        std::cout << buffer << std::endl;
        std::cout << "---------------------------end----------------------------" << std::endl;
#else
        EndPoint *ep = new EndPoint(sock);
        ep->RecvHttpRequest();
        if(!ep->IsStop()){
            LOG(INFO, "Recv No Error, Begin Build And Send");
            ep->BuildHttpResponse();
            ep->SendHttpResponse();
        }else{
            LOG(WARNING, "Recv Error, Stop Build And Send");
        }
        delete ep;

#endif
        LOG(INFO, "Header Request End");
    }
    ~CallBack(){}
};
