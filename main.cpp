#include<iostream>
#include<sys/epoll.h>
#include<unistd.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<string>
#include<vector>
#include<memory>
#include<fcntl.h>
#include<string.h>
#include<fstream>
#include<sys/stat.h>
#include<errno.h>
#include<sys/sendfile.h>
#define MAX_LISTEN_NUM			10
#define MAX_BLOCK_NUM			1024
#define MAX_EPOLL_RETURN_NUM	1024
#define MAX_BUFFER_SIZE			1024
#define HTML_SOURCE_ROOT		"./html"
#define HTTP_GET	0
#define HTTP_POST	1
using namespace std;
class item {
public:
	item(int m_client, uint32_t m_event, string m_wbuf, string m_rbuf)
	{
		this->clientfd = m_client;
		this->event = m_event;
		this->r_buffer = m_rbuf;
		this->w_buffer = m_wbuf;
		this->resource = "";
		ret_code = 0;
	}
	int clientfd;
	string w_buffer;
	string r_buffer;
	uint32_t event;
	string resource;
	string file_type;
	int Request_type;
	int ret_code;
};
class item_block {
public:
	item_block()
	{
		items.resize(MAX_BLOCK_NUM, NULL);
	}
	vector<item*> items;
	~item_block()
	{
		for (int i = 0; i < MAX_BLOCK_NUM; i++)
		{
			delete this->items[i];
		}
		cout << "clear successful!" << endl;
	}
};
class reactor {
public:
	reactor()
	{
		this->epollfd = epoll_create(1);
		this->fdcount = 0;
	}
	void Add_To_Reactor(int clientfd, uint32_t epoll_type);
	void Del_From_Reactor(int clientfd, uint32_t epoll_type);
	void Destory_Reactor();
	void Deal_Events(int sock);
	void Add_To_Epoll(int clientfd, uint32_t epoll_type);
	void Delete_From_Epoll(int clientfd, uint32_t epoll_type);
	void Http_Response(item*& it);
	int Http_Request(item*& it);
	void Send_cb(item*& it);
	void Recv_cb(item*& it);
	void Accept_cb(int sock);
	size_t Http_GetLine(item* const& it, string& str, size_t indx);
	int Http_Deal_Get(item*& it);
	int Http_Deal_Post(item*& it);
	string Ret_Type(item*& it);
	int epollfd;
	int fdcount;//fd数量
	vector<item_block> blocks;

};
int Init_Sock(int port)
{
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd <= 0) {
		exit(0);
	}
	sockaddr_in sin;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	if (bind(sockfd, (sockaddr*)&sin, sizeof(sin)) == -1)
	{
		cout << "bind faild" << endl;
		exit(0);
	}
	int flag = fcntl(sockfd, F_GETFL, 0);
	flag |= O_NONBLOCK;
	fcntl(sockfd, F_SETFL, flag);
	listen(sockfd, MAX_LISTEN_NUM);
	return sockfd;
}

void reactor::Add_To_Reactor(int clientfd, uint32_t epoll_type)
{
	long blcs_ind = clientfd / MAX_BLOCK_NUM;
	long blc_ind = clientfd % MAX_BLOCK_NUM;
	item* blc = new item(clientfd, epoll_type, "", "");
	if ((this->blocks.size() == 0) || blcs_ind > (this->blocks.size() - 1)) {
		item_block newblock;
		this->blocks.push_back(newblock);
	}
	this->blocks[blcs_ind].items[blc_ind] = blc;
	this->fdcount++;
	Add_To_Epoll(clientfd, epoll_type);
}

void reactor::Del_From_Reactor(int clientfd, uint32_t epoll_type)
{
	int blcs_ind = clientfd / MAX_BLOCK_NUM;
	int blc_ind = clientfd % MAX_BLOCK_NUM;
	Delete_From_Epoll(clientfd, epoll_type);
	close(clientfd);
	delete this->blocks[blcs_ind].items[blc_ind];
	this->blocks[blcs_ind].items[blc_ind] = NULL;
}

