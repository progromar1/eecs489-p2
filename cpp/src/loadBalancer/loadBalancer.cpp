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

static std::vector<in_addr_t> rrAddrs;
static std::vector<uint16_t> rrPorts;
static size_t rrNext = 0;
 
enum NodeType { 
   CLIENT, 
   SWITCH, 
   SERVER 
};
static std::vector<NodeType> nodeTypes;
static std::vector<in_addr_t> nodeAddrs; 
static std::vector<std::vector<int>> distanceMatrix;

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

   f >> key >> n;

   for (int i = 0; i < n; i++) {
      std::string ip;
      int port;
      in_addr in{};
      f >> ip >> port;
      if (inet_pton(AF_INET, ip.c_str(), &in) != 1){
         return false;
      }
      rrAddrs.push_back(in.s_addr);
      rrPorts.push_back(static_cast<uint16_t>(port));
   }
   return !rrAddrs.empty();
}
 
static bool loadGeo(const std::string &path) {
   std::ifstream f(path);
   std::string key;
   int numNodes, numLinks;
   if (!f){
      return false;
   }
   f >> key >> numNodes;
   for (int i = 0; i < numNodes; i++) {
      std::string type, ip;
      f >> type >> ip;

      std::cout << type << " " << ip << std::endl;

      if (type == "CLIENT"){
         nodeTypes.push_back(CLIENT);
      }
      else if (type == "SWITCH"){
         nodeTypes.push_back(SWITCH);
      }
      else if (type == "SERVER"){
         nodeTypes.push_back(SERVER);
      }
      else{
         return false;
      }

      in_addr in{};
      if (ip == "NO_IP"){
         in.s_addr = 0;
      }
      else if(inet_pton(AF_INET, ip.c_str(), &in) != 1){
         return false;
      }
      nodeAddrs.push_back(in.s_addr);
   }

   f >> key >> numLinks;

   distanceMatrix.assign(numNodes, std::vector<int>(numNodes, INF));

   //Floyd Warshall init
   for (int i = 0; i < numNodes; i++){
      distanceMatrix[i][i] = 0;
   }
   for (int i = 0; i < numLinks; i++) {
      int node1, node2, weight;
      f >> node1 >> node2 >> weight;
      if (node1 < 0 || node2 < 0 || node1 >= numNodes || node2 >= numNodes){
         return false;
      }
      if (weight < distanceMatrix[node1][node2]) {
         distanceMatrix[node1][node2] = weight;
         distanceMatrix[node2][node1] = weight;
      }
   }

   //Floyd Warshall actual algo
   for (int intermediateNode = 0; intermediateNode < numNodes; intermediateNode++){
      for (int i = 0; i < numNodes; i++) {
         if (distanceMatrix[i][intermediateNode] >= INF){
            continue;
         }
         for (int j = 0; j < numNodes; j++) {
               if (distanceMatrix[intermediateNode][j] >= INF){
                  continue;
               }
               if (distanceMatrix[i][intermediateNode] + distanceMatrix[intermediateNode][j] < distanceMatrix[i][j])
                  distanceMatrix[i][j] = distanceMatrix[i][intermediateNode] + distanceMatrix[intermediateNode][j];
         }
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
         clientId = int(i);
         break;
      }
   }
   if (clientId < 0) return false;

   int best = INF;
   int bestId = -1;
   
   for (size_t i = 0; i < nodeTypes.size(); i++) {
      if (nodeTypes[i] == SERVER && distanceMatrix[clientId][i] < best) {
         best = distanceMatrix[clientId][i];
         bestId = int(i);
      }
   }
   if (bestId < 0){
      return false;
   }

   addr = nodeAddrs[bestId];
   port = VID_SERVER_PORT;
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
      if (!pickRoundRobin(addr, port)){
         spdlog::info("Failed to fulfill request ID {}", requestId);
         close(fd);
      }
   }
   else{
      if (!pickGeo(req.client_addr, addr, port)){
         spdlog::info("Failed to fulfill request ID {}", requestId);
         close(fd);
      }
   }

   LoadBalancerResponse resp;
   std::memset(&resp, 0, sizeof(resp));
   resp.videoserver_addr = addr;
   resp.videoserver_port = htons(port);
   resp.request_id = htons(requestId);

   const char* p = reinterpret_cast<const char*>(&resp);

   int totalBytes = sizeof(resp);
   int sent = 0;
   while (sent < totalBytes) {
      int bs = send(fd, p + sent, totalBytes - sent, 0);
      if (bs <= 0){
         return;
      }
      sent += bs;
   }
   spdlog::info("Responded to request ID {} with server {}:{}", requestId, addrToString(addr), port);
   
   close(fd);
}

void init_listen_socket(const char* listenPort){
   struct addrinfo hints, *res;
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

   std::cout << geo << " " << rr << std::endl;
   if (rr){
      loadRoundRobin(serversPath);
   }
   if (geo){
      loadGeo(serversPath);
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
 