# Doorknob is a SMTP (mail) forwarder that is dumb as a doorknob.

Doorknob is meant to be used on machines where you want to forward all
of the mail to one remote mail account. Example use cases are laptops
where you want to forward all your mail to say gmail no matter who it
is really for. Or routers. Or work machines with overly strict email
policies :(

Doorknob has one simple config file and comes with a sample file that
is fairly well documented. All mail goes to the One True Server
(smtp-server) from the One True User (mail-from). Doorknob can even
overwrite the From: in the header for those overly strict work
policies.

If the recipient has an @ in the email (fred@gmail.com) then the
address goes through unchanged. However if the recipient is local
(root) then it is sent to the One True User. This means all your cron
output gets sent to the right place.

Hint: It is recommended that HOSTNAME be set with the fully qualified
      host name.


## How to install doorknob

Doorknob tries to be distro agnostic. This means you have to work a
bit harder to install it.

Check the user settable portion at the top of the Makefile and make
sure the settings are correct. The defaults should work. I recommend
using BearSSL. See README.BearSSL.

    make && sudo make install

A file called setup.sh should have been created. It should have all
the defaults from the Makefile so that it is consistent with the
executables. Take a good look at it since it must be run as root. When
you are happy with it run:

    sudo sh ./setup.sh

You now must edit /etc/doorknob.conf (or whatever you set CONFIGFILE
to). This file will most likely contain your username and password, so
keep the permissions tight.

You can test doorknob in the foreground using `sudo doorknob -d'. Send
a couple of mails and make sure there are no errors.

You can now use your favorite init script or supervisor system to
start doorknob. A sample rc.doorknob is included. The rc.doorknob was
tested with Slackware and also with Ubuntu. For Ubuntu copy
rc.doorknob to /etc/init.d/doorknob. This should work for
RedHat/Centos too, but I haven't tested it.


## Validation and Security

Doorknob is not very bright. A basic assumption is that you are
sending to a commercial grade mail server which will already be doing
a lot of (too much?) checking and filtering of the email. Or you are
on a local network where security is not really a consideration. So
doorknob lets them do that job and just does as little as
possible. Maybe doorknob isn't as dumb as he looks.

A note about SSL. Doorknob makes can be run with no certificates. This
is easier, but we do not guarantee we are talking to the right server.

You can specify the cert files with 'cert' entries in the config
file. You should have at least two files: one from the server and the
trusted root.

Currently doorknob supports BearSSL. To verify the certs you can use
the brssl program from BearSSL.

    brssl verify -CA <cert-root-file> <server-cert-file>


## How It Works

You can safely skip this section. You only need this if you want to
interface with doorknob directly.

I am going to assume the defaults here.

Doorknob watches the /var/spool/doorknob/queue directory for new files and
then sends them. You should create the file somewhere else
(/var/spool/doorknob/tmp is used by sendmail) and then move it into the
queue directory.

The file name doesn't really matter as long as it isn't a hidden
file. The recommended format should guarantee no collisions (except
over NFS):

    <seconds>.<microseconds>.<pid>

The format of the file is:

    <raw to address ...>
    <empty line>
    <stuff>

A raw address is `fred@gmail.com`, not `<fred@gmail.com>` and certainly
not `Fred Flintstone <fred@gmail.com>`.

Seriously, unless rewriting the From: line, doorknob doesn't care what
comes after the empty line.

A note about the raw to address. Doorknob assumes that the real SMTP
server does not know who root, lisa, or fred are on your
machine. To addresses that have an @ in them are sent through
unmolested. But the first occurrence of an address without an @ is
replaced with the mail-from address. All other occurrences are
silently dropped. The To: field in the header should still contain the
correct addresses.
