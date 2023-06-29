// 기본적으로 추가해야 하는 헤더파일, 순서도 지켜줘야 함.
#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]){

  int ret_val;
  ret_val = myfunction(argv[1]);
  printf(1, "Return value : 0x%x\n", ret_val);
  exit();
}
