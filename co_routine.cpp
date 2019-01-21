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
#include "co_routine_inner.h"
#include "co_epoll.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>

#include <poll.h>
#include <sys/time.h>
#include <errno.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

extern "C"
{
	extern void coctx_swap( coctx_t *,coctx_t* ) asm("coctx_swap");
};
using namespace std;
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env );
struct stCoEpoll_t;

struct stCoRoutineEnv_t
{
	stCoRoutine_t *pCallStack[ 128 ];
	int iCallStackSize;
	stCoEpoll_t *pEpoll; //协程环境关联的epoll对象

  //传入即将调度的协程pending_co,得到occupy_co,存入全局的env变量
  //  调入执行则备份occupy_co栈空间,coctx_swap后开始执行
  //  恢复执行则恢复pending_co栈空间,从coctx_swap后面恢复执行
	//for copy stack log lastco and nextco
	stCoRoutine_t* pending_co; //nextco
	stCoRoutine_t* occupy_co; //lastco
};
//int socket(int domain, int type, int protocol);
void co_log_err( const char *fmt,... )
{
}


#if defined( __LIBCO_RDTSCP__) 
static unsigned long long counter(void)
{
	register uint32_t lo, hi;
	register unsigned long long o;
	__asm__ __volatile__ (
			"rdtscp" : "=a"(lo), "=d"(hi)::"%rcx"
			);
	o = hi;
	o <<= 32;
	return (o | lo);

}
static unsigned long long getCpuKhz()
{
	FILE *fp = fopen("/proc/cpuinfo","r");
	if(!fp) return 1;
	char buf[4096] = {0};
	fread(buf,1,sizeof(buf),fp);
	fclose(fp);

	char *lp = strstr(buf,"cpu MHz");
	if(!lp) return 1;
	lp += strlen("cpu MHz");
	while(*lp == ' ' || *lp == '\t' || *lp == ':')
	{
		++lp;
	}

	double mhz = atof(lp);
	unsigned long long u = (unsigned long long)(mhz * 1000);
	return u;
}
#endif

static unsigned long long GetTickMS()
{
#if defined( __LIBCO_RDTSCP__) 
	static uint32_t khz = getCpuKhz();
	return counter() / khz;
#else
	struct timeval now = { 0 };
	gettimeofday( &now,NULL );
	unsigned long long u = now.tv_sec;
	u *= 1000;
	u += now.tv_usec / 1000;
	return u;
#endif
}

static pid_t GetPid()
{
    static __thread pid_t pid = 0;
    static __thread pid_t tid = 0;
    if( !pid || !tid || pid != getpid() )
    {
        pid = getpid();
#if defined( __APPLE__ )
		tid = syscall( SYS_gettid );
		if( -1 == (long)tid )
		{
			tid = pid;
		}
#elif defined( __FreeBSD__ )
		syscall(SYS_thr_self, &tid);
		if( tid < 0 )
		{
			tid = pid;
		}
#else 
        tid = syscall( __NR_gettid );
#endif

    }
    return tid;

}
/*
static pid_t GetPid()
{
	char **p = (char**)pthread_self();
	return p ? *(pid_t*)(p + 18) : getpid();
}
*/
template <class T,class TLink>
void RemoveFromLink(T *ap) //从ap所在的双向列表删除ap对象
{
  //取到双向列表入口Link
  //事件激活时,pfnPrepare根据iAllEventDetach==0从pTimeout事件超时队列中删除
  //stPoll_t::pLink置为空
  //协程返回到co_poll_inner:co_yield_env后面继续执行
  //  再次RemoveFromLink(&arg)时,pLink已经置为空,提前return结束
	TLink *lst = ap->pLink;
	if(!lst) return ;
	assert( lst->head && lst->tail );

	if( ap == lst->head )
	{
		lst->head = ap->pNext;
		if(lst->head)
		{
			lst->head->pPrev = NULL;
		}
	}
	else
	{
		if(ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext;
		}
	}

	if( ap == lst->tail )
	{
		lst->tail = ap->pPrev;
		if(lst->tail)
		{
			lst->tail->pNext = NULL;
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev;
	}

	ap->pPrev = ap->pNext = NULL;
	ap->pLink = NULL;
}

template <class TNode,class TLink>
void inline AddTail(TLink*apLink,TNode *ap)
{
	if( ap->pLink ) //ap必须先独立(从其它队列删除)才能添加到另一个队列
	{
		return ;
	}
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)ap;
		ap->pNext = NULL;
		ap->pPrev = apLink->tail;
		apLink->tail = ap;
	}
	else
	{
		apLink->head = apLink->tail = ap;
		ap->pNext = ap->pPrev = NULL;
	}
	ap->pLink = apLink;
}
template <class TNode,class TLink>
void inline PopHead( TLink*apLink )
{
	if( !apLink->head ) 
	{
		return ;
	}
	TNode *lp = apLink->head;
	if( apLink->head == apLink->tail )
	{
		apLink->head = apLink->tail = NULL;
	}
	else
	{
		apLink->head = apLink->head->pNext;
	}

	lp->pPrev = lp->pNext = NULL;
	lp->pLink = NULL;

	if( apLink->head )
	{
		apLink->head->pPrev = NULL;
	}
}

