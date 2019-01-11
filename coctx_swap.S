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

.globl coctx_swap
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
.type  coctx_swap, @function
#endif
coctx_swap:

#if defined(__i386__)
	leal 4(%esp), %eax //sp 
	movl 4(%esp), %esp 
	leal 32(%esp), %esp //parm a : &regs[7] + sizeof(void*)

	pushl %eax //esp ->parm a 

	pushl %ebp
	pushl %esi
	pushl %edi
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl -4(%eax)

	
	movl 4(%eax), %esp //parm b -> &regs[0]

	popl %eax  //ret func addr
	popl %ebx  
	popl %ecx
	popl %edx
	popl %edi
	popl %esi
	popl %ebp
	popl %esp
	pushl %eax //set ret func addr

	xorl %eax, %eax
	ret

#elif defined(__x86_64__)
	//pushq %rbp; movq %rsp,%rbp ---rsp指向rbp入栈位置,即rsp指向有效值
	//co_swap调用本函数coctx_swap
	//co_swap栈空间64B(EBP,ESP=EBP-64),<curr,pending_co>填充在EBP-56和EBP-64位置
	//call coctx_swap: pushl 返回地址(call指令下一条指令);movl coctx_swap %eip
  //栈空间:curr,pending_co,返回地址
	//%rsp指向返回地址
	//%rsp+8指向pending_co
	//%rsp+16指向curr
	//%rdi=curr->ctx,%rsi=pending_co->ctx
	//注:lea->传送寄存器的值,lea 8(%eax),%ecx;ecx=eax+8
	//   mov->传送寄存器的值指向的地址的值,mov 8(%eax),%ecx;ecx=[eax+8]
	//   r10,r11是调用者保存寄存器,只用保存其它14个寄存器
	leaq 8(%rsp),%rax      //rax=pending_co
	leaq 112(%rdi),%rsp    //rsp=&(curr->ctx::regs[14])
	//------通过把rsp指向ctx::regs[14],再利用pushq指令,把寄存器保存到ctx::regs[14]
	pushq %rax             //regs[13]=rax,rax为pending_co(%rsp),rax-8为返回地址
	                       //以后用该值恢复为%esp,再pushq待切换协程的返回地址
												 //再通过ret恢复到待切换协程
	pushq %rbx             //regs[12]=rbx
	pushq %rcx             //regs[11]=rcx
	pushq %rdx             //regs[10]=rdx

	pushq -8(%rax) //ret func addr,regs[9]=返回地址,暂存,后面再pushq该值

	pushq %rsi             //regs[8]=rsi
	pushq %rdi             //regs[7]=rdi,rdi=curr->ctx
	pushq %rbp             //regs[6]=rbp,co_swap栈基址
	pushq %r8              //regs[5]=r8
	pushq %r9              //regs[4]=r9
	pushq %r12             //regs[3]=r12
	pushq %r13             //regs[2]=r13
	pushq %r14             //regs[1]=r14
	pushq %r15             //regs[0]=r15
	//------至此保存完成
	
	movq %rsi, %rsp        //rsi=pending_co->ctx
	//------通过把rsp指向ctx::regs[0],再利用popq指令,把挂起协程数据恢复到寄存器ctx::regs[14]
	popq %r15              //regs[0]->%r15
	popq %r14              //regs[1]->%r14
	popq %r13              //regs[2]->%r13
	popq %r12              //regs[3]->%r12
	popq %r9               //regs[4]->%r9
	popq %r8               //regs[5]->%r8
	popq %rbp              //regs[6]->%rbp
	popq %rdi              //regs[7]->%rdi
	popq %rsi              //regs[8]->%rsi
	popq %rax //ret func addr//regs[9]->%rax,取出返回地址,后面pushq %rax
	popq %rdx              //regs[10]->%rdx
	popq %rcx              //regs[11]->%rcx
	popq %rbx              //regs[12]->%rbx
	popq %rsp              //regs[13]->%rsp,rsp指向pending_co,后面再pushq后,rsp指向返回地址,ret返回
	pushq %rax             //把真实返回地址push到栈中,rsp指向返回地址下一个地址
	
	xorl %eax, %eax
	ret
#endif
