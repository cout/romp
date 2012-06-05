require 'mkmf'
create_makefile("romp_helper")
system("echo CFLAGS+=-g -Wall -O3 >> Makefile")
