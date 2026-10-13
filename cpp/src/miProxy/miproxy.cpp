#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <chrono>
#include <unistd.h>
#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include "../common/network_utils.h"
#include <pugixml.hpp>
#include <algorithm>

#define BACKLOG 10
#define MAX_MESSAGE_SIZE 1024
#define DATACHUNK 80000

struct connectionState{
   int connectedTo = -1;
   int bitrate = 0;
   bool is_client = false;
   bool head_parsed = false;
   std::string readBuffer;
};

void init_listen_socket(const char* listenPort);
int accept_browser_clients();
void run(const char* vidServerPort, const char* vidServerHost, double alpha);
void connect_to_videoserver(const char* vidServerPort, const char* validServerHost, int browserConnectionFD);
void handle_client_activity(fd_set &readfds, double alpha);

std::unordered_map<int, int> msg_counts; //Holds all socketFDs from browser client and proxy client
std::unordered_map<std::string, std::vector<int>> availBitrates;
std::unordered_map<int, connectionState> connectionStatus;
std::unordered_map<std::string, double> throughputs;
int sockServerFD;
bool flag;

int main(int argc, char* argv[]){
   cxxopts::Options options("miProxy");
   options.add_options()
      ("h,hostname", "Video server IP", cxxopts::value<std::string>())
      ("p,port", "Vid Server port number", cxxopts::value<int>())
      ("l,listen-port", "Proxy port number", cxxopts::value<int>())
      ("a,alpha", "Coefficient in EW<A throughput estimate", cxxopts::value<double>());

   cxxopts::ParseResult result;
   try {
      result = options.parse(argc, argv);
   }
   catch (const cxxopts::exceptions::exception &e) {
      return 1;
   }

   // Any leftover unrecognized args.
   if (!result.unmatched().empty()) {
      std::cerr << "Unrecognized arguments";
      return 1;
   }

   if (result.count("port") == 0) {
      std::cerr << "No video server port args";
      return 1; 
   }
   if (result.count("listen-port") == 0) {
      std::cerr << "No proxy port args";
      return 1; 
   }

   double alpha = result["alpha"].as<double>();
   int vsPort = result["port"].as<int>();
   int proxyPort = result["listen-port"].as<int>();
   std::string vidServerHost = result["hostname"].as<std::string>();

   if (vsPort < 1024 || vsPort > 65535) {
      std::cerr << "Error: port number must be in the range of [1024, 65535]";
      return 1;
   }

   if (alpha < 0 || alpha > 1) {
      std::cerr << "Alpha not in range [0,1]";
      return 1; 
   }

   if (proxyPort < 1024 || proxyPort > 65535) {
      std::cerr << "Error: port number must be in the range of [1024, 65535]";
      return 1;
   }

   std::string validProxyPort = (std::to_string(proxyPort));
   std::string vidServerPort = (std::to_string(vsPort));
   init_listen_socket(validProxyPort.c_str());
   run(vidServerPort.c_str(), vidServerHost.c_str(), alpha);

   return 0;

}

void run(const char* vidServerPort, const char* vidServerHost, double alpha){
   flag = true;
   int browserConnectionFD;
   while(flag){
      fd_set readfds;
      FD_ZERO(&readfds);

      // Add the listening socket to the set.
      FD_SET(sockServerFD, &readfds);

      // Add client sockets to the set.
      for (const auto &entry : msg_counts) {
         FD_SET(entry.first, &readfds);
      }

      // Wait for activity on any socket.
      /*
      before select() there is a bit per FD that is set to 1 if it is in the readfds
      after select() only the fds that have activity such as a send or accept have their bits set to 1, everything else 0
      FD_ISSET() checks that bit to see if it is 1, if so check the activity
      */
      int activity = select(FD_SETSIZE, &readfds, nullptr, nullptr, nullptr);
      if (activity < 0 && errno != EINTR) {
         std::cerr << "select() failed: " << std::strerror(errno) << std::endl;
         continue;
      }

      // since FD set contains the client sockets AND the listening socket, if this socket has activity and selected
      //    it means that the listening socket detects a new connection and thus you call accept_browser_clients()
      if (FD_ISSET(sockServerFD, &readfds)) {
         browserConnectionFD = accept_browser_clients();
         if (browserConnectionFD < 0){
            std::perror("Error when connecting with browser client");
            std::exit(1);
         }
         connect_to_videoserver(vidServerPort, vidServerHost, browserConnectionFD);
      }

      // At this point, there is only client socket activity
      handle_client_activity(readfds, alpha);
   }
   
}