void reactor::Destory_Reactor()
{
	int n = this->blocks.size();
	this->blocks.clear();
}

void reactor::Deal_Events(int sock)
{
	epoll_event events[MAX_EPOLL_RETURN_NUM];
	while (1)
	{
		int nready = epoll_wait(this->epollfd, events, MAX_EPOLL_RETURN_NUM, -1);
		if (nready == -1) continue;
		for (int i = 0; i < nready; i++)
		{
			if (events[i].data.fd == sock)
			{
				Accept_cb(sock);
			}
			else {
				int blcs_ind = events[i].data.fd / MAX_BLOCK_NUM;
				int blc_ind = events[i].data.fd % MAX_BLOCK_NUM;
				item* it = this->blocks[blcs_ind].items[blc_ind];
				//it = move(this->blocks[blcs_ind].items[blc_ind]);
				if (events[i].events & EPOLLIN)
				{
					Recv_cb(it);
				}
				else if (events[i].events & EPOLLOUT)
				{
					Send_cb(it);
				}
			}
		}
	}
}

void reactor::Add_To_Epoll(int clientfd, uint32_t epoll_type)
{
	epoll_event ev;
	ev.data.fd = clientfd;
	ev.events = epoll_type;
	epoll_ctl(this->epollfd, EPOLL_CTL_ADD, clientfd, &ev);
}

void reactor::Delete_From_Epoll(int clientfd, uint32_t epoll_type)
{
	epoll_event ev;
	ev.data.fd = clientfd;
	ev.events = epoll_type;
	epoll_ctl(this->epollfd, EPOLL_CTL_DEL, clientfd, &ev);
}

size_t reactor::Http_GetLine(item* const& it, string& str, size_t indx)
{
	size_t next_beg = it->r_buffer.find_first_of("\r\n", indx) + 2;
	str = it->r_buffer.substr(indx, next_beg - indx);
	return next_beg;
}
int reactor::Http_Deal_Get(item*& it)
{
	int fd = open(it->resource.c_str(), O_RDONLY);
	string response = "";
	if (fd == -1)
	{
		it->ret_code = 404;
		int errfd = open("./html/error.html", O_RDONLY);
		it->resource = "./html/error.html";
		struct stat stat_err;
		fstat(errfd, &stat_err);
		close(errfd);
		response = "Http/1.1 404 Not Found\r\n\
				    Date: Sun, 19 Mar 2023 16:28:66 GMT\r\n\
					Content-Type: ";
		response += "text / html; charset = utf - 8\r\n\
					Content-Length: ";
		response += stat_err.st_size;
		response += "\r\n\r\n";

	}
	else {
		struct stat stat_buf;
		fstat(fd, &stat_buf);
		close(fd);
		if (S_ISDIR(stat_buf.st_mode))
		{
			it->ret_code = 404;
			int errfd = open("./html/error.html", O_RDONLY);
			it->resource = "./html/error.html";
			struct stat stat_err;
			fstat(errfd, &stat_err);
			close(errfd);
			response = "Http/1.1 404 Not Found\r\n\
				    Date: Sun, 19 Mar 2023 16:28:66 GMT\r\n\
					Content-Type: ";
			response += "text / html; charset = utf - 8\r\n\
					Content-Length: ";
			response += stat_err.st_size;
			response += "\r\n\r\n";
		}
		else if (S_ISREG(stat_buf.st_mode))
		{
			it->ret_code = 200;
			response = "Http/1.1 200 OK\r\n\
						Date: Sun, 19 Mar 2023 16:28:66 GMT\r\n\
						Content-Type: ";
			response += it->file_type;
			response += "charset = utf - 8\r\n\
						Content-Length: ";
			response += stat_buf.st_size;
			response += "\r\n\r\n";
		}
	}
	it->w_buffer = response;
	return it->w_buffer.length();
}

int reactor::Http_Deal_Post(item*& it)
{

}

