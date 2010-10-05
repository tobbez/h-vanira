
.PHONY: clean

all: version.h h-vanira.c
	gcc -Wall -Wextra -ansi -pedantic -D_POSIX_C_SOURCE=1 -Os h-vanira.c \
	    -Ilibucfg libucfg/ucfg.c -o h-vanira

clean:
	rm -f h-vanira version.h

version.h: force
	@echo "#ifndef VERSION_H_" >  version.h
	@echo "#define VERSION_H_" >> version.h
	@echo -n "#define VERSION \"" >> version.h
	@git show -s --pretty=format:"H-Vanira commit %h (%ai) by InDigo176 and tobbez\"%n" >> version.h
	@echo "#endif" >> version.h

force: ;
