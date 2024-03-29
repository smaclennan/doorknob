.PHONY: all clean setup install devinstall

#### User settable

# # For QNX
# CC = ntox86_64-gcc
# LIBS += -lsocket -lslog2
# USE_BEAR = 0

# This is needed by doorknob only
CONFIGFILE ?= /etc/doorknob.conf

# Username for doorknob only. This should be a unique user with no
# special priviledges. Doorknob will drop privilege to this user.
DOORKNOBUSER ?= doorknob

# Used by everybody
# MAIL_DIR must contain queue and tmp
MAILDIR ?= /var/spool/doorknob

# Another un-privileged user for the mail queues
MAILUSER ?= mail

# Doorknob only. Setting to 0 supports smtp only. Username/passwords
# will be sent in the clear. Only recommended for local smtp servers.
USE_BEAR ?= 1

# Tweak this if you have BearSSL installed somewhere else.
ifeq ($(USE_BEAR),1)
BEAR_FILES = bear.o bear-tools.o
BEARDIR ?= ./BearSSL
CFLAGS += -DWANT_SSL -I $(BEARDIR)/inc
LIBS += $(BEARDIR)/build/libbearssl.a
endif

#### End of user settable

CONFFLAGS += -DCONFIGFILE=\"$(CONFIGFILE)\"
CONFFLAGS += -DMAILDIR=\"$(MAILDIR)\"
CONFFLAGS += -DDOORKNOBUSER=\"$(DOORKNOBUSER)\"
CONFFLAGS += -DMAILUSER=\"$(MAILUSER)\"

# C needs quotes, m4 does not
M4FLAGS = $(subst \",,$(CONFFLAGS))

# If you set D=1 on the command line then $(D:1=-g) returns -g,
# else it returns the default (-O2).
D = -O2
CFLAGS += -Wall $(D:1=-g)

CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=1

# If you set V=1 on the command line then you will get the actual
# commands displayed.
V	      = @
Q	      = $(V:1=)
QUIET_CC      = $(Q:@=@echo    '     CC       '$@;)
QUIET_RM      = $(Q:@=@echo    '     RM       '$@;)
QUIET_M4      = $(Q:@=@echo    '     M4       '$@;)

.c.o:
	$(QUIET_CC)$(CC) -o $@ -c $(CFLAGS) $(CONFFLAGS) $<

all: doorknob sendmail mailq

doorknob: doorknob.o utils.o $(BEAR_FILES)
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+ $(LIBS)

sendmail: sendmail.o
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+

mailq: mailq.o
	$(QUIET_CC)$(CC) $(CFLAGS) -o $@ $+

install: all setup
	install -d $(DESTDIR)/usr/bin $(DESTDIR)/usr/sbin
	install doorknob $(DESTDIR)/usr/sbin/doorknob
	install sendmail $(DESTDIR)/usr/sbin/sendmail
	rm -f $(DESTDIR)/usr/bin/sendmail
	ln -s /usr/sbin/sendmail $(DESTDIR)/usr/bin/sendmail
	install mailq $(DESTDIR)/usr/sbin/mailq
	sh ./setup.sh

setup:
	$(QUIET_M4)m4 $(M4FLAGS) setup-template > setup.sh

clean:
	$(QUIET_RM)rm -f doorknob sendmail mailq *.o
