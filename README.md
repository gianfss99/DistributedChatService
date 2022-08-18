# DistributedChatService
Chat service with multiple server clusters

A Twitter-like chat service where one can follow other users and see their new posts.
Unlike the typical client-server chat service project, this service distributes the clients
between 3 server clusters, each of which has a master and a slave server for fault-tolerance,
and a synchronizing process, which communicates with other server clusters and retrieves new
relevant information from them. When a new user logs in, it is chosen (id mod 3) by one of the
servers, which will be responsible to maintain their posts and follower/following information.

The servers and clients can live in different machines thanks to the use of Remote Procedure Calls, 
which allows for custom protocols to be sent across machines through a network. Because of these
custom protocols, it is more flexible than regular network communications. It is easily scalable to
many more servers without much more code. 
