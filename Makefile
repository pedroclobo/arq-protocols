TARGETS = bin/file-sender bin/file-receiver

CC = gcc
CFLAGS = -Wall -O0 -g
LD = gcc
LDFLAGS =

default: $(TARGETS) bin/log-packets.so
bin/file-sender: bin/file-sender.o
bin/file-receiver: bin/file-receiver.o

$(TARGETS):
	$(LD) $(LDFLAGS) -o $@ $^

bin/%.o: src/%.c
	$(CC) -MT $@ -MMD -MP -MF $@.d $(CFLAGS) -c -o $@ $<

bin/%.so: src/%.c
	$(CC) -MT $@ -MMD -MP -MF $@.d -shared -fPIC $(CFLAGS) -o $@ $< -ldl

clean::
	(rm -f $(TARGETS) && rm -f bin/* && find . -name "*\.so" -o -name "*\.o" -o -name "*\.d" -o -name "*\.log" -o -name "*\.dat" -o -name "*\.eps" | xargs rm -f)

tests:: $(TARGETS) bin/log-packets.so
	(./tests/run.sh)

-include $(wildcard *.d)
