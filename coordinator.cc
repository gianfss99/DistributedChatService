#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <fstream>
#include <iostream>
#include <thread>
#include <memory>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>

#include "coord.grpc.pb.h"

using google::protobuf::Timestamp;
using google::protobuf::Duration;

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;

using coord::Credentials;
using coord::Attempt;
using coord::Response;
using coord::Heartbeat;
using coord::ProcessType;
using coord::MASTER;
using coord::SLAVE;
using coord::FSYNCH;
using coord::CLIENT;
using coord::CoordService;

struct routing_table{
	int port;
	bool active;
};

std::vector<routing_table> master;
std::vector<routing_table> slave;
std::vector<routing_table> fsynch; 

std::string ip_addr = "0.0.0.0";

class CoordServiceImpl final : public CoordService::Service{

	//first connection with coordinator, saves port and id
	Status Subscribe(ServerContext* context, const Credentials* creds, Response* response){
		int id = (creds->id()-1)%3;
		ProcessType service = creds->service();
		std::string port = creds->port();
		if(service == MASTER){
			master[id].port = std::stoi(port);
			master[id].active = true;
			std::cout<<"master:"<<port<<std::endl;
		}
		else if(service == SLAVE){
			slave[id].port = std::stoi(port);
			slave[id].active = true;
			std::cout<<"slave:"<<port<<std::endl;
		}else if(service == FSYNCH){
			fsynch[id].port = std::stoi(port);
			fsynch[id].active = true;
		}
		return Status::OK;
	}

	Status Contact(ServerContext* context, const Attempt* attempt, Response* response){

		int id = (attempt->id()-1)%3;	
		std::cout<<"id "<<id<<std::endl;
		ProcessType service = attempt->service();
		if(service == CLIENT){
			if(master[id].active){
				response->set_ip(ip_addr);
				response->set_port(std::to_string(master[id].port));
				return Status::OK;
			}else{
				response->set_ip(ip_addr);
				response->set_port(std::to_string(slave[id].port));
				return Status::OK;
			}
		}


		if(service == MASTER){
			if(slave[id].active){
				response->set_ip(ip_addr);
				response->set_port(std::to_string(slave[id].port));
				return Status::OK;
			}
		}

		if(service == FSYNCH){
			if(fsynch[id].active){
				response->set_ip(ip_addr);
				response->set_port(std::to_string(fsynch[id].port));
				return Status::OK;
			}
		}

		response->set_ip("service_unavailable");
		response->set_port("0");
		return Status::OK;

	}

	
	Status ServerAlive(ServerContext* context, ServerReaderWriter<Heartbeat, Heartbeat>* stream) override {
		std::thread hb([stream,context](){
			int count = 2;
			Heartbeat hb;
			stream->Read(&hb);
			int id = (hb.id()-1)%3;
			ProcessType service = hb.server_type();
			
			while(1){
				if(!stream->Read(&hb)){
					count--;
				}else count = 2;
				if(count==0) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10000));
			}

			if(service==MASTER)
				master[id].active = 0;
			else if(service==SLAVE)
				slave[id].active = 0;
			else
				fsynch[id].active = 0;
			std::cout<<"server"<<id<<" lost!"<<std::endl;
		});

		hb.join();
		return Status::OK;
	}
	
};

//create routing tables
void build_tables(){
	master.resize(3,{0,0});
	slave.resize(3,{0,0});
	fsynch.resize(3,{0,0});
}

//starts coordinator server and waits for contact
void RunServer(std::string port_no) {
  std::string server_address = "0.0.0.0:"+port_no;
  CoordServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  server->Wait();
}

int main(int argc, char* argv[]){
	build_tables();
	std::string port = "9090";
  	int opt = 0;
  	while ((opt = getopt(argc, argv, "p:")) != -1){
	    switch(opt) {
	      case 'p':
	          port = optarg;break;
	      default:
		  	std::cerr << "Invalid Command Line Argument\n";
	    }
 	}
  	RunServer(port);
	return 0;
}