template <class TNode,class TLink>
void inline Join( TLink*apLink,TLink *apOther )
{
	//printf("apOther %p\n",apOther);
	if( !apOther->head )
	{
		return ;
	}
	TNode *lp = apOther->head;
	while( lp )
	{
		lp->pLink = apLink;
		lp = lp->pNext;
	}
	lp = apOther->head;
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)lp;
		lp->pPrev = apLink->tail;
		apLink->tail = apOther->tail;
	}
	else
	{
		apLink->head = apOther->head;
		apLink->tail = apOther->tail;
	}

	apOther->head = apOther->tail = NULL;
}

/////////////////for copy stack //////////////////////////
stStackMem_t* co_alloc_stackmem(unsigned int stack_size)
{
	stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t));
	stack_mem->occupy_co= NULL;
	stack_mem->stack_size = stack_size;
	stack_mem->stack_buffer = (char*)malloc(stack_size);
	stack_mem->stack_bp = stack_mem->stack_buffer + stack_size; //栈底
	return stack_mem;
}

stShareStack_t* co_alloc_sharestack(int count, int stack_size)
{
	stShareStack_t* share_stack = (stShareStack_t*)malloc(sizeof(stShareStack_t));
	share_stack->alloc_idx = 0;
	share_stack->stack_size = stack_size;

	//alloc stack array
	share_stack->count = count;
	stStackMem_t** stack_array = (stStackMem_t**)calloc(count, sizeof(stStackMem_t*));
	for (int i = 0; i < count; i++)
	{
		stack_array[i] = co_alloc_stackmem(stack_size);
	}
	share_stack->stack_array = stack_array;
	return share_stack;
}

static stStackMem_t* co_get_stackmem(stShareStack_t* share_stack)
{
	if (!share_stack)
	{
		return NULL;
	}
	int idx = share_stack->alloc_idx % share_stack->count;
	share_stack->alloc_idx++;

	return share_stack->stack_array[idx];
}


// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;
struct stCoEpoll_t
{
	int iEpollFd; //epoll描述符
	static const int _EPOLL_SIZE = 1024 * 10;

  //libco定义双向链表,入口称为Link(配置head和tail),链表每个元素有一个成员变量指向Link
	struct stTimeout_t *pTimeout; //pTimeout->pItems[60*1000],超时双向链表入口数组,默认6万个双向链表入口
                                //pTimeout->ullStart = 当前时间毫秒
                                //pTimeout->llStartIdx = 0

	struct stTimeoutItemLink_t *pstTimeoutList; //超时对象链表入口Link对象

	struct stTimeoutItemLink_t *pstActiveList; //事件激活对象链表入口Link
  //每轮事件处理时
  //先把事件激活对象从pTimeout[]的每个链表取出,放入激活对象链表
  //再把pTimeout[]中超时队列整体合并到激活对象链表(会设置超时标记)

	co_epoll_res *result;  //epoll事件数组,用于epoll_wait取出激活事件

};
typedef void (*OnPreparePfn_t)( stTimeoutItem_t *,struct epoll_event &ev, stTimeoutItemLink_t *active );
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *);
struct stTimeoutItem_t //双向链表元素结构基础元素
{

	enum
	{
		eMaxTimeout = 40 * 1000 //40s
	};
	stTimeoutItem_t *pPrev;
	stTimeoutItem_t *pNext;
	stTimeoutItemLink_t *pLink; //指向链表入口Link结构

  //元素超时时间(epoll事件对象),与当前时间差值决定插入stCoEpoll_t::pTimeout[]哪个队列
	unsigned long long ullExpireTime;

	OnPreparePfn_t pfnPrepare;
	OnProcessPfn_t pfnProcess;

