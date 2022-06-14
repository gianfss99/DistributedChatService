/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>

#include "sns.grpc.pb.h"
#include "coord.grpc.pb.h"

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

using csce438::Message;
using csce438::ListReply;
using csce438::Request;
using csce438::Reply;
using csce438::SNSService;

using coord::Attempt;
using coord::Credentials;
using coord::Response;
using coord::Heartbeat;
using coord::ProcessType;
using coord::MASTER;
using coord::SLAVE;
using coord::FSYNCH;
using coord::CoordService;

struct Client {
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::vector<Client*> client_followers;
  std::vector<Client*> client_following;
  ServerReaderWriter<Message, Message>* stream = 0;
  bool operator==(const Client& c1) const{
    return (username == c1.username);
  }
};

//Vector that stores every client that has been created
std::vector<Client> client_db;
std::string slave_port;
std::string slave_ip;
std::string user_id;
std::string type;
std::unique_ptr<CoordService::Stub> coord_stub_;
std::unique_ptr<SNSService::Stub> slave_stub_;
std::string directory;
std::queue<Message> message_queue;
std::mutex mtx;
std::condition_variable cv;

int Contact();
int Subscribe(std::string port);
bool checkQueue();
Message popQueue();
void Slave_Timeline();

void ReloadDB(){
  std::ifstream in(directory+"/server_users.txt");
  std::string line;
  while(getline(in,line)){
    Client c;
    c.username = line;
    c.connected = false;
    client_db.push_back(c);
  }

}

int FindUser(std::string filename, std::string user){
  std::ifstream in(filename);
  std::string line;
  int index = 0;
  while(getline(in,line)){
    if(line=="") continue;
    if(line==user) return index;
    index++;
  }
  return -1;
}


//Helper function used to find a Client object given its username
int find_user(std::string username){
  int index = 0;
  for(Client c : client_db){
    if(c.username == username)
      return index;
    index++;
  }
  return -1;
}

class SNSServiceImpl final : public SNSService::Service {
  
  Status List(ServerContext* context, const Request* request, ListReply* list_reply) override {
    Client user = client_db[find_user(request->username())];
    int index = 0;
    for(Client c : client_db){
      list_reply->add_all_users(c.username);
    }
    std::vector<Client*>::const_iterator it;
    for(it = user.client_followers.begin(); it!=user.client_followers.end(); it++){
      list_reply->add_followers((*it)->username);
    }
    return Status::OK;
  }

  Status Follow(ServerContext* context, const Request* request, Reply* reply) override {
    std::string username1 = request->username();
    std::string username2 = request->arguments(0);
    std::string filename = directory+"/"+username1+"_following.txt";

    //int join_index = find_user(username2);
    int join_index = FindUser(directory+"/users.txt",username2);
    if(join_index < 0 || username1 == username2)
      reply->set_msg("unkown user name");
    else{
      //Client *user1 = &client_db[find_user(username1)];
      //Client *user2 = &client_db[join_index];

      //if(std::find(user1->client_following.begin(), user1->client_following.end(), user2) != user1->client_following.end()){
      if(FindUser(filename,username2)>=0){
	      reply->set_msg("you have already joined");
        return Status::OK;
      }
      //user1->client_following.push_back(user2);
      //user2->client_followers.push_back(user1);
      reply->set_msg("Follow Successful");
      std::ofstream following(filename,std::ios::app|std::ios::out|std::ios::in);
      following << (username2+"\n"); 

      if(type=="master"){
        ClientContext master_context;
        Reply slave_reply;
        slave_stub_->Follow(&master_context,*request,&slave_reply);
      }

    }
    return Status::OK; 
  }

  Status UnFollow(ServerContext* context, const Request* request, Reply* reply) override {
    std::string username1 = request->username();
    std::string username2 = request->arguments(0);
    int leave_index = find_user(username2);
    if(leave_index < 0 || username1 == username2)
      reply->set_msg("unknown follower username");
    else{
      Client *user1 = &client_db[find_user(username1)];
      Client *user2 = &client_db[leave_index];
      if(std::find(user1->client_following.begin(), user1->client_following.end(), user2) == user1->client_following.end()){
	reply->set_msg("you are not follower");
        return Status::OK;
      }
      user1->client_following.erase(find(user1->client_following.begin(), user1->client_following.end(), user2)); 
      user2->client_followers.erase(find(user2->client_followers.begin(), user2->client_followers.end(), user1));
      reply->set_msg("UnFollow Successful");
    }
    return Status::OK;
  }
  
