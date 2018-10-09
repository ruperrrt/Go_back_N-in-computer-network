//
//  main.cpp
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
#include <queue>
#include <vector>
#include <time.h>
#include <sys/time.h>
#include <zlib.h>

using namespace std;

#define MAXDATASIZE 1024
#define SERVPORT 19578
#define WINDOWSIZE 5
#define IP "10.0.0.1" 
#define receiver_IP "10.0.0.2" 

struct packet{
    bool fin = false;
    bool syn = false;
    bool ackn = false;
    int seqn = 0;
    char data;
    uLong crc = crc32(0L, Z_NULL, 0); 

    //packet (bool fin, bool syn, bool ackn, int seqn, char data) : fin(fin), syn(syn), ackn(ackn), seqn(seqn), data(data) {}
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
    packet buffer;      //buffer used for incoming and outgoing messages};
    queue<packet> window;   // a queue for window to store packets
    int num_active = 0;     // number of active buckets in the window
    int expected_ack_num = 0;    //expected ACK number
};

int send(connection *connect, packet *pkt) {

    if(pkt->syn){
        printf("Establishing a connection to receiver... (sending SYN)\n");
    }
    else if(pkt->fin){
        cout << "Terminating connection... (sending FIN)" << endl;
    }
    else{
        cout <<"Sending character \""<<pkt->data<<"\""<<endl;
    }
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
    return_buffer->seqn = connect->pkt.seqn;
    return_buffer->syn = connect->pkt.syn;
    return_buffer->data = connect->pkt.data;
    return_buffer->fin = connect->pkt.fin;
    return_buffer->ackn = connect->pkt.ackn;
    //printf("UDPConnection receive got %i bytes\n", connect->datalen);
    return connect->datalen;
}

void setTimer(int ms, connection connect){
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = ms*1000;
    if(setsockopt(connect.sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeval))<0){
    	perror ("Error!");
    }
}

bool validAck(packet received, int expected_seqn){
    if(received.seqn!=expected_seqn || !received.ackn){
        return false;
    }
    return true;
}


bool listenForAcks(packet * buffer, connection connect, gobackn * arq){
    /* if time out, stop */
    if(blocking_receive(buffer, &connect) == -1){
        printf("Window timed-out...\n");
        return false;
    }
    /* if it is invalid, reset timer and try again*/
    if (!validAck(*buffer, arq->expected_ack_num)) {
        //cout << "expectedack" <<  arq->expected_ack_num << endl;
        //printf("\tBAD ACK '%d', RELISTENING\n", buffer->seqn);
        /* this ACK is invalid, reset our timeout and try again */
        return listenForAcks(buffer, connect, arq);
    }
    cout << "ACK Received with SEQ# " << buffer->seqn << endl;
    arq->expected_ack_num++;
    arq->num_active--;
    arq->window.pop();
    return true;
}

/* set timeout to 200 ms before listening for an ACK */
bool acceptAck(connection connect, gobackn *arq){
    bool didNotTimeout;
    setTimer(200, connect);
    didNotTimeout = listenForAcks(&(connect.pkt), connect, arq);
    setTimer(0, connect);
    
    return didNotTimeout;
}

bool canAdd (gobackn arq){
    return arq.num_active < WINDOWSIZE;
}

void addToWindow(packet pac, gobackn *arq, connection connect){
    arq->window.push(pac);
    arq->buffer =  pac;
    arq->num_active ++;

    send (&connect, &arq->buffer);
}

void resendWindow(connection connect, gobackn *arq){
    //cout << arq-> expected_ack_num << endl;
    for (int i = 1; i<= arq->num_active; i++) {
        packet temp = arq->window.front();
        arq->window.pop();
        
        send(&connect, &temp);
        arq->window.push(temp);
    }
}


void GBNUDP_send(vector<packet> message, gobackn *arq, connection connect) {
    /* loop while we have more to send or are still waiting
     on outstanding ACKs */
    
	/* sending and receiving ACK for the first packet (handshake) */
    while(1){
    	packet first = message[0];
    	addToWindow (first, arq, connect);
    	if (!acceptAck(connect, arq)) {
    		addToWindow (first, arq, connect);
    	}
    	else{
    		break;
    	}
    }
    int i = 1;
    packet temp = message[1];
    while (!temp.fin || arq->num_active!=0) {
        /* send as many as we can*/
        //while (last_sent_seqn )
        while (canAdd(*arq) && !temp.fin) {
        	//cout << arq->buffer.syn << endl;
            addToWindow(temp, arq, connect);
            i++;
            if(i==message.size()){
                break;
            }
            temp = message[i];
        }

        if(canAdd(*arq)){
            addToWindow(message.back(), arq, connect);
        }
        
        /* listen */
        if (!acceptAck(connect, arq)) {
            /* our listen timed out, resend the entire window */
            resendWindow(connect, arq);
        }

        if(arq->expected_ack_num==message.size()){
            break;
        }
    }


    //printf("WINDOW EMPTY & ALL BYTES SENT\n");
}


int main() {
    
    connection connect;
    
    
    /*********************** Connect ************************/
    
    connect.serv_addr.sin_family = AF_INET;
    connect.serv_addr.sin_port = htons(SERVPORT);
    connect.serv_addr.sin_addr.s_addr = inet_addr(IP);
    connect.rem_addr.sin_family = AF_INET;
    connect.rem_addr.sin_port = htons(SERVPORT);
    connect.rem_addr.sin_addr.s_addr = inet_addr(receiver_IP);
    connect.addrlen = sizeof(struct sockaddr_in);
    
    if((connect.sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0){
        perror("error in OnSocketCreated\n");
        printf("Could not connect\n");
        exit(1);
    }
    
    /*********************** GBN ARQ ***********************/
    gobackn arq;
    
    printf("Please provide a string with at least 10 characters\n");
    string input;
    getline(cin,input);
    vector<packet> mesg;
    //packet *first = new packet (false,true,false,0,NULL);
    packet first;
    first.syn = true;
    mesg.push_back(first);
    for (int i=0; i<input.length(); i++) {
        //packet *temp = new packet (false,false,false,i+1,input.at(i));
        packet temp;
        temp.seqn = i+1;
        temp.data = input.at(i);
        //const unsigned char* dataTemp = "1";
        temp.crc = crc32(temp.crc, reinterpret_cast<const unsigned char*>(&temp.data), sizeof(temp.data));
        //cout << temp.crc << endl;
        mesg.push_back(temp);
    }
    //packet *last = new packet (true,false,false,static_cast<int>(input.length()+1),NULL);
    packet last;
    last.fin = true;
    last.seqn = static_cast<int>(input.length()+1);
    mesg.push_back(last);
    
    //struct timeval t0;
    //struct timeval t1;
    //gettimeofday(&t0, 0);
    GBNUDP_send(mesg, &arq, connect);
    cout << "Done!" << endl;
    //gettimeofday(&t1, 0);
    //long long elapsed = (t1.tv_sec-t0.tv_sec)*1000000 + t1.tv_usec - t0.tv_usec;
    //cout << "Time taken is " << elapsed << endl;
    return 0;
}
