/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include "co_routine.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <stack>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <vector>
#include <set>
#include <unistd.h>

#ifdef __FreeBSD__
#include <cstring>
#endif

using namespace std;

struct task_t
{
	stCoRoutine_t *co;
	int fd;
	struct sockaddr_in addr;
};

static int SetNonBlock(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0); //同原始fcntl,获取flags
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags); //设置标记,记录在g_rpchook_socket_fd
    return ret;
}



static void SetAddr(const char *pszIP,const unsigned short shPort,struct sockaddr_in &addr)
{
	bzero(&addr,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(shPort);
	int nIP = 0;
	if( !pszIP || '\0' == *pszIP   
	    || 0 == strcmp(pszIP,"0") || 0 == strcmp(pszIP,"0.0.0.0") 
		|| 0 == strcmp(pszIP,"*") 
	  )
	{
		nIP = htonl(INADDR_ANY);
	}
	else
	{
		nIP = inet_addr(pszIP);
	}
	addr.sin_addr.s_addr = nIP;

}

static int CreateTcpSocket(const unsigned short shPort  = 0 ,const char *pszIP  = "*" ,bool bReuse  = false )
{
	int fd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP); //创建套接字,并分配g_rpchook_socket_fd
	if( fd >= 0 )
	{
		if(shPort != 0)
		{
			if(bReuse)
			{
				int nReuseAddr = 1;
				setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&nReuseAddr,sizeof(nReuseAddr));
			}
			struct sockaddr_in addr ;
			SetAddr(pszIP,shPort,addr);
			int ret = bind(fd,(struct sockaddr*)&addr,sizeof(addr));
			if( ret != 0)
			{
				close(fd);
				return -1;
			}
		}
	}
	return fd;
}

static void *poll_routine( void *arg )
{
	co_enable_hook_sys(); //main函数执行时,未初始化协程环境,尝试获取当前协程时返回NULL,enable调用未改变任何东西

	vector<task_t> &v = *(vector<task_t>*)arg;
	for(size_t i=0;i<v.size();i++)
	{
		int fd = CreateTcpSocket();
		SetNonBlock( fd );
		v[i].fd = fd;
    printf("fd=%d\n", fd);

		int ret = connect(fd,(struct sockaddr*)&v[i].addr,sizeof( v[i].addr )); 
    unsigned nIP = v[i].addr.sin_addr.s_addr;
    unsigned char *p = (unsigned char*)&nIP;
		printf("co %p connect i %ld [%d.%d.%d.%d] ret %d errno %d (%s)\n",
			co_self(),i,p[0],p[1],p[2],p[3],ret,errno,strerror(errno));
	}
	struct pollfd *pf = (struct pollfd*)calloc( 1,sizeof(struct pollfd) * v.size() );

	for(size_t i=0;i<v.size();i++)
	{
		pf[i].fd = v[i].fd;
		pf[i].events = ( POLLOUT | POLLERR | POLLHUP );
	}
	set<int> setRaiseFds;
	size_t iWaitCnt = v.size();
	for(;;)
	{
    //正常来说,co_create & co_resume时初始化协程环境
    //co_resume后env->pCallStack[主协程,新协程]
    //执行协程至此,新协程poll->co_poll_inner->co_yield_env->co_swap,协程切换,env->pCallStack[主协程]
    //如果是main调用,因为未初始化协程环境,enable_sys_hook=false,所以下面实际是调用系统poll函数
		int ret = poll( pf,iWaitCnt,1000 );
    //epoll事件触发时,通过data.ptr找到Time对象(保存协程信息)
    //再调用pfnProcess(Time)->OnPollProcessEvent->co_resume切换到co_swap->co_yield_env->co_poll_inner->poll
    errno = ret<0?errno:0;
		printf("co %p poll wait %ld ret %d, errno %d (%s)\n",
				co_self(),iWaitCnt,ret,errno,strerror(errno));
		for(int i=0;i<(int)iWaitCnt;i++) {
      if (pf[i].revents & POLLOUT) {
        //POLLOUT事件表示connect连接建立成功
        printf("co %p fire fd %d revents 0x%X(POLLOUT 0x%X POLLERR 0x%X POLLHUP 0x%X)\n",
            co_self(),
            pf[i].fd,
            pf[i].revents,
            POLLOUT,
            POLLERR,
            POLLHUP
            );
        setRaiseFds.insert( pf[i].fd );
      }
    }
    //如果事件没有全部触发,再次到for(;;),poll时情况复杂
    //这里实际简化了,for(;;)把所有事件都放在setRaiseFds中了,所以不会出现多次循环调用poll的情况
    if( setRaiseFds.size() == v.size())
    {
			break;
		}
		//if( ret <= 0 )
		if( ret < 0 )
		{
			break;
		}

		iWaitCnt = 0;
		for(size_t i=0;i<v.size();i++)
		{
			if( setRaiseFds.find( v[i].fd ) == setRaiseFds.end() )
			{
				pf[ iWaitCnt ].fd = v[i].fd;
				pf[ iWaitCnt ].events = ( POLLOUT | POLLERR | POLLHUP );
				++iWaitCnt;
			}
		}
	}
	for(size_t i=0;i<v.size();i++)
	{
		close( v[i].fd ); //关闭连接
		v[i].fd = -1;
	}

	printf("co %p task cnt %ld fire %ld\n",
			co_self(),v.size(),setRaiseFds.size() );
	return 0;
}
int main(int argc,char *argv[])
{
	vector<task_t> v;
	for(int i=1;i<argc;i+=2)
	{
		task_t task = { 0 };
		SetAddr( argv[i],atoi(argv[i+1]),task.addr );
		v.push_back( task );
	}

//------------------------------------------------------------------------------------
	printf("--------------------- main -------------------\n");
	vector<task_t> v2 = v;
	poll_routine( &v2 ); //建立连接然后关闭
	printf("--------------------- routine -------------------\n");

	for(int i=0;i<1;i++)
	{
		stCoRoutine_t *co = 0;
		vector<task_t> *v2 = new vector<task_t>();
		*v2 = v;
		co_create( &co,NULL,poll_routine,v2 ); //main函数初次执行则建立协程环境及主协程
		printf("routine i %d\n",i);
    //main函数初次执行,co_swap接管main栈空间,poll_routine协程开始执行
    //poll_routine调用poll->co_poll_inner->把协程添加到epoll事件中,然后co_yield_env切换到上一个协程(主协程)
    //主协程回到co_resume->co_swap->main继续for循环,创建新协程
		co_resume( co );
	}

	co_eventloop( co_get_epoll_ct(),0,0 ); //事件轮询

	return 0;
}
//./example_poll 127.0.0.1 12365 127.0.0.1 12222 192.168.1.1 1000 192.168.1.2 1111

