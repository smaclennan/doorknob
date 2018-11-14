# This is needed by doorknob only
CONFIG_FILE ?= /etc/doorknob.conf

# Used by doorknob and sendmail
# MAIL_DIR must contian queue and tmp
MAILDIR ?= /var/spool/mail

CFLAGS += -DCONFIG_FILE=\"$(CONFIG_FILE)\" -DMAILDIR=\"$(MAILDIR)\"
