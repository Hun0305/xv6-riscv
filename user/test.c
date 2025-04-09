#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "proc.h"
#include "syscall.h"

/*
    file 내용 출력하는건 전부 N O T
    freemem의 출력은 mmap() 출력 후 증가하고, munmap() 출력 후 감소하는 것만 확인되면 O
*/

int main(int argc, char **argv)
{
    	int flag = 0;
	char* test, *test2, *test3, *test4;
	int option = atoi(argv[1]);
        int fd = open("README",O_RDWR);
        printf(1,"fd is %d option %d\n",fd, option);
	switch(option){
	    case 1:
		goto ANONY;
		break;
	    case 2:
		goto FILEMAP;
		break;
	    default:
		goto FORK;
	}
ANONY:
        printf(1,"FIRST freemem now is %d\n",freemem());
        test = (char*)mmap(0, 4096, PROT_READ|PROT_WRITE,MAP_POPULATE|MAP_ANONYMOUS, -1, 0);
	printf(1, "- addr: %x\n", (uint)test);
	printf(1,"SECOND freemem now is %d\n",freemem());
        test2 = (char*)mmap(4096,4096,PROT_READ|PROT_WRITE,MAP_ANONYMOUS,-1,0);
	printf(1, "- addr: %x\n", (uint)test2);
	
	printf(1, "LAST freemem now is %d\n",freemem());
	printf(1, "ANONYMOUS test done\n");
	
	close(fd);
	exit();
	return 1;
FILEMAP:
	printf(1,"THIRD freemem now is %d\n",freemem());
	test3 = (char *)mmap(8192, 4096, PROT_READ|PROT_WRITE,MAP_POPULATE, fd, 0);
        test3[2286]='\0';
	printf(1, "- addr: %x\n", (uint)test3);
	printf(1, "- fd data: %c %c %c\n", test3[0], test3[1], test3[2]);
        printf(1,"FOURTH freemem now is %d\n",freemem());
        test4 = (char*)mmap(16384,4096,PROT_READ|PROT_WRITE,0,fd,0);
        test4[2286]='\0';
	printf(1, "- addr: %x\n", (uint)test4);
	printf(1, "- pf data: %c %c %c\n", test4[0], test4[1], test4[2]);
	printf(1,"LAST freemem now is %d\n",freemem());
	printf(1, "FILEMAP test done\n");
	close(fd);
	exit();
	return 0;
FORK:
        printf(1,"FIRST freemem now is %d\n",freemem());
        test = (char*)mmap(0, 4096, PROT_READ|PROT_WRITE,MAP_POPULATE|MAP_ANONYMOUS, -1, 0);
	printf(1, "- addr: %x\n", (uint)test);
	
	printf(1,"SECOND freemem now is %d\n",freemem());
        test2 = (char*)mmap(4096,4096,PROT_READ|PROT_WRITE,MAP_ANONYMOUS,-1,0);
	printf(1, "- addr: %x\n", (uint)test2);
	
	printf(1,"THIRD freemem now is %d\n",freemem());
	test3 = (char *)mmap(8192, 4096, PROT_READ|PROT_WRITE,MAP_POPULATE, fd, 0);
        test3[2286]='\0';
	printf(1, "- addr: %x\n", (uint)test3);
	printf(1, "- fd data: %c %c %c\n", test3[0], test3[1], test3[2]);
	printf(1,"FOURTH freemem now is %d\n",freemem());
        test4 = (char*)mmap(16384,4096,PROT_READ|PROT_WRITE,0,fd,0);
        test4[2286]='\0';
	printf(1, "- addr: %x\n", (uint)test4);
	printf(1, "- pf data: %c %c %c\n", test4[0], test4[1], test4[2]);
	printf(1,"LAST freemem now is %d\n",freemem());
	flag = 1;
	printf(1,"\n\n\n!!fork start!\n\n\n");
	if(flag == 1){
		int f;
		if((f=fork())==0){
			printf(1, "\n\nCHILD START\n");
			int x;
			int base = 0x40000000;
			
			x = munmap(0+base);
			printf(1,"0: %d unmap results\n",x);
			printf(1,"freemem now is %d\n",freemem());
			x = munmap(4096+base);
			printf(1,"4096: %d unmap results\n",x);
			printf(1,"freemem now is %d\n",freemem());
			
			printf(1, "- fd data: %c %c %c\n", test3[0], test3[1], test3[2]);
			x = munmap(8192+base);
			printf(1,"8192: %d unmap results\n",x);
			printf(1,"freemem now is %d\n",freemem());
			
			printf(1, "- pf data: %c %c %c\n", test4[0], test4[1], test4[2]);
			x = munmap(16384+base);
			printf(1,"16384: %d unmap results\n",x);
			printf(1,"freemem now is %d\n",freemem());
			
			
			exit();
			return 0;
		}
		else{
		    	wait();
			printf(1, "\n\nPARENT START\n");
			int x;
			int base = 0x40000000;
			x = munmap(0+base);
			printf(1,"0: %d unmap results\n",x);
			printf(1,"freemem now is %d\n",freemem());
			x = munmap(4096+base);
			printf(1,"4096: %d unmap results\n",x);
			printf(1,"freemem now is %d\n",freemem());
			x = munmap(8192+base);
			printf(1,"8192: %d unmap results\n",x);
			printf(1,"freemem now is %d\n",freemem());
			x = munmap(16384+base);
			printf(1,"16384: %d unmap results\n",x);
			printf(1,"freemem now is %d\n",freemem());
		}
	}
	printf(1,"Lastly freemem now is %d\n",freemem());
        close(fd);
	exit();
        return 0;
}

