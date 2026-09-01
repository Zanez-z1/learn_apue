#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N  19	

int main(void)
{
	FILE *fp,*tmp;
	char *str = "hello,world";
	char store[1024];
	size_t ret;
	tmp = tmpfile();

	
	fwrite(str,1,strlen(str),tmp);
	
	rewind(tmp);	
	
	ret = fread(store,1,strlen(str),tmp);
	store[ret] = '\0';
	
	printf("%s\n",store);
	printf("read %zu bytes\n",ret);
	exit(0);
}
