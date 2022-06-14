#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <vector>
#include <unordered_map>
#include <thread>
#include <sys/stat.h>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>

#include "coord.grpc.pb.h"
#include "fsynch.grpc.pb.h"

using google::protobuf::Timestamp;
using google::protobuf::Duration;

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;

using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;

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

using synch::SynchService;
using synch::Target;
using synch::Confirmation;


//helps knowing how many lines to skip before the new info
struct counter{
	std::vector<std::string> users;
	int num_users;
	std::unordered_map<std::string,int> following_size;
	std::unordered_map<std::string,int> timeline_size;
	std::unordered_map<std::string,std::string> ports;
};

counter db;
std::string f_id;
std::string master_dir;
std::string slave_dir;
std::string ip;

std::unique_ptr<CoordService::Stub> coord_stub_;

std::unordered_map<std::string,std::unique_ptr<SynchService::Stub>> f_stubs_;

class SynchServiceImpl final : public SynchService::Service{
	Status User(ServerContext* context, const Target* target, Confirmation* conf){
		std::string input = target->id();
		std::ofstream m(master_dir+"/users.txt",std::ios::app|std::ios::out|std::ios::in);
		std::ofstream s(slave_dir+"/users.txt",std::ios::app|std::ios::out|std::ios::in);
		m << (input+"\n");
		s << (input+"\n");

		return Status::OK;
	}

	Status Follower(ServerContext* context, const Target* target, Confirmation* conf){
		std::string tgt = target->tgt();
		std::string input = target->id();
		std::ofstream m(master_dir+"/"+tgt+"_followers.txt",std::ios::app|std::ios::out|std::ios::in);
		std::ofstream s(slave_dir+"/"+tgt+"_followers.txt",std::ios::app|std::ios::out|std::ios::in);
		m << (input+"\n");
		s << (input+"\n");
		return Status::OK;
	}

	Status Update(ServerContext* context, const Target* target, Confirmation* conf){
		std::string tgt = target->tgt();
		std::string input = target->msg();
		std::ofstream m(master_dir+"/"+tgt+"_timeline.txt",std::ios::app|std::ios::out|std::ios::in);
		std::ofstream s(slave_dir+"/"+tgt+"_timeline.txt",std::ios::app|std::ios::out|std::ios::in);
		m << (input+"\n");
		s << (input+"\n");
		return Status::OK;
	}
};

void worker_threads(){
	std::thread users([](){
		std::string server_users = slave_dir+"/server_users.txt";
		
		bool startup = true;
		struct stat sfile;
		

		std::ifstream u(server_users);
		std::string line;
		std::vector<std::string> all_users;
		while(getline(u,line)) {
			all_users.push_back(line);
			db.num_users++;
			db.following_size[line] = 0;
			db.timeline_size[line] = 0;
		}
		time_t curr;
		time(&curr);

		while(1){
			time_t new_time;
			time(&new_time);
        	stat(server_users.c_str(),&sfile);
        	if(startup || (curr < sfile.st_mtim.tv_sec)){
        		
        	
				std::ifstream in(server_users);
				
				for(int i = 0; i<db.num_users; i++) 
					getline(in,line);
				std::vector<std::string> new_users;
				while(getline(in,line)){
					db.num_users++;
					new_users.push_back(line);
					all_users.push_back(line);
					db.following_size[line] = 0;
					db.timeline_size[line] = 0;

				}
				Target target;
				Confirmation conf;
				
				for(int i = 0; i<new_users.size(); i++){
					target.set_id(new_users[i]);
					for(auto &stub_ : f_stubs_){
						ClientContext context;
						stub_.second->User(&context,target,&conf);
					}
				}
			}

			for(int i = 0; i<all_users.size(); i++){
				std::string fn = slave_dir+"/"+all_users[i]+"_following.txt";

				stat(fn.c_str(),&sfile);
				if(startup || (curr < sfile.st_mtim.tv_sec)){
					std::cout<<"user followed: "<<std::endl;
					std::ifstream in(fn);
					for(int j = 0; j<db.following_size[all_users[i]]; j++) getline(in,line);

					while(getline(in,line))
					{
						if(db.ports.count(line) == 0){
							ClientContext context_coord;
							Attempt attempt;
							Response response;
							attempt.set_id(std::stoi(line));
							attempt.set_service(coord::FSYNCH);
							coord_stub_->Contact(&context_coord,attempt,&response);
							db.ports[line] = response.port();
							
						}
						ClientContext context;
						Target target;
						Confirmation conf;
						target.set_id(all_users[i]);
						target.set_tgt(line);
						f_stubs_[db.ports[line]]->Follower(&context,target,&conf);
						db.following_size[all_users[i]]++;
					}
				}
			}
			
			for(int i = 0; i<all_users.size(); i++){
				std::string fn = slave_dir+"/"+all_users[i]+".txt";
				stat(fn.c_str(),&sfile);
				if(startup || (curr < sfile.st_mtim.tv_sec)){
					std::ifstream in(fn);
					for(int j = 0; j<db.timeline_size[all_users[i]]; j++) getline(in,line);

					while(getline(in,line)){
						std::string fn2 = slave_dir+"/"+all_users[i]+"_followers.txt";
						std::string flwr;
						std::ifstream in2(fn2);
						while(getline(in2,flwr)){
							if(db.ports.count(flwr) == 0){
								ClientContext context_coord;
								Attempt attempt;
								Response response;
								attempt.set_id(std::stoi(flwr));
								attempt.set_service(coord::FSYNCH);
								coord_stub_->Contact(&context_coord,attempt,&response);
								db.ports[flwr] = response.port();
							}
							ClientContext context;
							Target target;
							Confirmation conf;
							target.set_tgt(flwr);
							target.set_msg(line);
							f_stubs_[db.ports[flwr]]->Update(&context,target,&conf);
						}
						db.timeline_size[all_users[i]]++;
					}
				}
			}

			startup=false;
			curr = new_time;
			time(&new_time);
       		std::this_thread::sleep_for(std::chrono::milliseconds(10000));


		}
	});


	users.join();
}

