#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cxxopts.hpp>
#include <fstream>
#include <iostream>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <limits>
#include <netdb.h>
#include <cstdio>
#include <cstdlib>
#include "../common/LoadBalancerProtocol.h"

#define BACKLOG 10
const int VID_SERVER_PORT = 8000;
const int INF = std::numeric_limits<int>::max();

int listenFD;

// Round-robin: server list (addr in network order, port in host order).
static std::vector<in_addr_t> rrAddrs;
static std::vector<uint16_t> rrPorts;
static size_t rrNext = 0;
 
// Geo: per-node info plus the all-pairs shortest path matrix.
enum NodeType { 
   CLIENT, 
   SWITCH, 
   SERVER 
};
static std::vector<NodeType> nodeTypes;
static std::vector<in_addr_t> nodeAddrs;  // 0 for NO_IP nodes
static std::vector<std::vector<long long>> dist;

static std::string addrToString(in_addr_t addr) {
    char buf[INET_ADDRSTRLEN] = {0};
    in_addr in{};
    in.s_addr = addr;
    inet_ntop(AF_INET, &in, buf, sizeof(buf));
    return buf;
}
 
static bool loadRoundRobin(const std::string &path) {
    std::ifstream f(path);
    std::string key;
    int n;
    if (!f || !(f >> key >> n) || key != "NUM_SERVERS:") return false;
 
    for (int i = 0; i < n; i++) {
        std::string ip;
        int port;
        in_addr in{};
        if (!(f >> ip >> port) || inet_pton(AF_INET, ip.c_str(), &in) != 1)
            return false;
        rrAddrs.push_back(in.s_addr);
        rrPorts.push_back(static_cast<uint16_t>(port));
    }
    return !rrAddrs.empty();
}
 
static bool loadGeo(const std::string &path) {
   std::ifstream f(path);
   std::string key;
   int numNodes, numLinks;
   if (!f || !(f >> key >> numNodes) || key != "NUM_NODES:") return false;

   for (int i = 0; i < numNodes; i++) {
      std::string type, ip;
      if (!(f >> type)) return false;
      // Tolerate an optional leading host id ("<id> TYPE IP").
      if (std::isdigit(static_cast<unsigned char>(type[0])) && !(f >> type))
         return false;
      if (!(f >> ip)) return false;

      if (type == "CLIENT") nodeTypes.push_back(CLIENT);
      else if (type == "SWITCH") nodeTypes.push_back(SWITCH);
      else if (type == "SERVER") nodeTypes.push_back(SERVER);
      else return false;

      in_addr in{};
      if (ip == "NO_IP") in.s_addr = 0;
      else if (inet_pton(AF_INET, ip.c_str(), &in) != 1) return false;
      nodeAddrs.push_back(in.s_addr);
   }

   if (!(f >> key >> numLinks) || key != "NUM_LINKS:") return false;

   dist.assign(numNodes, std::vector<long long>(numNodes, INF));
   for (int i = 0; i < numNodes; i++) dist[i][i] = 0;

   for (int i = 0; i < numLinks; i++) {
      int u, v;
      long long c;
      if (!(f >> u >> v >> c)) return false;
      if (u < 0 || v < 0 || u >= numNodes || v >= numNodes) return false;
      if (c < dist[u][v]) {
         dist[u][v] = c;
         dist[v][u] = c;
      }
   }

   // Floyd-Warshall, computed once at startup.
   for (int k = 0; k < numNodes; k++)
      for (int i = 0; i < numNodes; i++) {
         if (dist[i][k] >= INF) continue;
         for (int j = 0; j < numNodes; j++) {
               if (dist[k][j] >= INF) continue;
               if (dist[i][k] + dist[k][j] < dist[i][j])
                  dist[i][j] = dist[i][k] + dist[k][j];
         }
      }
   return true;
}
 
// Each returns false if no server can be chosen.
static bool pickRoundRobin(in_addr_t &addr, uint16_t &port) {
   if (rrAddrs.empty()) return false;
   addr = rrAddrs[rrNext];
   port = rrPorts[rrNext];
   rrNext = (rrNext + 1) % rrAddrs.size();
   return true;
}
 
