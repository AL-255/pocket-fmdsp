/* STM32F103VE(T6): 512 KB Flash @ 0x08000000, 64 KB SRAM @ 0x20000000.
   Also valid on the QEMU 'netduino2' machine (STM32F205, same base addresses,
   larger memories — our sizes are a strict subset). */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* top of stack */
_Min_Heap_Size  = 0x0;                  /* freestanding: no malloc */
_Min_Stack_Size = 0x800;                /* MSP: ISRs only (tasks use their own PSP) */

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /* The entire per-sample render path runs from 0-wait SRAM (functions tagged
     PFM_HOT): FM/SSG/drum/PCM mixers + timer slicing. Only cold code (UI, PMD
     sequencer, register writes, init) stays in the under-spec flash. */
  .ramfunc :
  {
    . = ALIGN(4);
    _sramfunc = .;
    *(.ramfunc)
    *(.ramfunc*)
    . = ALIGN(4);
    _eramfunc = .;
  } >RAM AT> FLASH
  _siramfunc = LOADADDR(.ramfunc);

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
