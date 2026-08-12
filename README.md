basic set of tools
witch themed
edit terminal via source code and recompile

how to compile:
1. scry      - dependencies: X11, Xft, fontconfig and libutil (note: on macos/bsd, <util.h> is used instead of <pty.h> and -lutil is usually part of standard libc)
             - cc -O2 scry.c -o scry $(pkg-config --cflags --libs x11 xft fontconfig)
             - move to /usr/local/bin
2. hexedit   - dependencies: standard c library
             - cc -O2 hexedit.c -o hexedit
             - move into /usr/local/bin
3. alchemy   - dependencies: standard c library
             - cc -O2 alchemy.c -o alchemy
             - move into /usr/local/bin

last changes:
updated alchemy - added transmute all
