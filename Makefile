.PHONY = all install clean

#global directory defined
TOPDIR       = $(shell pwd)
SRCDIR	 	= $(TOPDIR)/src
LIBDIR      = $(TOPDIR)/lib
OBJECTDIR    = $(TOPDIR)/build
INCLUDEDIR   = $(TOPDIR)/include

#cross compile tools defined 
CROSS_COMPILE ?= 
AS      = $(CROSS_COMPILE)as
LD      = $(CROSS_COMPILE)ld
CC      = $(CROSS_COMPILE)gcc
CPP     = $(CC) -E
AR      = $(CROSS_COMPILE)ar
NM      = $(CROSS_COMPILE)nm
STRIP   = $(CROSS_COMPILE)strip
RANLIB 	= $(CROSS_COMPILE)ranlib

#local host tools defined
CP		:= cp
RM		:= rm
MKDIR	:= mkdir
SED		:= sed
FIND	:= find
MKDIR	:= mkdir
XARGS	:= xargs

#target name
TARGETMAIN  = bittorent 
TARGETLIBS 	= libbittorent.a
TARGETSLIBS = libbitrorent.so

#FILE' INFOMATION COLLECT
VPATH = $(shell ls -AxR $(SRCDIR)|grep ":"|grep -v "\.svn"|tr -d ':')
SOURCEDIRS = $(VPATH)

#search source file in the current dir
SOURCES = $(foreach subdir,$(SOURCEDIRS),$(wildcard $(subdir)/*.c))
SRCOBJS	= $(patsubst %.c,%.o,$(SOURCES))
BUILDOBJS = $(subst $(SRCDIR),$(OBJECTDIR),$(SRCOBJS))
DEPS = $(patsubst %.o,%.d,$(BUILDOBJS))

#external include file define
CFLAGS = -g -Wall -MMD $(foreach dir,$(INCLUDEDIR),-I$(dir))
ARFLAGS = rc

#special parameters for apps
CFLAGS += -D_FILE_OFFSET_BITS=64 

#c file compile parameters and linked libraries
CPPFLAGS = 
LDFLAGS	 = -lrt
XLDFLAGS = -Xlinker "-(" $(LDFLAGS) -Xlinker "-)"
LDLIBS   += -L $(LIBDIR) 

#defaut target:compile the currrent dir file and sub dir 
all:  $(TARGETMAIN)

#for .h header files dependence
-include $(DEPS)

$(TARGETMAIN) :$(BUILDOBJS)
	$(CC) $^ $(CPPFLAGS) $(CFLAGS) $(XLDFLAGS) -o $@ $(LDLIBS) 
	#$(STRIP)  --strip-unneeded $(TARGETMAIN)

$(TARGETLIBS) :$(BUILDOBJS)
	@$(AR) $(ARFLAGS) $@ $(BUILDOBJS)
	@$(RANLIB) $@

$(TARGETSLIBS) :$(BUILDOBJS)
	@$(CC) -shared $^ $(CPPFLAGS) $(CFLAGS) $(XLDFLAGS) -o $@ $(LDLIBS)

$(OBJECTDIR)%.o: $(SRCDIR)%.c
	@[ ! -d $(dir $@) ] & $(MKDIR) -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $(subst $(SRCDIR),$(OBJECTDIR),$@) -c $<

intall:

clean:
	@$(FIND) $(OBJECTDIR) -name "*.o" -o -name "*.d" | $(XARGS) $(RM) -f
	@$(RM) -f $(TARGETMAIN) $(TARGETLIBS) $(TARGETSLIBS)
