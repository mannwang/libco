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

#ifndef __CO_ROUTINE_H__
#define __CO_ROUTINE_H__

#include <stdint.h>
#include <sys/poll.h>
#include <pthread.h>

//1.struct

struct stCoRoutine_t;
struct stShareStack_t;

struct stCoRoutineAttr_t
{
	int stack_size;
	stShareStack_t*  share_stack;
	stCoRoutineAttr_t()
	{
		stack_size = 128 * 1024;
		share_stack = NULL;
	}
}__attribute__ ((packed));

struct stCoEpoll_t;
typedef int (*pfn_co_eventloop_t)(void *);
typedef void *(*pfn_co_routine_t)( void * );

//2.co_routine

int 	co_create( stCoRoutine_t **co,const stCoRoutineAttr_t *attr,void *(*routine)(void*),void *arg );
//协程开始
void    co_resume( stCoRoutine_t *co );
//协程暂停,本质上是通过协程环境记录的状态信息恢复last协程,co_yield_env
void    co_yield( stCoRoutine_t *co ); //协程环境:co->env
void    co_yield_ct(); //ct = current thread,协程环境:co_get_curr_thread_env
void    co_release( stCoRoutine_t *co );

stCoRoutine_t *co_self(); //调用栈栈顶协程即为当前协程,返回栈顶协程

//libco协程在事件处理上有两种方式发生切换(另有直接切换方式:co_resume/co_yield*)
//一种是隐式的,调用hook方式实现的send等函数时,send调用poll实现协程自动切换
//一种是显式的,开发者调用co_poll实现协程切换
//显式(send->poll->co_poll_inner(poll))和隐式(co_poll->co_poll_inner(NULL))均调用co_poll_inner实现
int		co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms );
void 	co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg );

//3.specific

int 	co_setspecific( pthread_key_t key, const void *value ); //设置协程特定数据
void *	co_getspecific( pthread_key_t key );

//4.event

stCoEpoll_t * 	co_get_epoll_ct(); //ct = current thread,获取当前线程stCoEpoll_t

//5.hook syscall ( poll/read/write/recv/send/recvfrom/sendto )

void 	co_enable_hook_sys();  //获取当前协程(协程栈),并打开hook
void 	co_disable_hook_sys();  //获取当前协程,并关闭hook
bool 	co_is_enable_sys_hook(); //获取当前协程的hook状态

//6.sync
struct stCoCond_t; //stCoCondItem_t *head,*tail,双向队列入口

stCoCond_t *co_cond_alloc();
int co_cond_free( stCoCond_t * cc );

int co_cond_signal( stCoCond_t * );
int co_cond_broadcast( stCoCond_t * );
int co_cond_timedwait( stCoCond_t *,int timeout_ms );

//7.share stack
stShareStack_t* co_alloc_sharestack(int iCount, int iStackSize); //创建共享栈

//8.init envlist for hook get/set env
void co_set_env_list( const char *name[],size_t cnt); //设置线程(协程)环境变量

void co_log_err( const char *fmt,... );
#endif

