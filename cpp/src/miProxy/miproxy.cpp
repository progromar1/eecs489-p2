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

#define BACKLOG 10
#define MAXLENBUFFER 1
#define DATACHUNK 80000


void init_listen_socket(const char* listenPort);
void accept_browser_clients();
void run();
std::unordered_map<int, int> client_msg_counts;
int sockServerFD;
bool flag;

int main(int argc, char* argv[]){
   cxxopts::Options options("miProxy");
   options.add_options()
      //("h,hostname", "Server hostname/IP (client mode)", cxxopts::value<std::string>())
      ("p,port", "Port number", cxxopts::value<int>())
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
      std::cerr << "No port args";
      return 1; 
   }
   if (result["alpha"].as<double>() < 0 || result["alpha"].as<double>() > 1) {
      std::cerr << "Alpha not in range [0,1]";
      return 1; 
   }

   int listenPort = result["port"].as<int>();

   if (listenPort < 1024 || listenPort > 65535) {
      std::cerr << "Error: port number must be in the range of [1024, 65535]";
      return 1;
   }
   std::string validPort = (std::to_string(listenPort));
   init_listen_socket(validPort.c_str());

   run();

   return 0;

}

void run(){
   flag = true;
   int counter = 0;
   while(flag){
      fd_set readfds;
      FD_ZERO(&readfds);

      // Add the listening socket to the set.
      FD_SET(sockServerFD, &readfds);

      // Add client sockets to the set.
      for (const auto &entry : client_msg_counts) {
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
            std::cerr << "select() failed: " << std::strerror(errno)
                     << std::endl;
            continue;
      }

      // since FD set contains the client sockets AND the listening socket, if this socket has activity and selected
      //    it means that the listening socket detects a new connection and thus you call accept_browser_clients()
      if (FD_ISSET(sockServerFD, &readfds)) {
            accept_browser_clients();
            counter+=1;
            std::cout << counter << std::endl;
      }

      // At this point, there is only client socket activity

   }
   
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

   std::cout << get_IP(sockServerFD) << std::endl;

   spdlog::info("miProxy started");

}

void accept_browser_clients(){
   sockaddr_in their_addr;
   socklen_t addr_size = sizeof(their_addr);
   std::string client_ip;
   int client_port;

   int clientFD = accept(sockServerFD, (struct sockaddr *)&their_addr, &addr_size);
   if (clientFD < 0){
      std::perror("Accept Failed: ");
      std::exit(1);
   }

   client_msg_counts.emplace(clientFD,0);
   client_port = get_client_port_number(&their_addr);
   client_ip = get_client_IP(&their_addr);
   spdlog::info("New client socket connected with {}:{} on sockfd {}", client_ip, client_port, clientFD); 

}

/*
void handle_client_activity(fd_set &readfds){
   for (auto it : client_msg_counts.begin()){
      int client_sockfd = it->first;
      int &num_messages = it->second;

      if (FD_ISSET(client_sockfd, &readfds)){
         
      }
   }
}
*/

/*
int runServer(const char* listenPort){
   char serverBuf[MAXLENBUFFER] = {0};
   char serverMsg = 'A';
   size_t totalReceived = 0;
   ssize_t numBytesIn = 0;
   ssize_t numBytesOut = 0;
   char chunk[DATACHUNK] = {'\0'};

   //Start
   struct sockaddr_storage their_addr;
   socklen_t addr_size = sizeof(their_addr);
   int communicationFD = accept(sockServerFD, (struct sockaddr *)&their_addr, &addr_size);
   if (communicationFD < 0){
      std::perror("Accept Failed: ");
      return -1;
   }
   spdlog::info("Client connected");

   //Write the loop 8 times for estimating RTT
   
   numBytesIn = recv(communicationFD, serverBuf, MAXLENBUFFER, MSG_WAITALL); // receives client's C0

   for (int i = 0; i < 7; ++i){
      auto start = std::chrono::high_resolution_clock::now();
      numBytesOut = send(communicationFD, &serverMsg, 1, 0);
      numBytesIn = recv(communicationFD, &serverBuf, MAXLENBUFFER, MSG_WAITALL);
      auto end = std::chrono::high_resolution_clock::now();

      std::chrono::duration<double, std::milli> elapsed = (end - start);
      auto ms = elapsed.count();
      if (i >= 3){
         rttSum += ms;
      }
   }

   numBytesOut = send(communicationFD, &serverMsg, 1, 0);

   //Calculate the RTT
   double rttMeasurement = (rttSum / 4.0) /1000.0;
   
   //Begin the 80KB and time run
   auto start = std::chrono::high_resolution_clock::now();
   auto lastACK = start;
   while((numBytesIn = recv(communicationFD, chunk, DATACHUNK, MSG_WAITALL)) > 0){
      if (numBytesIn < DATACHUNK){
         break;
      }
      totalReceived+=numBytesIn;
      lastACK = std::chrono::high_resolution_clock::now();
      numBytesOut = send(communicationFD, &serverMsg, 1, 0);
      if (numBytesOut == -1){
         std::perror("Send failed: ");
         return -1;
      }
      chunksIn+=1;
   }
   std::chrono::duration<double> elapsedSeconds = lastACK - start;
   elapsedDuration = elapsedSeconds.count();

   closeStatus = close(communicationFD);

   //End
   
   closeStatus = close(sockServerFD);

   double transmissionDel = elapsedDuration - chunksIn * rttMeasurement;
   double rate = ((totalReceived * 8.0)/1000000.0)/transmissionDel;
   
   spdlog::info("Received={} KB, Rate={:.3f} Mbps, RTT={}ms\n", totalReceived/1000, rate, int(rttSum/4));

   return 0;

}
*/
