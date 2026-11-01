all: launch fib sum

launch:
	gcc -m32 -fPIE -pie -o launch launch.c loader.c

fib:
	gcc -m32 -no-pie -nostdlib -o fib fib.c

sum:
	gcc -m32 -no-pie -nostdlib -o sum sum.c

clean:
	-@rm -f launch fib sum
