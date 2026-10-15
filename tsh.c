#define _GUN_SOURCE
/* 
 * tsh - A tiny shell program with job control
 * 
 * <Put your name and login ID here>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* 识别不到*/
#define FG 1    /* 前台作业*/
#define BG 2    /* 后台作业*/
#define ST 3    /*停止*/

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* 进程的pid*/
    int jid;                /* 进程的进程组内的id */
    int state;              /* 进程的状态 */
    char cmdline[MAXLINE];  /* 指令数组 */
};
struct job_t jobs[MAXJOBS]; /* 进程列表 */
/* End global variables */


/* 函数的声明的地方*/

/*包装函数*/
pid_t Fork(void);
void Sigprocmask(int how, sigset_t *set,sigset_t *oldset);
void Sigemptyset(sigset_t *set);
void Sigfillset(sigset_t *set);
void Sigaddset(sigset_t *set,int signum);
void Kill(pid_t pid,int sig);
void Execve(char *path,char **argv,char **env);

/* 我需要实现的函数*/
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* 提供的帮助 */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * 工作台的主程序
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }


    /* 初始化要跳转的信号 */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* 停止或终止一个子进程 */

    /* 强制停止工作台 */
    Signal(SIGQUIT, sigquit_handler); 

    /* 初始化工作列表*/
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* 读取命令*/
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* 处理命令*/
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/*包装函数区域*/
pid_t Fork(void){
    pid_t pid;
    if((pid = fork())<0){
    unix_error("Fork error");
    }
    return pid;
}
void Sigprocmask(int how, sigset_t *set,sigset_t *oldset){
    if(sigprocmask(how,set,oldset)<0)
    unix_error("Sigprocmask error");
    return;
}
void Sigemptyset(sigset_t *set){
    if(sigemptyset(set)<0)
    unix_error("Sigemptyset error");
    return;
}
void Sigfillset(sigset_t *set){
    if(sigfillset(set)<0)
    unix_error("Sigfillset error");
    return;
}
void Sigaddset(sigset_t *set,int signum){
    if(sigaddset(set,signum)<0)
    unix_error("Sigaddset error");
}
void Kill(pid_t pid,int sig){
    if(kill(pid,sig)<0){
        if(errno == ESRCH)
        return;//避免unix_error直接将程序结束
        unix_error("Kill error");
    }
    
    return;
}
void Execve(char *path,char **argv,char **env){
    if(execve(path,argv,env)<0)
    printf("%s: Command not found\n",path);//表明既不是指令，也不是路径，输出找不到对应指令
    exit(0);
}
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
    char *argv[MAXARGS];
    char buf[MAXLINE];
    int bg,state;//处理区别前后台作业
    int order = 0;//判断指令是不是内置的指令

    sigset_t oldset,mask;
    pid_t pid;
    strcpy(buf,cmdline);
    bg = parseline(buf,argv);
    state = bg? BG:FG;//处理前后台

    if(argv[0] == NULL)
    return;//判断命令是否为空
    Sigemptyset(&mask);
    Sigaddset(&mask,SIGCHLD);
    order = builtin_cmd(argv);
    if(!order){
        Sigprocmask(SIG_BLOCK,&mask,&oldset);//信号屏蔽，保证addjob在deletejob之前
        if((pid = Fork())==0){
            setpgid(0,0);//生成单一的进程组，方便信号处理
            Sigprocmask(SIG_SETMASK,&oldset,NULL);
            Execve(argv[0],argv,environ);//跳转程序
            exit(0);
        }
            addjob(jobs,pid,state,cmdline);//添加到工作表
            Sigprocmask(SIG_SETMASK,&oldset,NULL);
    if(state == FG){
        waitfg(pid);//等待前台作业完成
    }
    if(state == BG){
        struct job_t *temp;
        temp = getjobpid(jobs,pid);
        printf("[%d] (%d) %s",temp->jid,temp->pid,temp->cmdline);
    }
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* 得到指令的复制*/
    char *buf = array;          /* 作为指针指向读取到的指令*/
    char *delim;                /* points to first space delimiter */
    int argc;                   /*指令的分段数*/
    int bg;                     /* 是不是后台工作*/

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* 替代换行符为空格 */
    while (*buf && (*buf == ' ')) /* 跳过空格 */
	buf++;

    /* 建立指令表*/
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{   
    if(!strcmp(argv[0],"quit")){
        exit(0);
    }//quit指令
    if(!strcmp(argv[0],"fg")||!strcmp(argv[0],"bg")){
        do_bgfg(argv);
        return 1;
    }//进入前后台切换的函数
    if(!strcmp(argv[0],"jobs")){
        listjobs(jobs);
        return 1;
    }//列出进程表
    return 0;     //不是指令时返回0，是一个需运行的程序的路径
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{   
    struct job_t *job_num;
    pid_t pid;
    int jid;
    int flag = -1;
    if(!strcmp(argv[0],"fg"))
    flag = FG;
    if(!strcmp(argv[0],"bg"))
    flag = BG;
    if(argv[1]==NULL){
        printf("%s command requires PID or %%jobid argument\n",argv[0]);
        return;
    }
    /*处理对象是jid*/
    int num;
    if(argv[1][0]=='%'){
        int i=1;
        jid = 0;
        while(argv[1][i]!='\0'){
            if(!(argv[1][i]<='9'&&argv[1][i]>='0')){
                if(flag == FG)
                printf("fg: argument must be a PID or %%jobid\n");
                if(flag == BG)
                printf("bg: argument must be a PID or %%jobid\n");
                return;
            }
            num = argv[1][i] - '0';
            jid = jid * 10;
            jid = jid + num;
            i++;
        }
        job_num = getjobjid(jobs,jid);
        if(job_num == NULL){
            printf("%%%d: No such job\n",jid);
            return;
        }
    }//处理对像为pid
    else {
        int j = 0;
    while(argv[1][j]!='\0'){
        if(!(argv[1][j]<='9'&&argv[1][j]>='0')){
            if(flag == FG)
            printf("fg: argument must be a PID or %%jobid\n");
            if(flag == BG)
            printf("bg: argument must be a PID or %%jobid\n");
            return;
        }
        j++;
    }
        int pid_enter;
        pid_enter = atoi(argv[1]);
        job_num = getjobpid(jobs,pid_enter);
        if(job_num == NULL){
            printf("(%d): No such process\n",pid_enter);
            return;
        }
    }
    pid = job_num->pid;
    /*得到对应程序的pid*/
    if(flag == FG){
        job_num->state = FG; //改变状态标记为前台运行
        Kill(-pid,SIGCONT);
        waitfg(pid);
    }
    if(flag == BG){
        job_num->state = BG;//改变标记状态为后台运行
        printf("[%d] (%d) %s",job_num->jid,job_num->pid,job_num->cmdline);
        Kill(-pid,SIGCONT);
        return;
    }
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    while(pid == fgpid(jobs)){
        sleep(1);
    }
    //最开始在这里用了掩码，只放了sigchld，还用了waitpid，造成了和handler彼此抢进程以及ctrlc，z失效，但实际上这里不用掩码，只负责等待，利用job的哪些函数，判断目标进程状态就行，结束进程交给handler
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    int olderrno = errno;
    pid_t pid;
    int staus,si;
    struct job_t *job_temp;
    while((pid = waitpid(-1,&staus,WNOHANG|WUNTRACED))>0){
        if(WIFSTOPPED(staus)){
            job_temp = getjobpid(jobs,pid);
            job_temp->state = ST;
            si = WSTOPSIG(staus);
            printf("Job [%d] (%d) stopped by signal %d\n",job_temp->jid,job_temp->pid,si);
        }
        else if(WIFEXITED(staus)){
            deletejob(jobs,pid);
        }
        else if(WIFSIGNALED(staus)){
            si = WTERMSIG(staus);
            job_temp = getjobpid(jobs,pid);
            printf("Job [%d] (%d) terminated by signal %d\n",job_temp->jid,job_temp->pid,si);
            deletejob(jobs,pid);
        }
    }
    errno = olderrno;
    return;
}/*删除进程，并复原errno*/

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    int olderrno = errno;
    errno = olderrno;
    pid_t pid;
    pid = fgpid(jobs);
    if(pid>0)
    Kill(-pid,SIGINT);
    return;
}/*复原errno的同时发送信号*/

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    int olderrno = errno;
    pid_t pid;
    pid = fgpid(jobs);
    if(pid>0)
    Kill(-pid,SIGTSTP);
    errno = olderrno;
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* 清空整个工作表 */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* 初始化工作表 */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* 返回最大的工作进程的id*/
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* 增加进程工作表，成功返回1，失败返回0 */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* 删除pid的工作进程*/
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* 返回当前前台工作的工作进程的pid，返回零则没有进程*/
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* 通过pid找工作进程*/
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* 通过job里的id找对应工作进程 */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* 返回pid对应的jid*/
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* 打印工作列表*/
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