  Status Login(ServerContext* context, const Request* request, Reply* reply) override {
    Client c;
    std::string username = request->username();
    int user_index = find_user(username);
    if(user_index < 0){
      c.username = username;
      client_db.push_back(c);
      reply->set_msg("Login Successful!");
      std::ofstream server_users(directory+"/server_users.txt",std::ios::app|std::ios::out|std::ios::in);
      server_users << (username+"\n");
    }
    else{ 
      Client *user = &client_db[user_index];
      if(user->connected){
        reply->set_msg("Invalid Username");
        
      }

      else{
        std::string msg = "Welcome Back " + user->username;
	      reply->set_msg(msg);
        user->connected = true;
      }
    }
    if(type=="master"){
      ClientContext master_context;
      Reply slave_reply;
      slave_stub_->Login(&master_context,*request,&slave_reply);
    }
    return Status::OK;
  }

  Status Timeline(ServerContext* context, 
		ServerReaderWriter<Message, Message>* stream) override {
    

    Message message;
    
    stream->Read(&message);
    std::string username = message.username();
    std::string tl_filename = directory+"/"+username+"_timeline.txt";
    std::string filename = directory+"/"+username+".txt";
    //std::ofstream user_file(filename,std::ios::app|std::ios::out|std::ios::in);
    //google::protobuf::Timestamp temptime = message.timestamp();
    //std::string time = google::protobuf::util::TimeUtil::ToString(temptime);
    //std::string fileinput = time+" :: "+message.username()+":"+message.msg()+"\n";
    int idx = find_user(username);
    Client cl = client_db[idx];
    //std::cout<<"Howdyho"<<filename<<std::endl;
    //user_file << fileinput;
    if(type=="master"){
      {
        std::lock_guard<std::mutex> lk(mtx);
        message_queue.push(message);
      }
      cv.notify_one();
      
    }
    

    std::thread reader([stream,filename,context](){
      Client *c;
      Message message;
      while(true){
        if(context->IsCancelled()){
          //c->connected = false;
          std::cout<<"HELLO"<<std::endl;
          return;
        } 

        while(stream->Read(&message)) {
          if(context->IsCancelled()){
          //c->connected = false;
          std::cout<<"HELLO"<<std::endl;
          return;
        } 
          std::string username = message.username();
          int user_index = find_user(username);
          c = &client_db[user_index];
          //Write the current message to "username.txt"
          
          std::ofstream user_file(filename,std::ios::app|std::ios::out|std::ios::in);
          google::protobuf::Timestamp temptime = message.timestamp();
          std::string time = google::protobuf::util::TimeUtil::ToString(temptime);
          //std::string fileinput = time+" :: "+message.username()+":"+message.msg()+"\n";
          std::string fileinput = message.username()+" > "+message.msg()+"\n";
          //"Set Stream" is the default message from the client to initialize the stream
          if(message.msg() != "Set Stream"){
            std::cout<<"WHAT"<<std::endl;
            user_file << fileinput;
            if(type=="master"){
              {
                std::lock_guard<std::mutex> lk(mtx);
                message_queue.push(message);
              }
              cv.notify_one();
              
            }
          }
          
        }
      }
    });

    std::thread writer([tl_filename,stream,context](){
      bool startup = true;
      time_t curr;
      time(&curr);


      while(true){
        if(context->IsCancelled()) {
          return;
        }
        //get new mod time
        struct stat sfile;
        stat(tl_filename.c_str(),&sfile);

        //continue only if its the first iteration or curr < mod time
        if(!startup && (curr > sfile.st_mtim.tv_sec)){
          time(&curr);
          std::this_thread::sleep_for(std::chrono::milliseconds(5000));
          continue;
        }
        
        std::string line;
        std::vector<std::string> newest_twenty;
        std::ifstream in(tl_filename);
        int total = 0;
        while(getline(in,line)){
          if(line=="") continue;
          total++;
        }
        int count = 0;
        in.close();
        in.open(tl_filename);
        //Read the last up-to-20 lines (newest 20 messages) from userfollowing.txt
        while(getline(in, line)){
          if(line=="") continue;
          if(total > 20){
            if(count < total-20){
              count++;
              continue;
            }
          }
          newest_twenty.push_back(line);
        }
        Message new_msg; 
        //Send the newest messages to the client to be displayed
        for(int i = 0; i<newest_twenty.size(); i++){
          new_msg.set_msg(newest_twenty[i]);
          stream->Write(new_msg);
        }    
        time(&curr);
        startup = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5000));

      }
    });

    if(type=="master"){
      Slave_Timeline();
    }

    reader.join();
    writer.join();
    //If the client disconnected from Chat Mode, set connected to false
    cl.connected = false;
    return Status::OK;
  }

};