	void *pArg; // routine ,协程对象
	bool bTimeout; //是否超时
};
struct stTimeoutItemLink_t //双向链表入口Link结构
{
	stTimeoutItem_t *head;
	stTimeoutItem_t *tail;

};
struct stTimeout_t
{
	stTimeoutItemLink_t *pItems; //超时双向链表对象,[60*1000]个链表
	int iItemSize;//pItems长度

	unsigned long long ullStart; //开始时间
	long long llStartIdx; //[ullStartIdx,ullCurr-ullStart]链表均超时,虚拟双向队列
};
stTimeout_t *AllocTimeout( int iSize )
{
	stTimeout_t *lp = (stTimeout_t*)calloc( 1,sizeof(stTimeout_t) );	

	lp->iItemSize = iSize;
	lp->pItems = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) * lp->iItemSize );

	lp->ullStart = GetTickMS();
	lp->llStartIdx = 0;

	return lp;
}
void FreeTimeout( stTimeout_t *apTimeout ) //释放超时队列
{
	free( apTimeout->pItems );
	free ( apTimeout );
}
//往apTimeout->pItems[60*1000]队列allNow位置插入对象apItem
int AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,unsigned long long allNow )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}
	if( allNow < apTimeout->ullStart )
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
					__LINE__,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	if( apItem->ullExpireTime < allNow )
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
					__LINE__,apItem->ullExpireTime,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart;

	if( diff >= (unsigned long long)apTimeout->iItemSize ) //太长时间没有执行,才会出现
	{
		diff = apTimeout->iItemSize - 1;
		co_log_err("CO_ERR: AddTimeout line %d diff %d",
					__LINE__,diff);

		//return __LINE__;
	}
	AddTail( apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize , apItem );

	return 0;
}
inline void TakeAllTimeout( stTimeout_t *apTimeout,unsigned long long allNow,stTimeoutItemLink_t *apResult )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}

	if( allNow < apTimeout->ullStart )
	{
		return ;
	}
	int cnt = allNow - apTimeout->ullStart + 1;
	if( cnt > apTimeout->iItemSize )
	{
		cnt = apTimeout->iItemSize;
	}
	if( cnt < 0 )
	{
		return;
	}
	for( int i = 0;i<cnt;i++)
	{
		int idx = ( apTimeout->llStartIdx + i) % apTimeout->iItemSize;
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( apResult,apTimeout->pItems + idx  );
	}
	apTimeout->ullStart = allNow;
	apTimeout->llStartIdx += cnt - 1;


}
//coctx_init初始化coctx_t ctx为空
//coctx_make时
//用该函数初始化coctx_t对象(64位):rsp,retaddr,rdi(co),rsi(0),注:rbp=0,rsp=stack_buffer-8
//栈空间+执行地址入口(retaddr)
//32位设计略有差异
//协程切入执行,交换寄存器环境后,切入到CoRoutineFunc函数执行
static int CoRoutineFunc( stCoRoutine_t *co,void * )
{
	if( co->pfn )
	{
    //协程首次切入时,调用协程函数体执行,co->arg为输入参数
    //如example_poll.cpp示例中poll_routine(v2)
		co->pfn( co->arg );
    //有两种可能情况:假设pfn调用一次poll
    //1.协程隐式切换,pfn中调用poll等函数实现协程切换了,后续应当进入状态2
    //2.协程执行完成,pfn中poll事件触发返回了
	}
	co->cEnd = 1; //状态1切换到状态2后,即协程执行完成,置cEnd标记

	stCoRoutineEnv_t *env = co->env;

	co_yield_env( env ); //切换回主协程

	return 0;
}



struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env, const stCoRoutineAttr_t* attr,
		pfn_co_routine_t pfn,void *arg )
{

	stCoRoutineAttr_t at; //at.stack_size初始化128KB,at.share_stack=NULL
	if( attr ) //主协程attr=NULL,at.stack_size=128KB
	{
		memcpy( &at,attr,sizeof(at) );
	}
	if( at.stack_size <= 0 )
	{
		at.stack_size = 128 * 1024;
	}
	else if( at.stack_size > 1024 * 1024 * 8 )
	{
		at.stack_size = 1024 * 1024 * 8;
	}

	if( at.stack_size & 0xFFF ) //4KB对齐
	{
		at.stack_size &= ~0xFFF;
		at.stack_size += 0x1000;
	}

	stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) );
	
	memset( lp,0,(long)(sizeof(stCoRoutine_t))); 


	lp->env = env;
	lp->pfn = pfn; //主协程pfn=NULL
	lp->arg = arg; //主协程arg=NULL

	stStackMem_t* stack_mem = NULL;
	if( at.share_stack ) //协程可能用共享栈
	{
		stack_mem = co_get_stackmem( at.share_stack);
		at.stack_size = at.share_stack->stack_size;
	}
	else //主协程单独分配栈空间,协程也可以单独分配栈空间
	{
		stack_mem = co_alloc_stackmem(at.stack_size);
	}
  //stack_mem
  //  ocupy_co=NULL
  //  stack_size=at.stack_size
  //  stack_buffer=malloc()
  //  stack_bp=stack_buffer+stack_size 栈底
	lp->stack_mem = stack_mem;

  //栈底指针在stack_mem->stack_bp
  //栈顶指针在stRoutine_t::ctx.ss_sp,也是stack_mem->stack_buffer
	lp->ctx.ss_sp = stack_mem->stack_buffer; //栈顶
	lp->ctx.ss_size = at.stack_size;

	lp->cStart = 0;
	lp->cEnd = 0;
	lp->cIsMain = 0;
	lp->cEnableSysHook = 0;
	lp->cIsShareStack = at.share_stack != NULL;

	lp->save_size = 0;
	lp->save_buffer = NULL;

	return lp;
}