void init_listen_socket(const char* listenPort){
   struct addrinfo hints, *res;
   const char *proxyIP = "0.0.1.0";
   int yes = 1;

   //Setting up the server socket
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = AI_PASSIVE;

   getaddrinfo(NULL, listenPort, &hints, &res);

   /*
      *AF_INET is for IPv4 protocol in the network layer, SOCK_STREAM establishes 2 way communication, IPPROTO_TCP is just TCP setup
      *IPPROTO_TCP is optional since it is already assumed by using SOCK_STREAM

   One thing to note is that this socket is soley responsible for listening for incoming connection attempts
      when a client connects, accept() returns a new socket with a different file descripter which is used
      for the actual communication with that client
   */
   sockServerFD = socket(res->ai_family, res->ai_socktype, res->ai_protocol); 
   
   if (sockServerFD < 0){
      std::perror("Error creating server socket: ");
      std::exit(1);
   }
   /*
   This prevents the address already in use error message and allows the program to reuse the port
   yes = 1 is enabling this
   */
   if (setsockopt(sockServerFD, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) {
      std::perror("setsockopt");
      std::exit(1);
   }

   if (bind(sockServerFD, res->ai_addr,res->ai_addrlen) < 0){
      std::perror("Bind Failed: "); // Using perror since bind sets errno if there is a failure
      std::exit(1);
   }

   // Once we have bounded to a port we no longer need the addrinfo and can deallocate memory
   freeaddrinfo(res);

   if (listen(sockServerFD, BACKLOG) == -1){
      std::perror("Listen Failed: ");
      std::exit(1);
   }

   spdlog::info("miProxy started");

}

int accept_browser_clients(){
   sockaddr_in their_addr;
   socklen_t addr_size = sizeof(their_addr);
   std::string client_ip;
   int client_port;

   int browserConnectionFD = accept(sockServerFD, (struct sockaddr *)&their_addr, &addr_size);
   if (browserConnectionFD < 0){
      std::perror("Accept Failed: ");
      return -1;
   }

   msg_counts.emplace(browserConnectionFD,0);
   connectionStatus.emplace(browserConnectionFD, connectionState{});

   client_port = get_client_port_number(&their_addr);
   client_ip = get_client_IP(&their_addr);
   spdlog::info("New client socket connected with {}:{} on sockfd {}", client_ip, client_port, browserConnectionFD); 

   return browserConnectionFD;

}

void connect_to_videoserver(const char* vidServerPort, const char* validServerHost, int browserConnectionFD){
   struct sockaddr_in vidServerAddr;
   struct addrinfo hints, *res;
   int clientProxyFD;

   memset(&hints, 0, sizeof hints);
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   getaddrinfo(validServerHost, vidServerPort, &hints, &res);

   clientProxyFD = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   
   if(connect(clientProxyFD, res->ai_addr, res->ai_addrlen) < 0){
      std::perror("Connection Failed: ");
      exit(1);
   }

   msg_counts.emplace(clientProxyFD, 0);
   connectionStatus.emplace(clientProxyFD, connectionState{browserConnectionFD});
   auto it = connectionStatus.find(browserConnectionFD);
   if (it != connectionStatus.end()){
      it->second.connectedTo = clientProxyFD;
      it->second.is_client = true;
   }
   else{
      std::cout << "connect_to_videoserver: Could not find browser FD\n";
   }

   std::cout << "Connected to Video Server !\n";
   
}

void handle_client_activity(fd_set &readfds, double alpha){
   for (auto it = msg_counts.begin(); it != msg_counts.end();){
      int sockfd = it->first;
      int &num_messages = it->second;

      if (FD_ISSET(sockfd, &readfds)){
         //Proxy receives from the client 
         if (connectionStatus[sockfd].is_client){
            char buffer[MAX_MESSAGE_SIZE+1] = {};
            int bytes_received = recv(sockfd, &buffer, MAX_MESSAGE_SIZE+1, 0);

            if (bytes_received == 0){
               spdlog::info("Client socket sockfd {} disconnected", sockfd);
               int serverFD = connectionStatus[sockfd].connectedTo;
               close(sockfd); //closing the client socket
               close(serverFD); //closing the proxy side socket for the vid server
               connectionStatus.erase(serverFD);
               connectionStatus.erase(sockfd);
               it = msg_counts.erase(it);
               msg_counts.erase(serverFD);
               spdlog::info("Client socket sockfd {} disconnected", sockfd); 
            }
            else if (bytes_received < 0){
               std::perror("Receive failed: ");
               exit(1);
            }
            else{
               connectionState &state = connectionStatus[sockfd];
               std::string uuid, size_str, start_str, end_str;
               
               state.readBuffer.append(buffer, bytes_received);

               if (!state.head_parsed){
                  size_t header_end_pos = state.readBuffer.find("\r\n\r\n");

                  if (header_end_pos != std::string::npos) {
                     state.head_parsed = true;

                     // Extract header section and body start position
                     std::string headers = state.readBuffer.substr(0, header_end_pos);
                     // skips the delimeter and marks the start of the body
                     size_t body = header_end_pos + 4; 

                     std::string requestType = get_first_line(headers);
                     size_t content_length = extract_content_length(headers);

                     // Calculate if any body bytes were overread into the buffer
                     size_t byteOverlap = 0;
                     if (state.readBuffer.size() > body){
                        byteOverlap = state.readBuffer.size() - body;
                     }
                     if (content_length > byteOverlap) {
                        size_t bytes_still_needed = content_length - byteOverlap;
                        
                        std::vector<char> remaining_body(bytes_still_needed);
                        ssize_t remainder_recv = recv(sockfd, remaining_body.data(), bytes_still_needed, MSG_WAITALL);
                        
                        if (remainder_recv > 0) {
                           state.readBuffer.append(remaining_body.data(), remainder_recv);
                        }
                        
                     }

                     if (requestType.find("GET") != std::string::npos) {
                        if (requestType.find(".mpd") != std::string::npos) {
                           // Extract the path
                           std::string vidPath = parse_request_line(requestType);
                           //spdlog::info("Manifest requested by {} forwarded to {}:{} for {}", client_uuid, get_IP(state.connectedTo), get_port_number(state.connectedTo), chunkname); 

                           // First-ever request for this video, fetch vid.mpd for the proxy's own use
                           if (availBitrates.find(vidPath) == availBitrates.end()) {
                              send(state.connectedTo, state.readBuffer.data(), state.readBuffer.size(), 0);

                              // Block until headers + Content-Length bytes have arrived
                              std::string resp;
                              char buf[4096];
                              size_t hdrEnd = std::string::npos, len = 0;
                              while (hdrEnd == std::string::npos || resp.size() < hdrEnd + 4 + len) {
                                 ssize_t n = recv(state.connectedTo, buf, sizeof(buf), 0);
                                 if (n <= 0) break;
                                 resp.append(buf, n);
                                 if (hdrEnd == std::string::npos && (hdrEnd = resp.find("\r\n\r\n")) != std::string::npos)
                                    len = extract_content_length(resp.substr(0, hdrEnd));
                              }

                              // Parse the video bitrates only (skips audio)
                              if (hdrEnd != std::string::npos) {
                                 pugi::xml_document doc;
                                 if (doc.load_string(resp.substr(hdrEnd + 4).c_str())) {
                                    pugi::xpath_node_set reps = doc.select_nodes(
                                       "//AdaptationSet[@contentType='video' or starts-with(@mimeType, 'video')]/Representation");

                                    std::vector<int> rates;
                                    for (pugi::xpath_node xn : reps)
                                       rates.push_back(xn.node().attribute("bandwidth").as_int());
                                    availBitrates[vidPath] = rates;
                                 }
                              }
                           }

                           size_t p = state.readBuffer.find("vid.mpd");
                           if (p != std::string::npos) state.readBuffer.replace(p, 7, "vid-no-list.mpd");
                        }
                        else if (requestType.find(".m4s") != std::string::npos) {
                           std::string vidPath = parse_request_line(requestType);

                           // 1. Process headers line-by-line using a stream wrapper
                           if (vidPath.find("audio") == std::string::npos){
                              std::istringstream stream(state.readBuffer);
                              std::string line;
                              
                              // Skip the initial POST / request status line
                              std::getline(stream, line); 
                              
                              while (std::getline(stream, line) && line != "\r") {
                                 // Strip trailing carriage return if present
                                 if (!line.empty() && line.back() == '\r') {
                                    line.pop_back();
                                 }

                                 size_t colon = line.find(':');
                                 if (colon == std::string::npos) continue;

                                 // Extract key and lowercase it for reliable comparison
                                 std::string key = line.substr(0, colon);
                                 std::transform(key.begin(), key.end(), key.begin(), ::tolower);

                                 // Extract raw value and trim leading space safely
                                 std::string value = line.substr(colon + 1);
                                 size_t first_char = value.find_first_not_of(" \t");
                                 if (first_char != std::string::npos) {
                                    value = value.substr(first_char);
                                 }

                                 // Map keys directly to your target variables
                                 if (key == "x-489-uuid"){
                                    uuid = value;
                                 }
                              }
                              

                              double minThroughput = throughputs[uuid] / 1.5;
                              int lowestBitRate = *(std::min_element(availBitrates[vidPath].begin(),availBitrates[vidPath].end()));
                              int goingBitRate = lowestBitRate;
                              for (int bitRate : availBitrates[vidPath]){
                                 if (minThroughput > bitRate){
                                    goingBitRate = bitRate;
                                 }
                              }
                              
                              //change the request to the server with this bitrate
                              std::string oldFilePattern = "vid-" + std::to_string(lowestBitRate) + "-";
                              std::string newFilePattern = "vid-" + std::to_string(goingBitRate) + "-";

                              // Find where the old bitrate sits inside the first request line or the text buffer
                              size_t reqLinePos = state.readBuffer.find(oldFilePattern);
                              if (reqLinePos != std::string::npos) {
                                 state.readBuffer.replace(reqLinePos, oldFilePattern.size(), newFilePattern);
                              }
                              
                           }
                        }
                     } 
                     else if (requestType.find("POST") != std::string::npos) {
                        if (requestType.find("/on-fragment-received") != std::string::npos) {
                           //Parse to get the byte size and time diff 
                           std::string uuid, size_str, start_str, end_str;

                           // 1. Process headers line-by-line using a stream wrapper
                           std::istringstream stream(headers);
                           std::string line;
                           
                           // Skip the initial POST / request status line
                           std::getline(stream, line); 

                           while (std::getline(stream, line) && line != "\r") {
                              // Strip trailing carriage return if present
                              if (!line.empty() && line.back() == '\r') {
                                 line.pop_back();
                              }

                              size_t colon = line.find(':');
                              if (colon == std::string::npos) continue;

                              // Extract key and lowercase it for reliable comparison
                              std::string key = line.substr(0, colon);
                              std::transform(key.begin(), key.end(), key.begin(), ::tolower);

                              // Extract raw value and trim leading space safely
                              std::string value = line.substr(colon + 1);
                              size_t first_char = value.find_first_not_of(" \t");
                              if (first_char != std::string::npos) {
                                 value = value.substr(first_char);
                              }

                              // Map keys directly to your target variables
                              if (key == "x-489-uuid"){
                                 uuid = value;
                              }
                              else if (key == "x-fragment-size"){
                                 size_str = value;
                              }
                              else if (key == "x-timestamp-start"){
                                 start_str = value;
                              }
                              else if (key == "x-timestamp-end"){
                                 end_str = value;
                              }
                           }

                           // 2. Perform conversions and update throughput metrics
                           if (!uuid.empty() && !size_str.empty() && !start_str.empty() && !end_str.empty()) {
                              try {
                                    long long elapsedMs = std::stoll(end_str) - std::stoll(start_str);
                                    if (elapsedMs > 0) {
                                       double tNew = (std::stod(size_str) * 8.0) / elapsedMs;
                                       double &tCur = throughputs[uuid]; // Ensure alpha and throughputs are visible globally
                                       tCur = alpha * tNew + (1.0 - alpha) * tCur;
                                    }
                              } catch (const std::exception &e) {
                                    std::cerr << "Header value conversion failed: " << e.what() << std::endl;
                              }
                           }
                           state.readBuffer.append("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
                           send(sockfd, state.readBuffer.data(), state.readBuffer.size(), 0);
                           return;
                        }
                     }
                     size_t sent = 0;
                     while (sent < state.readBuffer.size()){
                        ssize_t n = send(state.connectedTo, state.readBuffer.data() + sent, state.readBuffer.size() - sent, 0);
                        if (n < 0) { std::perror("send to server"); break; }
                        sent += n;
                     }
                     state.readBuffer.clear();
                     state.head_parsed = false;
                  }
               }
            }
         }
         else{
            char buffer[4096] = {};
            ssize_t bytes_received = recv(sockfd, buffer, sizeof(buffer), 0);

            if (bytes_received > 0) {
               int client_fd = connectionStatus[sockfd].connectedTo;
               
               // Loop to ensure all received bytes are fully sent (handling partial writes)
               size_t total_sent = 0;
               while (total_sent < static_cast<size_t>(bytes_received)) {
                  ssize_t bytes_sent = send(client_fd, buffer + total_sent, bytes_received - total_sent, 0);
                  
                  if (bytes_sent < 0) {
                     std::perror("Send to client failed: ");
                     break;
                  }
                  total_sent += bytes_sent;
               }
            }
            else if (bytes_received == 0) {
               int clientFD = connectionStatus[sockfd].connectedTo;
               spdlog::info("Video server socket sockfd {} disconnected", sockfd);
               close(sockfd);
               close(clientFD);
               connectionStatus.erase(clientFD);
               connectionStatus.erase(sockfd);
               msg_counts.erase(clientFD);
               it = msg_counts.erase(it);
            }
         }
         break;
      }
      ++it;
   }
}