#include <iostream>
#include <string>
#include <algorithm>

int main()
{
    std::string method = "Get";

    std::cout << "method: " << method << std::endl;

    std::transform(method.begin(), method.end(), method.begin(), ::toupper);
    std::cout << "method: " << method << std::endl;
    return 0;
}

// int main(){
//     std::string msg = "GET /a/b/c HTTP/1.0";
//     std::string method;
//     std::string uri;
//     std::string version;

//     std::stringstream ss(msg);

//     ss >> method >> uri >> version;
//     std::cout << method << std::endl;
//     std::cout << uri << std::endl;
//     std::cout << version << std::endl;

//     return 0;
// }