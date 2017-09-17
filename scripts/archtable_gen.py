#!/usr/bin/env python3
import sys
from os import path, chdir


def usage():
	print("{}: the input should be ".format(sys.argv[0]) +
			"vu_syscalls.conf")

if len(sys.argv) < 2 or not path.isfile(sys.argv[1]):
	usage()
	sys.exit(1)

# Parse vu_syscalls.conf
vu_syscalls = dict()
vu_sysargs = dict()
vvu_sysargs = dict()
with open(sys.argv[1]) as f:
	for line in f:
		line = line.strip()
		if not line.startswith('#'):
			um_syscall_list = line.split(':')
			if len(um_syscall_list) > 1:
				um_syscall = um_syscall_list[0].split(',')
				value = um_syscall[0].strip()
				value = value.split('/')[0].strip()
				if value.startswith('-'):
					value = value[1:].strip()
					for s in um_syscall:
						sys_arg = s.split('/')
						if len(sys_arg) > 1:
							vvu_sysargs[sys_arg[0].strip()] = sys_arg[1].strip()
				else:
					for s in um_syscall:
						vu_syscalls[s.split('/')[0].strip()] = value
					for s in um_syscall:
						sys_arg = s.split('/')
						if len(sys_arg) > 1:
							vu_sysargs[sys_arg[0].strip()] = sys_arg[1].strip()
				
# Parse and output
footer = "};\n"
print ('''#include <syscall_defs.h>
#include <arch_table.h>

/*This table has been autogenerated! */
uint16_t vu_arch_table[SYSCALL_NR_OVERESTIMATION] = {''')
for syscall in sorted(vu_syscalls):
	print("\t#ifdef __NR_{}".format(syscall))
	print("\t\t[__NR_{}] = __VU_{},".format(syscall, vu_syscalls[syscall]))
	print("\t#endif")
print(footer);

print('uint8_t vu_arch_args[SYSCALL_NR_OVERESTIMATION] = {')
for syscall in sorted(vu_sysargs):
	print("\t#ifdef __NR_{}".format(syscall))
	print("\t\t[__NR_{}] = 0{},".format(syscall, vu_sysargs[syscall]))
	print("\t#endif")
print(footer);

print('''uint8_t vvu_arch_args[VVU_NR_SYSCALLS] = {
\t[0] = 0,''')
for syscall in sorted(vvu_sysargs):
	print("\t\t[__NR_{}] = 0{},".format(syscall, vvu_sysargs[syscall]))
print(footer);

