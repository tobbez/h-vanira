
.PHONY: clean

all: version.h h-vanira.c
	gcc -Wall -Wextra -ansi -pedantic -D_POSIX_C_SOURCE=1 -Os h-vanira.c \
	    -o h-vanira

clean:
	rm -f h-vanira

version.h: force
	@echo "#ifndef VERSION_H_" >  version.h
	@echo "#define VERSION_H_" >> version.h
	@echo -n "#define VERSION \"H-Vanira revision " >> version.h
	@svnversion -n >> version.h
	@echo -n " (" >> version.h
	@date -R | tr -d '\n' >> version.h
	@echo ") by InDigo176 and tobbez\"" >> version.h
	@echo "#endif" >> version.h

force: ;
