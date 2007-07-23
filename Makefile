CFLAGS=-g

all: rater librater.a raterclient

rater: rater.o bstrlib.o
	gcc -o rater -g rater.o bstrlib.o /usr/lib/libut.a -lsqlite3 -lpthread -lconfig

librater.a: librater.o bstrlib.o
	gcc -o librater.a -g librater.o bstrlib.o

raterclient: raterclient.o bstrlib.o
	gcc -o raterclient -g raterclient.o bstrlib.o /usr/lib/libut.a librater.a -lsqlite3 -lpthread -lconfig

clean:
	rm *.o rater

pretty:
	indent -bap -bad -bbb -bl -bls -ci8 -bli0 rater.c
