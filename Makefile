spawnshell: spawnshell.c
	gcc -g -Wall -Werror -o spawnshell spawnshell.c

clean:
	rm -f spawnshell