int co_create( stCoRoutine_t **ppco,const stCoRoutineAttr_t *attr,pfn_co_routine_t pfn,void *arg )
{
	if( !co_get_curr_thread_env() ) 
	{
		co_init_curr_thread_env();
	}
	stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(), attr, pfn,arg );
	*ppco = co;
	return 0;
}
void co_free( stCoRoutine_t *co )
{
    if (!co->cIsShareStack) 
    {    
        free(co->stack_mem->stack_buffer);
        free(co->stack_mem);
    }   
    free( co );
}
void co_release( stCoRoutine_t *co )
{
    co_free( co );
}

void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co);

void co_resume( stCoRoutine_t *co )
{
	stCoRoutineEnv_t *env = co->env;
  //lpCurrRoutine为主协程时,并未coctx_make初始化
  //co_swap中调用coctx_swap,主协程接管main函数栈空间
	stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ];
  //对任务协程调用coctx_make初始化
	if( !co->cStart )
	{
    //初始化协程执行环境,初始化为调用CoRoutineFunc(stCoRoutine_t*,void*)
		coctx_make( &co->ctx,(coctx_pfn_t)CoRoutineFunc,co,0 );
		co->cStart = 1;
	}
	env->pCallStack[ env->iCallStackSize++ ] = co;
  //lpCurrRoutine接管main执行环境,同时调度co协程执行
  //co执行CoRoutineFunc,阻塞后调用co_yield_env自动切换到主协程环境
	co_swap( lpCurrRoutine, co );


}
void co_yield_env( stCoRoutineEnv_t *env )
{
	stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ];
	stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ];

	env->iCallStackSize--;

	co_swap( curr, last); //保存co_swap的栈空间,再次切回协程时,由co_swap返回这里,继而返回本函数的调用者
}

void co_yield_ct()
{

	co_yield_env( co_get_curr_thread_env() );
}
void co_yield( stCoRoutine_t *co )
{
	co_yield_env( co->env );
}

//上一个占用共享栈的协程(即当前占用共享栈的协程),需要为新协程腾出位置
void save_stack_buffer(stCoRoutine_t* occupy_co)
{
	///copy out
	stStackMem_t* stack_mem = occupy_co->stack_mem;
	int len = stack_mem->stack_bp - occupy_co->stack_sp; //按stack_mem::BP和Routine::SP拷贝备份栈空间

	if (occupy_co->save_buffer)
	{
		free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
	}

	occupy_co->save_buffer = (char*)malloc(len); //malloc buf;
	occupy_co->save_size = len;

	memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}

