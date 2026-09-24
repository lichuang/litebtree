# btreelite Makefile
#
# Phase 0 build: compile the extracted storage sources against
# src/btreeliteInt.h.  Sources are compiled unmodified from SQLite at this
# stage; unresolved-symbol analysis (make scan) drives the Phase 1-4
# seam work.
#
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2
CFLAGS  += -I. -I../include -Wall -Wno-unused-parameter -Wno-deprecated-declarations
# Debug build:  make CFLAGS="-O0 -g -DSQLITE_DEBUG"
# Release build keeps NDEBUG (asserts off).

HDRS    = btreeliteInt.h btreeInt.h pager.h wal.h pcache.h pcache1.h \
          os.h os_common.h os_setup.h mutex.h
LIB     = ../libbtreelite.a

# Order matters for the final link only; each file compiles independently.
CORE_SOURCES = \
    malloc.c \
    mem0.c \
    global.c \
    mem1.c \
    status.c \
    random.c \
    mutex.c \
    mutex_unix.c \
    bitvec.c \
    util.c \
    memjournal.c \
    os.c \
    os_unix.c \
    pcache.c \
    pcache1.c \
    pager.c \
    wal.c \
    btree.c

# The thin public-API layer is added in Phase 4; listed here so that
# "make" fails fast once it exists but is not yet compiled.
API_SOURCES =

SOURCES = $(CORE_SOURCES) $(API_SOURCES)
OBJECTS = $(SOURCES:.c=.o)

all: $(LIB)

# macOS: /usr/bin/libtool produces an archive the system linker accepts.
# GNU ar (homebrew binutils) writes a GNU symbol table that ld rejects.
$(LIB): $(OBJECTS)
	/usr/bin/libtool -static -o $@ $(OBJECTS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Compile-only pass: report every translation unit that fails to compile
# without trying to link.  The error list is the Phase 1-2 work inventory.
compile-only: $(OBJECTS)

.PHONY: $(OBJECTS)
$(OBJECTS): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Unresolved-symbol scan.  Compiles each object and greps the undefined
# references it makes.  Output feeds docs/phase.md phase planning.
scan: $(OBJECTS)
	@rm -rf scan.out && mkdir -p scan.out
	@for o in $(OBJECTS); do nm -u $$o > scan.out/$$o.undefined 2>/dev/null; done
	@cat scan.out/*.undefined | awk '{print $$2}' | sort | uniq -c \
	  | sort -rn > scan.out/undefined_symbols.txt
	@echo "== undefined symbols by count (see scan.out/) =="
	@cat scan.out/undefined_symbols.txt 2>/dev/null || cat scan.out/undefined.txt 2>/dev/null || head -50 scan.out/undefined_symbols.txt 2>/dev/null
	@awk '{print $$2}' scan.out/*.undefined | sort -u | grep -v '^_GLOBAL' \
	  | grep -v '^$' > scan.out/undefined_symbols.txt
	@wc -l scan.out/undefined_symbols.txt

# SQL-layer coupling census: count references to sqlite3-API symbols that
# only exist in the SQL layer.  These are the seam points to cut in
# Phase 1-2.
census:
	@grep -c 'sqlite3[A-Z]' $(SOURCES) || true
	@echo "---- per-file counts of SQL-layer symbols ----"
	@for f in $(SOURCES); do \
	  n=`grep -o 'sqlite3\(Malloc\|Realloc\|Free\|DbMalloc\|DbFree\|PageMalloc\|PageFree\|GlobalConfig\|MutexAlloc\|MutexInit\|MutexEnd\|StatusUp\|StatusDown\|StatusHighwater\|StatusValue\|BeginBenignMalloc\|EndBenignMalloc\|FaultSim\|Randomness\|Log\|StrAccum\|str_append\|str_appendf\|str_vappendf\|StrAccumInit\|StrAccumFinish\|Log\|InvokeBusyHandler\|ReportError\|MallocInit\|MallocEnd\|MutexEnd\|PcacheInitialize\|PcacheShutdown\|PcacheSetDefault\|OsInit\|IsNaN\|MemSetDefault\|Memdebug\|malloc64\|realloc64\|mprintf\|snprintf\|strcmp\|AbsInt32\|Get4byte\|Put4byte\|GetVarint\|PutVarint\|GetInt32\|Put2byte\|Get2byte\|Bitvec[A-Z]\|ConnectionBlocked\|TempInMemory\|WritableSchema\|Backup[A-Z]\|_exec\|Exec\|database_file_object\|create_filename\|filename_database\|uri_[a-z]*\|Strlen30\|IsMemdb\|MemSetArrayInt64\)' $$f | wc -l`; \
	  echo "$$f: $$n"; \
	done

clean:
	rm -f $(OBJECTS) $(LIB)
	rm -rf scan.out

.PHONY: all compile-only scan census clean