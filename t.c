#include <stdio.h>
#include <limits.h>

int main(int argc, char **argv){
	int file_pos = 0;
	int alignment = 512;
	int size;
	
	size = file_pos - (file_pos & ~(alignment-1));



	printf("~(alignment-1): %d  file_pos & alignment: %d  size: %d\n mod: %d \n"
		,~(alignment-1) ,file_pos & ~(alignment-1), size, file_pos%alignment);


	printf("%lu \n",sizeof("-9223372036854775808"));
}