//输入:待切换协程已存入调用栈
//如果用共享栈,待切入协程pending_co已分配好共享栈,而共享栈可能正被某协程占用
//协程分配共享栈始终相同,保证上下文环境有效,备份别人,恢复自己
//本函数是站在pending_co角度编写的,即调度pending_co执行
//1.主协程主动调入pending_co执行,执行coctx_swap,阻塞在coctx_swap后面
//  如果协程栈已被其它协程占用,则备份其它协程栈空间
//2.主协程eventloop事件触发pending_co执行,从coctx_swap后面恢复执行
//  此时协程栈可能被占用,须恢复pending_co协程栈
//  主协程传入pending_co,可通过pending_co->stack_mem得到occupy_co,保存到env->pending_co/occupy_co
//  如果pending_co协程栈被备份过,则须从备份恢复栈空间,再继续执行pending_co
//3.不使用共享栈逻辑简单,不存在栈空间复用恢复问题
void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co)
{
 	stCoRoutineEnv_t* env = co_get_curr_thread_env();

	//get curr stack sp
	char c;
  //协程栈切换发生在co_swap函数,此处记录栈顶指针
  //所有发生切换的协程栈顶指针记录于协程stack_sp字段,栈基址则记录于栈stack_bp字段
	curr->stack_sp= &c; //stack_bp(初始为stack_buffer+stack_size) -> stack_sp栈空间备份

	if (!pending_co->cIsShareStack) //主协程或不使用共享栈协程
	{
		env->pending_co = NULL;
		env->occupy_co = NULL;
	}
	else //共享栈须备份,occupy_co&pending_co是指对共享栈的占用状况
	{//main函数切入时,curr=主协程,pending_co=协程
		env->pending_co = pending_co; //即将执行的协程
		//get last occupy co on the same stack mem
		stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co;
		//set pending co to occupy thest stack mem;
		pending_co->stack_mem->occupy_co = pending_co;

		env->occupy_co = occupy_co;
		if (occupy_co && occupy_co != pending_co) //pending_co要用occupy_co占用的栈
		{
			save_stack_buffer(occupy_co); //备份occupy_co栈
		}
	}
// 主协程执行:for循环中创建协程,co_resume->...
//协程切入[0]:co_resume->co_swap->coctx_swap->切入协程函数CoRoutineFunc
//            env->pending_co=协程
//            env->occupy_co=xxx
//协程执行[0]:协程函数CoRoutineFunc->poll->...
//协程挂起[0]:poll->co_poll_inner->co_yield_env->co_swap->coctx_swap->切入主协程
//            env->pending_co=NULL
//            env->occupy_co=NULL
// 主协程执行:eventloop
//协程切入[1]:事件触发->pfnProcess->co_resume->co_swap->coctx_swap
//            env->pending_co=协程
//            env->occupy_co=xxx
//协程执行[1]:从协程挂起[0]&协程执行[0]恢复执行
//            co_swap->co_yield_env->co_poll_inner->poll->CoRoutineFunc
//            co->pfn()执行完成并退出,标记协程执行结束
//            co_yield_env切换到主协程
// 主协程执行:co_eventloop,继续执行其它active协程

	//swap context
  //curr为主协程时,coctx_swap返回后,curr->ctx保存main函数上下文,主协程接管main,即main变成主协程
	coctx_swap(&(curr->ctx),&(pending_co->ctx) );

	stCoRoutineEnv_t* curr_env = co_get_curr_thread_env();
	stCoRoutine_t* update_occupy_co =  curr_env->occupy_co;
	stCoRoutine_t* update_pending_co = curr_env->pending_co;
	
	if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
	{
		//resume stack buffer
		if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
		{
			memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
		}
	}
}



//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
struct stPollItem_t ;
struct stPoll_t : public stTimeoutItem_t 
{
	struct pollfd *fds;
	nfds_t nfds; // typedef unsigned long int nfds_t;

	stPollItem_t *pPollItems;

	int iAllEventDetach;

	int iEpollFd;

	int iRaiseCnt;


};
struct stPollItem_t : public stTimeoutItem_t
{
	struct pollfd *pSelf;
	stPoll_t *pPoll;

	struct epoll_event stEvent;
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0;	
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	if( events & POLLRDNORM ) e |= EPOLLRDNORM;
	if( events & POLLWRNORM ) e |= EPOLLWRNORM;
	return e;
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0;	
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	if( events & EPOLLRDNORM ) e |= POLLRDNORM;
	if( events & EPOLLWRNORM ) e |= POLLWRNORM;
	return e;
}

static __thread stCoRoutineEnv_t* gCoEnvPerThread = NULL;

void co_init_curr_thread_env()
{
	gCoEnvPerThread = (stCoRoutineEnv_t*)calloc( 1, sizeof(stCoRoutineEnv_t) );
	stCoRoutineEnv_t *env = gCoEnvPerThread;

	env->iCallStackSize = 0;
	struct stCoRoutine_t *self = co_create_env( env, NULL, NULL,NULL );
	self->cIsMain = 1;

	env->pending_co = NULL;
	env->occupy_co = NULL;

	coctx_init( &self->ctx );

	env->pCallStack[ env->iCallStackSize++ ] = self;

	stCoEpoll_t *ev = AllocEpoll();
	SetEpoll( env,ev );
}
stCoRoutineEnv_t *co_get_curr_thread_env()
{
	return gCoEnvPerThread;
}

void OnPollProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

