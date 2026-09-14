#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "relayer.h"

#define BUFSIZE 	1024


struct rel_fsm_st
{
	int state;
	int sfd;
	int dfd;
	int len;
	int pos;
	int64_t count;
	char *errstr;
	char buf[BUFSIZE];
}