void Slave_Timeline(){
    ClientContext master_context;
    
    std::shared_ptr<ClientReaderWriter<Message, Message>> slave_stream = std::shared_ptr<ClientReaderWriter<Message, Message>>(
          slave_stub_->Timeline(&master_context));

    std::thread slave_writer([slave_stream](){
      Message m;
      while(1){
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk,[]{
          return checkQueue();
        });
        
        m = popQueue();
        slave_stream->Write(m);
      }
      slave_stream->WritesDone();
    });
      
    
    slave_writer.join();
}

void heart_beat(){
  ClientContext context;

    std::shared_ptr<ClientReaderWriter<Heartbeat, Heartbeat>> stream(
            coord_stub_->ServerAlive(&context));

    std::thread sendHb([stream](){
      Heartbeat hb;
      hb.set_id(std::stoi(user_id));
      if(type=="master")
        hb.set_server_type(coord::MASTER);
      else hb.set_server_type(coord::SLAVE);
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

  SNSServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  
  coord_stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(
               grpc::CreateChannel(
                    coord_address, grpc::InsecureChannelCredentials())));

  //subscribe to coordinator

  int coord_conn = Subscribe(port_no);

  //connect to slave

  slave_stub_ = NULL;
  int slave_conn;
  if(type=="master"){
    do{
      slave_conn = Contact();
    }while(slave_conn<0);

    std::string slave_address = slave_ip+":"+slave_port;
    slave_stub_ = std::unique_ptr<SNSService::Stub>(SNSService::NewStub(
                 grpc::CreateChannel(
                      slave_address, grpc::InsecureChannelCredentials())));
    std::cout<<"Connected to slave on addr"<<slave_address<<std::endl;
    
  }
  //heartbeat thread
  heart_beat();
//

  
  std::cout << "Server listening on " << server_address << std::endl;
  // if(type=="master"){
  //   Slave_Timeline();
  // }

  server->Wait();
}

int main(int argc, char** argv) {
  
  std::string port = "3010";
  std::string coord_port = "9090";
  std::string coord_ip = "0.0.0.0";
  type = "master";
  user_id = "0";
  int opt = 0;
  while ((opt = getopt(argc, argv, "c:o:p:i:t:")) != -1){
    switch(opt) {
      case 'c':
          coord_ip = optarg;break;
      case 'o':
          coord_port = optarg;break;
      case 'p':
          port = optarg;break;
      case 'i':
          user_id = optarg;break;
      case 't':
          type = optarg;break;

      default:
	  std::cerr << "Invalid Command Line Argument\n";
    }
  }

  directory = type+"_"+user_id;
  mkdir(directory.c_str(),0777);
  ReloadDB();
  RunServer(port,coord_ip,coord_port);

  return 0;
}


int Subscribe(std::string port){
  ClientContext context;

  Credentials creds ;
  creds.set_id(std::stoi(user_id));
  if(type=="master")
    creds.set_service(coord::MASTER);
  else
    creds.set_service(coord::SLAVE);
  creds.set_port(port);

  Response response;
  Status status = coord_stub_->Subscribe(&context, creds, &response);

  if(!status.ok())
    return -1;

  return 1;

}

int Contact(){
  ClientContext context;
  Attempt attempt;
  attempt.set_id(std::stoi(user_id));
  attempt.set_service(coord::MASTER);
  Response response;
  Status status = coord_stub_->Contact(&context, attempt, &response);
  slave_ip = response.ip();
  slave_port = response.port();  
  if(!status.ok() || slave_port == "0"){
      return -1;
  }

  return 1;
}

bool checkQueue(){
  return !message_queue.empty();
}

Message popQueue(){
  Message m = message_queue.front();
  message_queue.pop();
  return m;
}