//epoll_wait(epfd,result,EPOLL_SIZE,1)
//    ap = result->events[i].data.ptr; //(stTimeoutItem_t*)
//     e = result->events[i]
//active = 激活事件队列
void OnPollPreparePfn( stTimeoutItem_t * ap,struct epoll_event &e,stTimeoutItemLink_t *active )
{
	stPollItem_t *lp = (stPollItem_t *)ap;
  //触发的epoll事件转换成poll事件保存到poll事件中
  //协程从revents中获取事件
	lp->pSelf->revents = EpollEvent2Poll( e.events );


  //从stPollItem_t对象pPoll指针找到事件集合对象stPoll_t
	stPoll_t *pPoll = lp->pPoll;
	pPoll->iRaiseCnt++; //集合对象事件次数累加

	if( !pPoll->iAllEventDetach ) //首次激活执行
	{
		pPoll->iAllEventDetach = 1;

    //co_poll_inner把stPoll_t对象添加到超时队列,此处移除
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( pPoll );

    //再添加到active队列
		AddTail( active,pPoll );

	}
}


void co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg ) //pfn(arg)
{
	if( !ctx->result )
	{
		ctx->result =  co_epoll_res_alloc( stCoEpoll_t::_EPOLL_SIZE );
	}
	co_epoll_res *result = ctx->result;


	for(;;)
	{
		int ret = co_epoll_wait( ctx->iEpollFd,result,stCoEpoll_t::_EPOLL_SIZE, 1 );

		stTimeoutItemLink_t *active = (ctx->pstActiveList); //马上要执行的协程,CoCond中处理的协程
		stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList); //超时的协程,既然已超时,当然也必须马上执行了,所以也会添加到activelist

		memset( timeout,0,sizeof(stTimeoutItemLink_t) );

		for(int i=0;i<ret;i++)
		{
			stTimeoutItem_t *item = (stTimeoutItem_t*)result->events[i].data.ptr;
			if( item->pfnPrepare )
			{
        //OnPollPreparePfn,取出item.stEpoll_t对象,如果是首次激活则添加到active队列
				item->pfnPrepare( item,result->events[i],active );
			}
			else //无事件处理时,fds=NULL,如定时器事件
			{
				AddTail( active,item ); //取出stEpollItem_t对象添加到active队列
			}
		}


		unsigned long long now = GetTickMS();
		TakeAllTimeout( ctx->pTimeout,now,timeout ); //超时对象添加到timeout队列,并更新基准时间为now

		stTimeoutItem_t *lp = timeout->head;
		while( lp )
		{
			//printf("raise timeout %p\n",lp);
			lp->bTimeout = true; //打超时标记
			lp = lp->pNext;
		}

		Join<stTimeoutItem_t,stTimeoutItemLink_t>( active,timeout ); //超时任务也放到active列表

		lp = active->head;
		while( lp ) //执行协程
		{

			PopHead<stTimeoutItem_t,stTimeoutItemLink_t>( active );
            if (lp->bTimeout && now < lp->ullExpireTime)  //超时了,但时间判断并未超时,什么情况下出现?
			{
				int ret = AddTimeout(ctx->pTimeout, lp, now); //假超时则继续添加到超时队列
				if (!ret) 
				{
					lp->bTimeout = false;
					lp = active->head;
					continue;
				}
			}
			if( lp->pfnProcess ) //active协程或真正超时协程
			{
				lp->pfnProcess( lp ); //pfnProcess=OnPollProcessEvent,co_resume(pArg)
			}

			lp = active->head;
		}
		if( pfn ) //额外功能
		{
			if( -1 == pfn( arg ) )
			{
				break;
			}
		}

	}
}
void OnCoroutineEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}


stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1,sizeof(stCoEpoll_t) );

	ctx->iEpollFd = co_epoll_create( stCoEpoll_t::_EPOLL_SIZE );
	ctx->pTimeout = AllocTimeout( 60 * 1000 );
	
	ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
	ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );


	return ctx;
}

void FreeEpoll( stCoEpoll_t *ctx )
{
	if( ctx )
	{
		free( ctx->pstActiveList );
		free( ctx->pstTimeoutList );
		FreeTimeout( ctx->pTimeout );
		co_epoll_res_free( ctx->result );
	}
	free( ctx );
}

stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
	return env->pCallStack[ env->iCallStackSize - 1 ];
}
stCoRoutine_t *GetCurrThreadCo( )
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	if( !env ) return 0;
	return GetCurrCo(env);
}
stCoRoutine_t *GetCurrThreadCo0( int idx )
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	if( !env ) return 0;
	return env->pCallStack[0];
}



typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
//pollfunc有两处使用:co_poll(pollfunc=NULL),poll(pollfunc=poll)
//ctx=协程环境->pEpoll
int co_poll_inner( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc)
{
    if (timeout == 0)
	{
		return pollfunc(fds, nfds, timeout);
	}
	if (timeout < 0)
	{
		timeout = INT_MAX;
	}
	int epfd = ctx->iEpollFd;
	stCoRoutine_t* self = co_self();

	//1.struct change
  //struct stPoll_t:stTimeoutItem_t **关注事件集合**
  //  struct pollfd *fds;
  //    {
  //      int fd;
  //      short event; //等待的事件
  //      short revent;//实际发生的事件
  //    }
  //  ull nfds;                //关注事件个数
  //  stPollItem_t *pPollItem; //关注事件列表
  //    {
  //      struct pollfd *pSelf; //指向事件集合对象fds[i]
  //      stPoll_t *pPoll; //指向事件集合对象
  //      struct epoll_event stEvent;
  //
  //      //双向队列成员变量
  //      OnPreparePfn_t pfnPrepare;
  //      OnProcessPfn_t pfnProcess;
  //      void *pArg; //routine
  //      bool bTimeout;
  //    }
  //  int iAllEventDetach;
  //  int iEpollFd;
  //  int iRaiseCnt; epoll激活次数
	stPoll_t& arg = *((stPoll_t*)malloc(sizeof(stPoll_t)));
	memset( &arg,0,sizeof(arg) );

	arg.iEpollFd = epfd;
	arg.fds = (pollfd*)calloc(nfds, sizeof(pollfd));
	arg.nfds = nfds;

	stPollItem_t arr[2];
	if( nfds < sizeof(arr) / sizeof(arr[0]) && !self->cIsShareStack) //关注1个事件,且非共享栈
	{
		arg.pPollItems = arr;
	}	
	else //多个事件或共享栈
	{
		arg.pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );
	}
	memset( arg.pPollItems,0,nfds * sizeof(stPollItem_t) );

	arg.pfnProcess = OnPollProcessEvent; //co_resume(pArg)
	arg.pArg = GetCurrCo( co_get_curr_thread_env() ); //*pArg为协程对象
	
	
	//2. add epoll
	for(nfds_t i=0;i<nfds;i++)
	{
		arg.pPollItems[i].pSelf = arg.fds + i; //事件[i]
		arg.pPollItems[i].pPoll = &arg;        //事件集合

		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
		struct epoll_event &ev = arg.pPollItems[i].stEvent;

		if( fds[i].fd > -1 )
		{
      //poll事件转换为epoll事件监听
			ev.data.ptr = arg.pPollItems + i; //事件集合stPoll_t中某事件stPollItem_t
			ev.events = PollEvent2Epoll( fds[i].events );

			int ret = co_epoll_ctl( epfd,EPOLL_CTL_ADD, fds[i].fd, &ev );
			if (ret < 0 && errno == EPERM && nfds == 1 && pollfunc != NULL) //EPERM - 文件fd不支持epoll
			{
				if( arg.pPollItems != arr )
				{
					free( arg.pPollItems );
					arg.pPollItems = NULL;
				}
				free(arg.fds);
				free(&arg);
				return pollfunc(fds, nfds, timeout); //不支持epoll,则继续使用poll
			}
		}
		//if fail,the timeout would work
	}

	//3.add timeout

	unsigned long long now = GetTickMS();
	arg.ullExpireTime = now + timeout;
  //把stPoll_t事件集合对象添加到超时队列中
  //1.stPoll_t事件列表中事件stPollItem_t添加到epoll中
  //2.stPoll_t本身添加到协程环境epoll对象超时队列中
	int ret = AddTimeout( ctx->pTimeout,&arg,now );
	int iRaiseCnt = 0;
	if( ret != 0 )
	{
		co_log_err("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
				ret,now,timeout,arg.ullExpireTime);
		errno = EINVAL;
		iRaiseCnt = -1; //如果执行出错,后面也会清理现场并返回iRaiseCnt=-1

	}
    else
	{
		co_yield_env( co_get_curr_thread_env() ); //切换协程,后面不再执行(协程切换统一调用co_swap)
		iRaiseCnt = arg.iRaiseCnt;//事件触发,切回协程后,继续执行,并返回触发的事件
	}

    //co_poll->co_poll_inner隐式切换协程
    //  或
    //hook版的系统调用通过hook版的poll->co_poll_inner隐式切换协程
    //  co_yield_env
    //隐式切换协程事件触发切回后,继续后面执行
    {//事件触发或AddTimeout出错时,均执行以下操作
		//clear epoll status and memory
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &arg );
		for(nfds_t i = 0;i < nfds;i++)
		{
			int fd = fds[i].fd;
			if( fd > -1 )
			{
				co_epoll_ctl( epfd,EPOLL_CTL_DEL,fd,&arg.pPollItems[i].stEvent ); //从epoll中删除事件
			}
			fds[i].revents = arg.fds[i].revents; //返回触发的事件
		}


		if( arg.pPollItems != arr )
		{
			free( arg.pPollItems );
			arg.pPollItems = NULL;
		}

		free(arg.fds);
		free(&arg);
	}

	return iRaiseCnt;
}

