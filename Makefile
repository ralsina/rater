CFLAGS=-g

all: rater

rater: rater.o bstrlib.o
	gcc -o rater -g rater.o bstrlib.o /usr/lib/libut.a -lsqlite3 -lpthread -lconfig

clean:
	rm *.o rater

pretty:
	indent -bap -bad -bbb -bl -bls -ci8 -bli0 *.c
