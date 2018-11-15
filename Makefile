.PHONY: all clean

include config.mk

# If you set D=1 on the command line then $(D:1=-g) returns -g,
# else it returns the default (-O2).
D = -O2
CFLAGS += -Wall $(D:1=-g)

# If you set V=1 on the command line then you will get the actual
# commands displayed.
V	      = @
Q	      = $(V:1=)
QUIET_CC      = $(Q:@=@echo    '     CC       '$@;)
QUIET_RM      = $(Q:@=@echo    '     RM       '$@;)

.c.o:
	$(QUIET_CC)$(CC) -o $@ -c $(CFLAGS) $<

all: doorknob sendmail

doorknob: doorknob.c
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+ -lcurl

sendmail: sendmail.c
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+

install: all
	install -g mail doorknob $(DESTDIR)/usr/sbin
	install -g mail sendmail $(DESTDIR)/usr/sbin
	install -d -m 777 -g mail $(DESTDIR)/var/spool/mail/queue
	install -d -m 777 -g mail $(DESTDIR)/var/spool/mail/tmp

clean:
	$(QUIET_RM)rm -f doorknob sendmail *.o