int	co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms )
{
	return co_poll_inner(ctx, fds, nfds, timeout_ms, NULL);
}

void SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev )
{
	env->pEpoll = ev;
}
stCoEpoll_t *co_get_epoll_ct()
{
	if( !co_get_curr_thread_env() )
	{
		co_init_curr_thread_env();
	}
	return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co;
	void *value;

	enum 
	{
		size = 1024
	};
};
//主协程spec用线程的
//协程spec用协程自己的aSpec[1024]
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_getspecific( key );
	}
	return co->aSpec[ key ].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo();
  //当前协程为空(主协程未接管main时)或当前协程已接管为主协程
  //主协程:直接设置线程
	if( !co || co->cIsMain )
	{
		return pthread_setspecific( key,value );
	}
	co->aSpec[ key ].value = (void*)value; //给当前协程调协key-val
	return 0;
}



void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 0;
	}
}
bool co_is_enable_sys_hook()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	return ( co && co->cEnableSysHook );
}

stCoRoutine_t *co_self()
{
	return GetCurrThreadCo();
}

//co cond
struct stCoCond_t;
struct stCoCondItem_t 
{
	stCoCondItem_t *pPrev;
	stCoCondItem_t *pNext;
	stCoCond_t *pLink;

	stTimeoutItem_t timeout;
};
struct stCoCond_t
{
	stCoCondItem_t *head;
	stCoCondItem_t *tail;
};
static void OnSignalProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

stCoCondItem_t *co_cond_pop( stCoCond_t *link );
int co_cond_signal( stCoCond_t *si ) //把CoCond_t队首协程添加到activelist,下次直接执行
{
	stCoCondItem_t * sp = co_cond_pop( si );
	if( !sp ) 
	{
		return 0;
	}
	RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );

	AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );

	return 0;
}
int co_cond_broadcast( stCoCond_t *si ) //把CoCond_t队列所有协程都添加到activelist,下次直接执行
{
	for(;;)
	{
		stCoCondItem_t * sp = co_cond_pop( si );
		if( !sp ) return 0;

		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );

		AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );
	}

	return 0;
}


int co_cond_timedwait( stCoCond_t *link,int ms ) //添加当前协程到CoCond队列
{
	stCoCondItem_t* psi = (stCoCondItem_t*)calloc(1, sizeof(stCoCondItem_t));
  //epoll事件处理中用stPoll_t:stTimeoutItem_t
  //信号处理中采用stTimeoutItem_t:{pfnProcess,pArg}
  //协程切换方式相同,均利用<pArg,pfnProcess,pfnPrepare>
	psi->timeout.pArg = GetCurrThreadCo(); //当前协程
	psi->timeout.pfnProcess = OnSignalProcessEvent;

	if( ms > 0 )
	{
		unsigned long long now = GetTickMS();
		psi->timeout.ullExpireTime = now + ms;

    //协程添加到定时器队列
		int ret = AddTimeout( co_get_curr_thread_env()->pEpoll->pTimeout,&psi->timeout,now ); //超时队列必须添加
		if( ret != 0 )
		{
			free(psi);
			return ret;
		}
	}
	AddTail( link, psi); //添加协程CoCond_t信号队列

	co_yield_ct(); //切换协程


	RemoveFromLink<stCoCondItem_t,stCoCond_t>( psi ); //切回协程后,从CoCond_t队列删除
	free(psi);

	return 0;
}
stCoCond_t *co_cond_alloc()
{
	return (stCoCond_t*)calloc( 1,sizeof(stCoCond_t) );
}
int co_cond_free( stCoCond_t * cc )
{
	free( cc );
	return 0;
}


stCoCondItem_t *co_cond_pop( stCoCond_t *link )
{
	stCoCondItem_t *p = link->head;
	if( p )
	{
		PopHead<stCoCondItem_t,stCoCond_t>( link );
	}
	return p;
}
