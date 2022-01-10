================ 
QEMU STIX Readme
================ 

**STIX FSW Adjustments** 

``main.c``: Move ``TMBuffer_init();`` before ``ASW_VERIFY(MemCommon_init(), FSW_FATAL_ERROR_MEMCOMMON_INIT);``

**Preparing QEMU**  

1. ``~/qemu-stix/build/qemu-system-sparc -s -S -no-reboot -nographic -M leon3_generic -m 64M -kernel /mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf`` 

*With tracing on*  

1. ``~/qemu-stix/build/qemu-system-sparc -s -S -no-reboot -nographic -M leon3_generic -m 64M -kernel /mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf  --trace "*irqmp_*"``  

**Using GDB** 

1. ``~/rtems/4.10/bin/sparc-rtems4.10-gdb /mnt/c/Users/laszl/Development/stix_asw_src/ASW/ASW/bin/ASW_em.elf``

2. ``target remote :1234`` 

3. ``break [filename]:[line]``

4. ``c``