static bool pickGeo(in_addr_t clientAddr, in_addr_t &addr, uint16_t &port) {
   int clientId = -1;
   for (size_t i = 0; i < nodeTypes.size(); i++) {
      if (nodeTypes[i] == CLIENT && nodeAddrs[i] == clientAddr) {
         clientId = static_cast<int>(i);
         break;
      }
   }
   if (clientId < 0) return false;

   long long best = INF;
   int bestId = -1;
   // Strict '<' over ascending ids keeps the lowest id on ties.
   for (size_t i = 0; i < nodeTypes.size(); i++) {
      if (nodeTypes[i] == SERVER && dist[clientId][i] < best) {
         best = dist[clientId][i];
         bestId = static_cast<int>(i);
      }
   }
   if (bestId < 0) return false;

   addr = nodeAddrs[bestId];
   port = VID_SERVER_PORT;
   return true;
}
 
static bool sendFull(int fd, const void *buf, size_t n) {
    const char *p = static_cast<const char *>(buf);
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, p + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}
 
static void handleClient(int fd, bool rr) {
   LoadBalancerRequest req;
   std::memset(&req, 0, sizeof(req));
   int bytes_received = recv(fd, &req, sizeof(req), MSG_WAITALL);
   if (bytes_received < 0) {
      std::perror("Receive failed: ");
      return;
   }

   uint16_t requestId = ntohs(req.request_id);
   spdlog::info("Received request for client {} with request ID {}",addrToString(req.client_addr), requestId);

   in_addr_t addr = 0;
   uint16_t port = 0;

   if (rr){
      pickRoundRobin(addr, port);
   }
   else{
      pickGeo(req.client_addr, addr, port);
   }

   LoadBalancerResponse resp;
   std::memset(&resp, 0, sizeof(resp));
   resp.videoserver_addr = addr;
   resp.videoserver_port = htons(port);
   resp.request_id = htons(requestId);

   if (sendFull(fd, &resp, sizeof(resp))) {
      spdlog::info("Responded to request ID {} with server {}:{}", requestId, addrToString(addr), port);
   }
   close(fd);
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
   listenFD = socket(res->ai_family, res->ai_socktype, res->ai_protocol); 
   
   if (listenFD < 0){
      std::perror("Error creating server socket: ");
      std::exit(1);
   }
   /*
   This prevents the address already in use error message and allows the program to reuse the port
   yes = 1 is enabling this
   */
   if (setsockopt(listenFD, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
      std::perror("setsockopt");
      std::exit(1);
   }

   if (bind(listenFD, res->ai_addr,res->ai_addrlen) < 0){
      std::perror("Bind Failed: "); // Using perror since bind sets errno if there is a failure
      std::exit(1);
   }

   // Once we have bounded to a port we no longer need the addrinfo and can deallocate memory
   freeaddrinfo(res);

   if (listen(listenFD, BACKLOG) == -1){
      std::perror("Listen Failed: ");
      std::exit(1);
   }

   spdlog::info("Load balancer started on port {}", listenPort);

}

int main(int argc, char *argv[]) {
   cxxopts::Options options("loadBalancer", "Video CDN load balancer");
   options.add_options()
      ("p,port", "Port of the load balancer", cxxopts::value<int>())
      ("g,geo", "Run in geo mode")
      ("r,rr", "Run in round robin mode")
      ("s,servers", "Path to file containing server info", cxxopts::value<std::string>());

   int port;
   bool geo, rr;
   std::string serversPath;

   cxxopts::ParseResult result;
   try{
      result = options.parse(argc,argv);
   }
   catch (const cxxopts::exceptions::exception &e){
      return 1;
   }

   if (!result.unmatched().empty()){
      std::perror("Unrecognized Args for LoadBalancer");
      return 1;
   }

   if (result.count("port") == 0) {
      std::cerr << "No lb port args";
      return 1; 
   }
   geo = result.count("geo") > 0;
   rr = result.count("rr") > 0;

   if (result.count("servers") == 0){
      std::perror("No server file specified.");
      return 1;
   }

   port = result["port"].as<int>();
   serversPath = result["servers"].as<std::string>();

   if (port < 1024 || port > 65535) {
      std::perror("Port must be in the range [1024, 65535]");
      return 1;
   }
   if (geo == rr) { 
      std::perror("Invalid selection of load balancing method (Both on or None on)");
      return 1;
   }

   //Establishing lb socket
   init_listen_socket((std::to_string(port)).c_str());

   while (true) {
      int fd = accept(listenFD, nullptr, nullptr); //Might be a source of error due to no their addr field filled in
      if (fd < 0){
         std::perror("Accept Failed: ");
         return -1;
      }
      handleClient(fd, rr);
   }
}
 