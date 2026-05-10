build:
	gcc -std=gnu18 -Wall -Wextra transport.c -o transport

clean:
	rm *.o 

distclean:
	rm transport 
	rm *.o 
