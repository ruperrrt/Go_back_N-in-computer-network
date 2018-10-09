//
//  receiver.cpp
//  GDN_receiver
//
//  Created by Rupert Sun on 4/10/18.
//  Copyright © 2018 Rupert Sun. All rights reserved.
//

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <memory.h>
#include <time.h>
#include <zlib.h>


using namespace std;

#define windowSize 5
#define MAXDATASIZE 1024
#define SERVPORT 19578
#define IP "10.0.0.2" 

struct packet{
    bool fin;
    bool syn;
    bool ackn;
    int seqn;
    char data;
    uLong crc = crc32(0L, Z_NULL, 0); 
};

struct connection{
    int sock_fd;
    struct sockaddr_in serv_addr;
    struct sockaddr_in rem_addr;
    int datalen;
    packet pkt;
    unsigned int addrlen;
};


struct gobackn{
    int expected_seqn = 0; //next sequence number expected by the server
    packet buffer; //buffer used for incoming and outgoing messages
};


int send(connection *connect, packet *pkt) {

    int datalen = sendto(connect->sock_fd, pkt, sizeof(* pkt), 0, (struct sockaddr *)&connect->rem_addr, sizeof(connect->rem_addr));
    if(datalen>=0){
        //printf("UDPConnection send\n");
        return datalen;
    }
    else{
        printf("UDPConnection send error\n");
        return 0;
    }
}

int blocking_receive(packet *return_buffer, connection *connect) {
    socklen_t addrlen = sizeof(connect->rem_addr);
    struct packet pkt;
    connect->datalen = recvfrom(connect->sock_fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&connect->rem_addr, &addrlen);
    connect->pkt = pkt;
    if (connect->datalen >= 0){
    return_buffer->seqn = connect->pkt.seqn;
    return_buffer->syn = connect->pkt.syn;
    return_buffer->data = connect->pkt.data;
    return_buffer->fin = connect->pkt.fin;
    return_buffer->ackn = connect->pkt.ackn;
    return_buffer->crc = connect->pkt.crc;
    //printf("UDPConnection receive got %i bytes\n", connect->datalen);
    //printf("UDPConnection received %c\n", connect->pkt.data);    
    }
    return connect->datalen;
}

bool validDatagram (packet pkt, int expected_seqn){
    if (pkt.seqn != expected_seqn || 
        (crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const unsigned char*>(&pkt.data), sizeof(pkt.data)) != pkt.crc && (!pkt.fin && !pkt.syn)))
    {
        //cout << expected_seqn << endl;
        //cout << pkt.seqn<<" "<< pkt.data<<endl;
        //cout << pkt.crc << endl;
        //cout << crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const unsigned char*>(&pkt.data), sizeof(pkt.data)) << endl;
        return false;
    }
    return true;
}

void sendAck(gobackn *arq, connection con){
    arq->buffer.ackn = true;
    send(&con,&arq->buffer);
}

void setTimer(int ms, int sock_fd){
    struct timeval timeout;
    timeout.tv_sec = ms;
    timeout.tv_usec = 0;
    if(setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeval))<0){
        perror ("Error!");
    }
}

string GBNUDP_receive(gobackn *arq, connection *connect){
    string output;
    bool flag = true;
    //clock_t before;
    //clock_t difference;
    while (blocking_receive(&(arq->buffer),connect)!=-1) {
        if (validDatagram( arq->buffer, arq->expected_seqn)) {
            if(arq->buffer.syn){
                arq->expected_seqn ++;
                char str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(connect->serv_addr.sin_addr), str, INET_ADDRSTRLEN);
                cout << "Connection request received from " << str << ":" << SERVPORT << endl;
            }
            else if(arq->buffer.fin){
                if(flag){
                    cout << "Sender is terminating with FIN..." << endl;
                    // before = clock();
                    setTimer(5, connect->sock_fd);
                    flag = false;
                }
                cout << "Reception Complete: \""<<output<<"\""<<endl;
                sendAck(arq,*connect);
                cout <<"Sending ACK with SEQ # " << arq->buffer.seqn <<endl;
               // break;
                
                
            }
            else{
                arq->expected_seqn ++;
                cout <<"Sending ACK with SEQ # " << arq->buffer.seqn <<", expecting SEQ # " << arq->expected_seqn <<endl;
                cout <<"Received character \"" << arq->buffer.data << "\"" << endl;
                output += arq->buffer.data;
            }
            sendAck(arq,*connect);

            
        }
        else if(arq->buffer.seqn == arq->expected_seqn && crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const unsigned char*>(&arq->buffer.data), sizeof(arq->buffer.data)) != arq->buffer.crc){
            sendAck(arq,*connect);
            cout << "packet discard due to corruption" << endl;
            
        }
        else{
            sendAck(arq,*connect);
            cout << "Out of order SEQ# " << arq->buffer.seqn << endl;
            cout << "Sending ACK with SEQ # " << arq->expected_seqn <<endl;
        }
        
        /* (!flag) {
            setTimer(0, connect->sock_fd);
        }*/

        /*
        if(!flag){
                difference = clock() - before;
                cout << difference * 1000 /CLOCKS_PER_SEC<< endl;
                if(difference * 1000 / CLOCKS_PER_SEC > 5000){
                    break;
                }
            }
            */
    }
    return output;
}

int main() {
    
     /*********************** Connect ************************/
    
    connection connect;

    connect.addrlen = sizeof(struct sockaddr_in);
    connect.serv_addr.sin_family=AF_INET;
    connect.serv_addr.sin_addr.s_addr = inet_addr(IP);
    connect.serv_addr.sin_port=htons(SERVPORT);
    
    
    if((connect.sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("cannot create datafram socket");
        exit(1);
    }
    
    if (bind(connect.sock_fd, (struct sockaddr *)&connect.serv_addr, sizeof(connect.serv_addr))<0){
        perror("bind error！");
        exit(1);
    }


    
        /*********************** GBN ARQ ***********************/
    

    while (1) {
        cout << "Receiver is running and ready to receive connections on port "<< SERVPORT <<"..." << endl;
        setTimer(0, connect.sock_fd);
        gobackn arq;
        GBNUDP_receive(&arq, &connect);

        //sleep(5); //millisecs inside a smaller 
        //check if receive anything 

    }
    return 0;
}
