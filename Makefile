.PHONY: all clean

include config.mk

# If you set D=1 on the command line then $(D:1=-g) returns -g,
# else it returns the default (-O2).
D = -O2
CFLAGS += -Wall $(D:1=-g)

#CFLAGS += -DWANT_CURL
#LIBS += -lcurl

CFLAGS += -DWANT_OPENSSL
LIBS += -lssl -lcrypto

# If you set V=1 on the command line then you will get the actual
# commands displayed.
V	      = @
Q	      = $(V:1=)
QUIET_CC      = $(Q:@=@echo    '     CC       '$@;)
QUIET_RM      = $(Q:@=@echo    '     RM       '$@;)

.c.o:
	$(QUIET_CC)$(CC) -o $@ -c $(CFLAGS) $<

all: doorknob sendmail mailq mkauth

doorknob: doorknob.c openssl.c base64.c
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $< $(LIBS)

sendmail: sendmail.c
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+

mailq: mailq.c
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+

mkauth: mkauth.c
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+

install: all
	install -D -g mail doorknob $(DESTDIR)/usr/sbin/doorknob
	install -D -g mail sendmail $(DESTDIR)/usr/sbin/sendmail
	rm -f $(DESTDIR)/usr/bin/sendmail
	install -d $(DESTDIR)/usr/bin
	ln -s /usr/sbin/sendmail $(DESTDIR)/usr/bin/sendmail
	install -D -g mail mailq    $(DESTDIR)/usr/sbin/mailq
	install -d -m 777 -g mail $(DESTDIR)/var/spool/mail/queue
	install -d -m 777 -g mail $(DESTDIR)/var/spool/mail/tmp

clean:
	$(QUIET_RM)rm -f doorknob sendmail mailq *.o