int Subscribe(std::string port){
  ClientContext context;

  Credentials creds ;
  creds.set_id(std::stoi(f_id));
  
  creds.set_service(coord::FSYNCH);
  creds.set_port(port);

  Response response;
  Status status = coord_stub_->Subscribe(&context, creds, &response);

  if(!status.ok())
    return -1;

  return 1;

}

int Contact(int id){
  ClientContext context;
  Attempt attempt;
  attempt.set_id(id);
  attempt.set_service(coord::FSYNCH);
  Response response;
  Status status = coord_stub_->Contact(&context, attempt, &response);
  ip = response.ip();
  std::string port = response.port();  
  if(!status.ok() || port == "0"){
      return -1;
  }
  std::string synch_address = ip+":"+port;
  std::cout<<synch_address<<std::endl;
  f_stubs_[port] = std::unique_ptr<SynchService::Stub>(SynchService::NewStub(
               grpc::CreateChannel(
                    synch_address, grpc::InsecureChannelCredentials())));
  return 1;
}

void connectSynchs(){
	for(int i = 0; i<3; i++){
		int conn;
		do{
			conn = Contact(std::stoi(f_id)+i);
		}while(conn<0);
	}
}

void heart_beat(){
	ClientContext context;

    std::shared_ptr<ClientReaderWriter<Heartbeat, Heartbeat>> stream(
            coord_stub_->ServerAlive(&context));

    std::thread sendHb([stream](){
    	Heartbeat hb;
    	hb.set_id(std::stoi(f_id));
    	hb.set_server_type(coord::FSYNCH);
    	while(1){
    		stream->Write(hb);
    	    std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    	}
    });
    sendHb.join();
}

void RunServer(std::string port_no, std::string coord_ip, std::string coord_port) {
  std::string server_address = "0.0.0.0:"+port_no;
  std::string coord_address = coord_ip+":"+coord_port;
  SynchServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  coord_stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(
               grpc::CreateChannel(
                    coord_address, grpc::InsecureChannelCredentials())));
  
  int coord_conn = Subscribe(port_no);
  connectSynchs();
  worker_threads();
  heart_beat();
  std::cout << "Server listening on " << server_address << std::endl;

  server->Wait();
}

int main(int argc, char* argv[]){
	std::string coord_port;
	std::string coord_ip;
	std::string port;
	int opt = 0;
  	while ((opt = getopt(argc, argv, "c:h:p:i:")) != -1){
	    switch(opt) {
    		case 'c':
    			coord_port = optarg;break;
    		case 'h':
    			coord_ip = optarg;break;
      		case 'p':
          		port = optarg;break;
          	case 'i':
          		f_id = optarg;break;
      		default:
	  		std::cerr << "Invalid Command Line Argument\n";
	    }
 	}
 	master_dir = "master_"+f_id;
	slave_dir = "slave_"+f_id;
	db.num_users = 0;
 	RunServer(port,coord_ip,coord_port);

 	return 0;
 	
}