void reactor::Http_Response(item*& it)
{
	if (it->Request_type == HTTP_GET) {
		int len = send(it->clientfd, it->w_buffer.c_str(), it->w_buffer.length(), 0);
		int fd = open(it->resource.c_str(), O_RDONLY);
		struct stat stat_buf;
		fstat(fd, &stat_buf);
		int flag = fcntl(it->clientfd, F_GETFL, 0);
		flag &= ~O_NONBLOCK;
		fcntl(it->clientfd, F_SETFL, flag);
		int ret = sendfile(it->clientfd, fd, NULL, stat_buf.st_size);
		cout << "sendfile  size:" << ret << endl;
		if (ret == -1) cout << "sendfile errno: " << errno << endl;
		flag |= O_NONBLOCK;
		fcntl(it->clientfd, F_SETFL, flag);
		it->w_buffer = "\r\n\r\n";
	}
	else if (it->Request_type == HTTP_POST) {

	}
	//cout<<"Http length:"<<response.length()<<endl;
}
string Ret_Type(item*& it)
{
	int beg = it->resource.rfind('.');
	string str = it->resource.substr(beg + 1);
	if (str == "pdf") {
		return "application / pdf; ";
	}
	else if (str == "html") {
		return "text / html; ";
	}
	else if (str == "png") {
		return "image / png; ";
	}
	return "text / palin; ";
}
int reactor::Http_Request(item*& it)
{
	size_t indx = 0;
	string line;
	indx = Http_GetLine(it, line, indx);
	if (line.find("GET") != string::npos || line.find("get") != string::npos)
	{

		it->Request_type = HTTP_GET;
		int begind = 4;
		int endind = line.find(' ', 4);
		it->resource = HTML_SOURCE_ROOT + line.substr(begind, endind - begind);
		it->file_type = this->Ret_Type(it);
		Http_Deal_Get(it);
	}
	else if (line.find("POST") != string::npos || line.find("post") != string::npos)
	{
		it->Request_type = HTTP_POST;
		Http_Deal_Post(it);
	}
}

void reactor::Accept_cb(int sock)
{
	sockaddr_in sin;
	socklen_t len = sizeof(sin);
	int clientfd = accept(sock, (sockaddr*)&sin, &len);
	this->Add_To_Reactor(clientfd, EPOLLIN);
	int flag = fcntl(sock, F_GETFL, 0);
	flag |= O_NONBLOCK;
	fcntl(sock, F_SETFL, flag);
}

void reactor::Send_cb(item*& it)
{
	epoll_event ev;
	ev.data.fd = it->clientfd;
	Http_Response(it);
	int wlen = send(it->clientfd, it->w_buffer.c_str(), it->w_buffer.length(), 0);
	Del_From_Reactor(it->clientfd, EPOLLOUT);
	//cout<<"send length: "<<wlen<<endl;
	/*it->w_buffer = "";
	it->r_buffer = "";
	ev.events = EPOLLIN;*/
	//poll_ctl(this->epollfd, EPOLL_CTL_MOD, it->clientfd, &ev);
	if (wlen <= 0)
	{
		cout << "send failed" << endl;
	}
}
void reactor::Recv_cb(item*& it)
{
	epoll_event ev;
	ev.data.fd = it->clientfd;
	char* buffer = new char[MAX_BUFFER_SIZE];
	memset(buffer, 0, MAX_BUFFER_SIZE);
	int rlen = recv(it->clientfd, buffer, MAX_BUFFER_SIZE, 0);
	//cout<<rlen<<endl;
	if (rlen <= 0)
	{
		Del_From_Reactor(it->clientfd, EPOLLIN);
	}
	else
	{
		it->r_buffer = buffer;
		cout << buffer << endl;
		Http_Request(it);
		ev.events = EPOLLOUT;
		epoll_ctl(this->epollfd, EPOLL_CTL_MOD, it->clientfd, &ev);
	}
	delete buffer;
}

int main(int argc, char* argv[])
{
	int sockfd = Init_Sock(atoi(argv[1]));//创建sock
	reactor* R = new reactor();
	R->Add_To_Reactor(sockfd, EPOLLIN);
	R->Deal_Events(sockfd);
	R->Destory_Reactor();
	return 0;
}