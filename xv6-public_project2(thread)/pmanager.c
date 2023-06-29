// Process manager.

#include "types.h"
#include "user.h"
#include "fcntl.h"

// command를 저장하는 구조체 
struct cmd {
  char command[10];
  char arg1[50];
  char arg2[10];
};

int fork1(void);
void panic(char*);
char* gets1(char*, int);
void parse_runCmd(char*);
int getcmd(char*, int);


int
main()
{
  static char buf[100];
  // exit 명령이 나올 때까지 반복
  while(getcmd(buf, sizeof(buf)) >= 0){
    // 명령을 수행할 자식 프로세스 생성
    if(fork1() == 0){
      // 명령 분석 후 실행
      parse_runCmd(buf);
    }
    // 부모 프로세스는 자식 프로세스가 종료되길 기다림
    wait();
    // exit 명령이 나오면 pmanager 종료
    if(strcmp(buf, "exit") == 0) break;
  }
  exit();
}

void
panic(char *s)
{
  printf(2, "%s\n", s);
  exit();
}

int
fork1(void)
{
  int pid;
  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

// 기본 함수 get에서 \n을 제거하기 위한 함수
char*
gets1(char *buf, int max)
{
  int i, cc;
  char c;

  for(i=0; i+1 < max; ){
    cc = read(0, &c, 1);
    if(cc < 1)
      break;
    buf[i++] = c;
    if(c == '\n' || c == '\r')
      break;
  }
  buf[i-1] = '\0';
  return buf;
}

// 명령 해석하고 실행하는 함수
void
parse_runCmd(char *s)
{
  int i = 0;
  int idx1 = 0;
  int idx2 = 0;
  int cnt = 0;
  int result;
  // 메모리 할당 후 초기화
  struct cmd *cmd = malloc(sizeof(struct cmd));
  memset(cmd, 0, sizeof(struct cmd));

  // 문자열 끝 도달할 때까지 읽기
  while(s[i] != '\0'){
    // 공백에 도달하면 다음 버퍼로 이동
    if(s[i] == ' '){
      cnt++;
    }
    // 명령어 버퍼
    else{
      if(cnt == 0){
        cmd->command[i] = s[i];
      }
      // 인자 1 버퍼
      else if(cnt == 1){
        cmd->arg1[idx1] = s[i];
        idx1++; 
      }
      // 인자 2 버퍼
      else if(cnt == 2){
        cmd->arg2[idx2] = s[i];
        idx2++;
      }
    }
    i++;
  }
  // 명령어 구분해서 실행
  if(strcmp(cmd->command, "list") == 0){
    processInfo();
  }
  else if(strcmp(cmd->command, "kill") == 0){
    result = kill(atoi(cmd->arg1));
    if(result == 0){
      printf(1, "success!\n");
    }
    else{
      printf(1, "fail!\n");
    }
  }
  else if(strcmp(cmd->command, "execute") == 0){
    // execute 명령어를 실행할 새로운 프로세스 생성
    if(fork1() == 0){
      char *args[2] = {cmd->arg1, 0};
      result = exec2(cmd->arg1, args ,atoi(cmd->arg2));
      
      if(result == -1){
        printf(1, "error!\n");
        exit();
      }
    }
  }
  else if(strcmp(cmd->command, "memlim") == 0){
    result = setmemorylimit(atoi(cmd->arg1), atoi(cmd->arg2));
    if(result == 0){
      printf(1, "success!\n");
      }
    else{
      printf(1, "fail!\n");
    }
  }
  else if(strcmp(cmd->command, "exit") == 0){
    printf(1, "exit\n");
  }
  
  // 잘못된 명령어 입력 시 
  else{
    printf(1, "command error!\n");
  }
  free(cmd);
  exit();
}

// 사용자에게 명령어를 입력 받는 함수
int
getcmd(char *buf, int nbuf)
{
  printf(2, "$ ");
  memset(buf, 0, nbuf);
  gets1(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;
  return 0;
}