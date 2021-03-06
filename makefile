LDLIBS = -lsodium

SRCDIR = src
VPATH = $(SRCDIR)

IDIR = $(SRCDIR)
CFLAGS=-g -I $(IDIR) -Werror -Wall -Wextra -std=c99 -pedantic

ODIR=build

DEPS = $(IDIR)/shs1.h

OBJ = $(ODIR)/shs1.o $(ODIR)/test.o

$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

test: $(ODIR)/shs1.o $(ODIR)/test.o
	cc $(LDLIBS) -o $@ $^ $(CFLAGS) $(LIBS)

testClient: $(ODIR)/shs1.o $(ODIR)/test-client.o
	cc $(LDLIBS) -o $@ $^ $(CFLAGS) $(LIBS)

testServer: $(ODIR)/shs1.o $(ODIR)/test-server.o
	cc $(LDLIBS) -o $@ $^ $(CFLAGS) $(LIBS)

example: src/example.c src/shs1.c
	cc $(LDLIBS) -o example src/example.c src/shs1.c

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o test example test-client test-server
