#include "network_utils.h"

#include <stdio.h>      // perror(), fprintf()
#include <string.h>     // memcpy()
#include <sys/socket.h> // getsockname(), send()
#include <unistd.h>     // close()
#include <string>

int make_server_sockaddr(struct sockaddr_in *addr, int port) {
    // (1) This is an Internet socket
    addr->sin_family = AF_INET;
    // (2) Server sockets listen; let the OS figure out the IP addres
    addr->sin_addr.s_addr = INADDR_ANY;
    // (3) Tell the socket to listen on the specified port
    addr->sin_port = htons(port);
    return 0;
}

int make_client_sockaddr(struct sockaddr_in *addr, const char *hostname,
                         int port) {
    // (1) This is an Internet socket
    addr->sin_family = AF_INET;

    // (2) Figure out the hostname, and assign it to the socket
    struct hostent *host = gethostbyname(hostname);
    if (host == nullptr) {
        fprintf(stderr, "%s: unknown host\n", hostname);
        return -1;
    }
    memcpy(&(addr->sin_addr), host->h_addr, host->h_length);

    // Step (3): Set the port value.
    addr->sin_port = htons(port);
    return 0;
}

int get_port_number(int sockfd) {
    struct sockaddr_in addr;
    socklen_t length = sizeof(addr);
    if (getsockname(sockfd, (sockaddr *)&addr, &length) == -1) {
        perror("Error getting port of socket");
        return -1;
    }
    return ntohs(addr.sin_port);
}

std::string get_first_line(const std::string& request) {
    size_t pos = request.find("\r\n");
    if (pos == std::string::npos) {
        pos = request.find('\n');
    }
    if (pos == std::string::npos) {
        return request;
    }
    return request.substr(0, pos);
}

// 3. Helper: Extract Content-Length case-insensitively
size_t extract_content_length(const std::string& headers) {
    std::stringstream ss(headers);
    std::string line;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::string lower_line = line;
        std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);

        if (lower_line.rfind("content-length:", 0) == 0) {
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string val_str = line.substr(colon_pos + 1);
                val_str.erase(0, val_str.find_first_not_of(" \t"));
                val_str.erase(val_str.find_last_not_of(" \t") + 1);

                try {
                    return std::stoul(val_str);
                } catch (...) {
                    return 0;
                }
            }
        }
    }
    return 0;
}

std::string get_IP(int sockfd) {
    struct sockaddr_in addr;
    socklen_t length = sizeof(addr);
    if (getsockname(sockfd, (sockaddr *)&addr, &length) == -1) {
        perror("Error getting port of socket");
        return "-1";
    }
    return std::string(inet_ntoa(addr.sin_addr));
}

std::string parse_request_line(const std::string &line){
   std::istringstream iss(line);
   std::string target, filler;
   iss >> filler >> target;

   size_t videoFolderPos = target.find("/video/");
   if (videoFolderPos != std::string::npos) {
      return target.substr(0, videoFolderPos + 1); // Returns up to "/videos/michigan-hype-video/"
   }

   // 3. Otherwise, if it is the .mpd path, strip off the file name at the final slash
   size_t lastSlashPos = target.find_last_of('/');
   if (lastSlashPos != std::string::npos) {
      return target.substr(0, lastSlashPos + 1); // Returns up to "/videos/michigan-hype-video/"
   }
   return target;
}

int send_data(int sockfd, std::string_view bytes) {
    size_t sent = 0;
    do {
        const ssize_t n =
            send(sockfd, bytes.data() + sent, bytes.size() - sent, 0);
        if (n < 0) {
            return -1;
        }
        sent += n;
    } while (sent < bytes.size());
    return 0;
}

int get_client_port_number(struct sockaddr_in *their_addr){
    return ntohs(their_addr->sin_port);
}

std::string get_client_IP(struct sockaddr_in *their_addr){
    return std::string(inet_ntoa(their_addr->sin_addr));
}
