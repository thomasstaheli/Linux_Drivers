# Linux_Drivers

This is a simple driver that displays a chaser on the de1-soc LEDs.

This driver contains :

- It's a character driver
- Has a init, exit, probe and remove
- Multiples sysfs example
- Critic section handling : mutex, atomic variable and spin_lock
- Init and handling a kthread
- Use a kfifo to store command

Frameworks :
- platform
- character device

DTS :

