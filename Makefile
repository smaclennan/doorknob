.PHONY: all clean

#### User settable

# This is needed by doorknob only
CONFIG_FILE ?= /etc/doorknob.conf

# Used by everybody
# MAIL_DIR must contian queue and tmp
MAILDIR ?= /var/spool/mail

# doorknob only
USE_CURL ?= 0

# doorknob non-curl only.
# Setting both to 0 supports smtp only. Username/passwords will be
# sent in the clear.  Only recommended for local smtp servers.
USE_OPENSSL ?= 0
USE_BEAR ?= 1

#### End of user settable

ifeq ($(USE_CURL),0)
ifeq ($(USE_BEAR),1)
CFLAGS += -DWANT_BEAR -DWANT_SSL -I BearSSL/inc
LIBS += BearSSL/build/libbearssl.a
else
ifeq ($(USE_OPENSSL),1)
CFLAGS += -DWANT_OPENSSL -DWANT_SSL
LIBS += -lssl -lcrypto
endif
endif
else
CFLAGS += -DWANT_CURL
LIBS += -lcurl
endif

CFLAGS += -DCONFIG_FILE=\"$(CONFIG_FILE)\" -DMAILDIR=\"$(MAILDIR)\"

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

all: doorknob sendmail mailq

doorknob: doorknob.c openssl.c base64.c bear.c
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+ $(LIBS)

sendmail: sendmail.c
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+

mailq: mailq.c
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
