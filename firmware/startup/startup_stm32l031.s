/**
 * @file    startup_stm32l031.s
 * @brief   STM32L031F6P6 复位入口、向量表与默认中断处理。
 *
 * NOTE: 本文件仅依赖 CMSIS 头文件暴露的异常/中断号，不依赖 HAL 或任何第三方启动代码。
 *       向量表顺序必须与 stm32l031xx.h 中 IRQn_Type 的编号严格一致，
 *       改动 IRQn 后需同步本文件与 firmware/linker/stm32l031.ld 中的段布局。
 */

  .syntax unified
  .cpu cortex-m0plus
  .fpu softvfp
  .thumb

.global g_pfnVectors
.global Reset_Handler

/* 由链接脚本提供 */
  .word _sidata   /* .data 段在 FLASH 中的只读副本起始地址 */
  .word _sdata    /* .data 段在 RAM 中的运行起始地址         */
  .word _edata
  .word _sbss
  .word _ebss
  .word _estack

  .section .text.Reset_Handler
  .weak Reset_Handler
  .type Reset_Handler, %function
Reset_Handler:
  ldr r0, =_estack
  mov sp, r0                    /* 先建立栈指针，后续才允许调用 C 代码 */

  /* 将 .data 段初值从 FLASH 拷贝到 RAM */
  ldr r0, =_sdata
  ldr r1, =_edata
  ldr r2, =_sidata
  movs r3, #0
  b   copy_data_check

copy_data_loop:
  ldr  r4, [r2, r3]
  str  r4, [r0, r3]
  adds r3, r3, #4

copy_data_check:
  adds r4, r0, r3
  cmp  r4, r1
  bcc  copy_data_loop

  /* 将 .bss 段清零 */
  ldr r2, =_sbss
  ldr r4, =_ebss
  movs r3, #0
  b   zero_bss_check

zero_bss_loop:
  str  r3, [r2]
  adds r2, r2, #4

zero_bss_check:
  cmp  r2, r4
  bcc  zero_bss_loop

  bl  SystemInit                /* 由 system_stm32l0xx.c 提供 */
  bl  __libc_init_array
  bl  main

  /* main 正常返回属于固件缺陷：进入死循环等待看门狗复位 */
hang:
  b   hang
  .size Reset_Handler, .-Reset_Handler

/* 除 Reset 外全部中断默认指向 Default_Handler，具体服务程序在 C 侧用同名符号覆盖。 */
  .macro IRQ handler
  .weak \handler
  .thumb_set \handler, Default_Handler
  .endm

  .section .text.Default_Handler, "ax", %progbits
Default_Handler:
Infinite_Loop:
  b Infinite_Loop
  .size Default_Handler, .-Default_Handler

IRQ NMI_Handler
IRQ HardFault_Handler
IRQ SVC_Handler
IRQ PendSV_Handler
IRQ SysTick_Handler
IRQ WWDG_IRQHandler
IRQ PVD_IRQHandler
IRQ RTC_IRQHandler
IRQ FLASH_IRQHandler
IRQ RCC_IRQHandler
IRQ EXTI0_1_IRQHandler
IRQ EXTI2_3_IRQHandler
IRQ EXTI4_15_IRQHandler
IRQ DMA1_Channel1_IRQHandler
IRQ DMA1_Channel2_3_IRQHandler
IRQ DMA1_Channel4_5_6_7_IRQHandler
IRQ ADC1_COMP_IRQHandler
IRQ LPTIM1_IRQHandler
IRQ TIM2_IRQHandler
IRQ TIM21_IRQHandler
IRQ TIM22_IRQHandler
IRQ I2C1_IRQHandler
IRQ SPI1_IRQHandler
IRQ USART2_IRQHandler
IRQ LPUART1_IRQHandler

  .section .isr_vector, "a", %progbits
  .type g_pfnVectors, %object
g_pfnVectors:
  .word _estack
  .word Reset_Handler
  .word NMI_Handler
  .word HardFault_Handler
  .word 0
  .word 0
  .word 0
  .word 0
  .word 0
  .word 0
  .word 0
  .word SVC_Handler
  .word 0
  .word 0
  .word PendSV_Handler
  .word SysTick_Handler
  .word WWDG_IRQHandler
  .word PVD_IRQHandler
  .word RTC_IRQHandler
  .word FLASH_IRQHandler
  .word RCC_IRQHandler
  .word EXTI0_1_IRQHandler
  .word EXTI2_3_IRQHandler
  .word EXTI4_15_IRQHandler
  .word 0
  .word DMA1_Channel1_IRQHandler
  .word DMA1_Channel2_3_IRQHandler
  .word DMA1_Channel4_5_6_7_IRQHandler
  .word ADC1_COMP_IRQHandler
  .word LPTIM1_IRQHandler
  .word 0
  .word TIM2_IRQHandler
  .word 0
  .word 0
  .word 0
  .word 0
  .word TIM21_IRQHandler
  .word 0
  .word TIM22_IRQHandler
  .word I2C1_IRQHandler
  .word 0
  .word SPI1_IRQHandler
  .word 0
  .word 0
  .word USART2_IRQHandler
  .word LPUART1_IRQHandler
  .size g_pfnVectors, .-g_pfnVectors

/* ---------------------------------------------------------------------------
 * newlib 存根：本工程以 -nostartfiles 链接，newlib 的 crt0 不参与，
 * 但 libc 的 __libc_init_array 仍会引用 _init/_fini。
 * 这里提供空实现，避免为两个未使用的符号引入完整的 crt0 启动代码。
 * --------------------------------------------------------------------------- */
  .section .text._init, "ax", %progbits
  .weak _init
  .type _init, %function
_init:
  bx lr
  .size _init, .-_init

  .section .text._fini, "ax", %progbits
  .weak _fini
  .type _fini, %function
_fini:
  bx lr
  .size _fini, .-_fini

  .end
