#include "myos.h"
#include "sys/stat.h"
#define CHECK(x) do { if(!(x)) { printf("SPAWN FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static int child(int argc,char **argv) {
    CHECK(argc==7 && !strcmp(argv[1],"child"));
    CHECK(getppid()==atoi(argv[2]));
    char name[24]={0},cwd[128]={0},byte;
    CHECK(account(5,name,NULL)==0 && !strcmp(name,argv[3]));
    CHECK(getcwd(cwd,sizeof(cwd))==cwd && !strcmp(cwd,argv[4]));
    CHECK(read(0,&byte,1)==0); /* stdin was replaced with /dev/null */
    CHECK(!strcmp(argv[6],"argument with spaces"));
    CHECK(write(atoi(argv[5]),"ok",2)==2); /* inherited descriptor */
    return 23;
}
static int exercise(void) {
    char name[24]={0},cwd[128]={0},parent[24],out[24],reply[3]={0};
    CHECK(account(5,name,NULL)==0 && getcwd(cwd,sizeof(cwd))==cwd);
    int fds[2]; CHECK(pipe(fds)==0);
    int input=open("/dev/null",O_READ); CHECK(input>=0);
    snprintf(parent,sizeof(parent),"%d",getpid());
    snprintf(out,sizeof(out),"%d",fds[1]);
    char *args[]={"spawncheck","child",parent,name,cwd,out,"argument with spaces",NULL};
    int pid=spawn("/bin/spawncheck",args,input); CHECK(pid>0);
    close(input); close(fds[1]);
    CHECK(read(fds[0],reply,2)==2 && !strcmp(reply,"ok")); close(fds[0]);
    int status=-1; CHECK(waitpid(pid,&status,0)==pid && status==23);
    char after[128]; CHECK(getcwd(after,sizeof(after))==after && !strcmp(after,cwd));
    CHECK(spawn("/missing",args,-1)==-ENOENT);
    CHECK(spawn((char *)0xffffffff,args,-1)==-EFAULT);
    CHECK(spawn("/bin/spawncheck",(char **)0xffffffff,-1)==-EFAULT);
    CHECK(spawn("/bin/spawncheck",args,1000)==-EBADF);
    CHECK(spawn("/bin/spawncheck",args,-2)==-EBADF);
    CHECK(spawn("/tmp",args,-1)==-EISDIR);
    int fd=open("/tmp/not-elf",O_WRITE);CHECK(fd>=0);
    char data[128];memset(data,0,sizeof(data));CHECK(write(fd,data,sizeof(data))==sizeof(data));close(fd);
    CHECK(chmod("/tmp/not-elf",0755)==0); /* reach ELF validation after the execute-permission check */
    CHECK(spawn("/tmp/not-elf",args,-1)==-ENOEXEC);unlink("/tmp/not-elf");
    fd=open("/tmp/shebang.sh",O_WRITE);CHECK(fd>=0);
    const char *bang="#!/bin/sh\necho shebang-ok > /tmp/shebang.out\n";
    CHECK(write(fd,bang,strlen(bang))==(int)strlen(bang));close(fd);
    CHECK(chmod("/tmp/shebang.sh",0755)==0);
    char *bang_args[]={"shebang.sh",NULL};
    pid=spawn("/tmp/shebang.sh",bang_args,-1);CHECK(pid>0);
    status=-1;CHECK(waitpid(pid,&status,0)==pid && status==0);
    fd=open("/tmp/shebang.out",O_READ);CHECK(fd>=0);
    char mark[16]={0};CHECK(read(fd,mark,12)==11 && !strcmp(mark,"shebang-ok\n"));close(fd);
    unlink("/tmp/shebang.sh");unlink("/tmp/shebang.out");
    fd=open("/tmp/bang-bad",O_WRITE);CHECK(fd>=0);
    CHECK(write(fd,"#!\n",3)==3);close(fd);
    CHECK(chmod("/tmp/bang-bad",0755)==0);
    CHECK(spawn("/tmp/bang-bad",bang_args,-1)==-ENOEXEC);unlink("/tmp/bang-bad");
    fd=open("/tmp/bang-miss",O_WRITE);CHECK(fd>=0);
    const char *miss="#!/no/such/interp\n";
    CHECK(write(fd,miss,strlen(miss))==(int)strlen(miss));close(fd);
    CHECK(chmod("/tmp/bang-miss",0755)==0);
    CHECK(spawn("/tmp/bang-miss",bang_args,-1)==-ENOENT);unlink("/tmp/bang-miss");
    fd=open("/tmp/bang-loop",O_WRITE);CHECK(fd>=0);
    const char *loop="#!/tmp/bang-loop\n";
    CHECK(write(fd,loop,strlen(loop))==(int)strlen(loop));close(fd);
    CHECK(chmod("/tmp/bang-loop",0755)==0);
    CHECK(spawn("/tmp/bang-loop",bang_args,-1)==-ELOOP);unlink("/tmp/bang-loop");
    /* Repeated failures must not consume process slots or leak descriptor refs. */
    for(int i=0;i<40;i++) CHECK(spawn("/missing",args,-1)==-ENOENT);
    return 0;
}
int main(int argc,char **argv) {
    if(argc>1) return child(argc,argv);
    CHECK(exercise()==0);
    CHECK(account(2,"spawnuser","spawn123")==0);
    int pid=fork();CHECK(pid>=0);
    if(!pid) { CHECK(account(1,"spawnuser","spawn123")==0);return exercise(); }
    int status=-1;CHECK(waitpid(pid,&status,0)==pid && status==0);
    puts("SPAWN PASS");return 0;
}
