/* Copyright 2014 JP Norair
  *
  * Licensed under the OpenTag License, Version 1.0 (the "License");
  * you may not use this file except in compliance with the License.
  * You may obtain a copy of the License at
  *
  * http://www.indigresso.com/wiki/doku.php?id=opentag:license_1_0
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
  */
/**
  * @file       /otplatform/stm32l1xx/core_main.c
  * @author     JP Norair
  * @version    R100
  * @date       27 Aug 2014
  * @brief      Main, core platform implementation for STM32L
  * @ingroup    Platform
  *
  ******************************************************************************
  */

#include <otstd.h>
#include <otplatform.h>

// OT modules that need initialization
#include <otlib/auth.h>
#include <otlib/buffers.h>
#include <otsys/sysclock.h>
#include <otsys/veelite.h>
#include <otsys/veelite_core.h>

#include <otsys/mpipe.h>
#include <otsys/syskern.h>

#include <m2/radio.h>
//#include <m2/session.h>



//API wrappers
void otapi_poweron()    { platform_poweron(); }
void otapi_poweroff()   { platform_poweroff(); }
void otapi_init()       { platform_init_OT(); }
void otapi_exec()       { platform_ot_run(); }
void otapi_preempt()    { platform_ot_preempt(); }
void otapi_pause()      { platform_ot_pause(); }




///@note for some reason, the Constant "RCC_CR_HSION" from the device header
/// is possessed by Satan.  Compiler flails and flails over it.
#define _RCC_CR_HSI     1



/** Context Caching Parameters <BR>
  * =======================================================================<BR>
  * STM32L and all other Cortex M devices have a hardware mechanism to assist
  * the caching of multiple contexts and levels of interrupt preemption.
  *
  * GULP kernels should use __CM0_NVIC_GROUPS == 1, which will create a single,
  * global interrupt context.  Tasks must manage their own contexts.
  *
  * HICCULP kernels can use __CM0_NVIC_GROUPS > 1, but each extra group (each
  * extra context) will require the allocation of extra stack.
  *
  * Usually, these configuration parameters are set in the compiler or in the
  * build_config.h file.  If they are not set, defaults are used.
  */



/** Clocking Constants <BR>
  * ========================================================================<BR>
  */

#define POWER_1V8   0x0800
#define POWER_1V5   0x1000
#define POWER_1V2   0x1800


///@todo build a board-defaults file with all of these
#ifndef BOARD_FEATURE_HFXTAL
#   define BOARD_FEATURE_HFXTAL     DISABLED
#endif
#ifndef BOARD_FEATURE_HFBYPASS
#   define BOARD_FEATURE_HFBYPASS   DISABLED
#endif
#ifndef MCU_CONFIG_USB
#   define MCU_CONFIG_USB           DISABLED
#endif
#ifndef BOARD_FEATURE_HFCRS
#   define BOARD_FEATURE_HFCRS      DISABLED
#endif
#ifndef BOARD_FEATURE_USBPLL
#   define BOARD_FEATURE_USBPLL     DISABLED
#endif
#ifndef PLATFORM_PLLCLOCK_OUT
#   define PLATFORM_PLLCLOCK_OUT    96000000
#endif


// error checks
#if (BOARD_FEATURE(HFXTAL) && BOARD_FEATURE(HFBYPASS))
#   error "BOARD_FEATURE_HFXTAL and BOARD_FEATURE_HFBYPASS cannot be both ENABLED."
#endif
#if (MCU_CONFIG(USB) && (BOARD_FEATURE(USBPLL) != ENABLED) && (BOARD_FEATURE(HFCRS) != ENABLED))
#   error "To use built-in USB, you must ENABLE the PLL or CRS"
#endif
#if (MCU_CONFIG(USB) && BOARD_FEATURE(USBPLL) && (PLATFORM_PLLCLOCK_OUT != 96000000))
#   error "STM32L0 requires PLL output to be 96 MHz when using it for internal USB."
#endif

// If GPTIM/KTIM uses RTC as a time source, we need to keep it open,
// and unfortunately this access tends to be in the same place as clocking.
// Also, the interval-timer uses WUTE, so basically DBP is always set
#if 1 || (OT_GPTIM_ID == 'R')
#   define _RTC_PROTECTION  PWR_CR_DBP
#else
#   define _RTC_PROTECTION  0
#endif

// Flankspeed uses PLL, which requires 1.8V
#if (BOARD_FEATURE(FLANKSPEED) || BOARD_FEATURE(PLL))
#   define _USE_PLL
#   define _PLL_SRC             ((BOARD_FEATURE(FLANKXTAL) == ENABLED) << 16)
#   define _FLANKSPEED_VOLTAGE  POWER_1V8
#   if (BOARD_PARAM(PLLmult) == 3)
#       define _PLL_MULT    (0<<18)
#   elif (BOARD_PARAM(PLLmult) == 4)
#       define _PLL_MULT    (1<<18)
#   elif (BOARD_PARAM(PLLmult) == 6)
#       define _PLL_MULT    (2<<18)
#   elif (BOARD_PARAM(PLLmult) == 8)
#       define _PLL_MULT    (3<<18)
#   elif (BOARD_PARAM(PLLmult) == 12)
#       define _PLL_MULT    (4<<18)
#   elif (BOARD_PARAM(PLLmult) == 16)
#       define _PLL_MULT    (5<<18)
#   elif (BOARD_PARAM(PLLmult) == 24)
#       define _PLL_MULT    (6<<18)
#   elif (BOARD_PARAM(PLLmult) == 32)
#       define _PLL_MULT    (7<<18)
#   elif (BOARD_PARAM(PLLmult) == 48)
#       define _PLL_MULT    (8<<18)
#   else
#       error "PLL Multiplier from BOARD_PARAM_PLLmult is out of range"
#   endif
#   if (BOARD_PARAM(PLLdiv) == 2)
#       define _PLL_DIV    (1<<22)
#   elif (BOARD_PARAM(PLLdiv) == 3)
#       define _PLL_DIV    (2<<22)
#   elif (BOARD_PARAM(PLLdiv) == 4)
#       define _PLL_DIV    (3<<22)
#   else
#       error "PLL Divider from BOARD_PARAM_PLLdiv is out of range"
#   endif
#   if (PLATFORM_PLLCLOCK_HZ > 32000000)
#       error "PLL Flank Speed Clock must be less than 32 MHz"
#   elif (PLATFORM_PLLCLOCK_HZ > 16000000)
#       define _FLANKSPEED_FLASHWAIT ENABLED
#   else
#       define _FLANKSPEED_FLASHWAIT DISABLED
#   endif
#   if BOARD_FEATURE(FLANKXTAL)
#       define _FLANKOSC_RDYFLAG        RCC_CR_HSERDY
#       define _FLANKOSC_CLOCKBIT       3
#       if BOARD_FEATURE(HFBYPASS)
#           define _FLANKOSC_ONBIT      (RCC_CR_HSEON | RCC_CR_HSEBYP)
#           define _FLANKOSC_TIMEOUT    1000
#       else
#           define _FLANKOSC_ONBIT      RCC_CR_HSEON
#           define _FLANKOSC_TIMEOUT    3000 //HSE_STARTUP_TIMEOUT
#       endif
#   else
#       define _FLANKOSC_ONBIT      _RCC_CR_HSI
#       define _FLANKOSC_RDYFLAG    RCC_CR_HSIRDY
#       define _FLANKOSC_CLOCKBIT   (3 | RCC_CFGR_STOPWUCK)
#       define _FLANKOSC_TIMEOUT    300 //HSI_STARTUP_TIMEOUT
#   endif
#else
#   define _PLL_SRC     0
#   define _PLL_MULT    0
#   define _PLL_DIV     0

#endif

// Fullspeed uses HSE or HSI without PLL
///@todo See if USB requires 1V8 for sure, and make it dynamic only when active.
#if BOARD_FEATURE(FULLSPEED)
#   if (PLATFORM_HSCLOCK_HZ > 32000000)
#       error "High Speed Clock must be less than 32 MHz"     
#   elif (PLATFORM_HSCLOCK_HZ > 16000000)
#       define _FULLSPEED_VOLTAGE   POWER_1V8
#       define _FULLSPEED_FLASHWAIT ENABLED
#   elif (MCU_CONFIG(USB) && (PLATFORM_HSCLOCK_HZ <= 16000000))
#       define _FULLSPEED_VOLTAGE   POWER_1V8
#       define _FULLSPEED_FLASHWAIT DISABLED
#   elif (PLATFORM_HSCLOCK_HZ > 8000000)
#       define _FULLSPEED_VOLTAGE   POWER_1V5
#       define _FULLSPEED_FLASHWAIT ENABLED
#   elif (PLATFORM_HSCLOCK_HZ > 4000000)
#       define _FULLSPEED_VOLTAGE   POWER_1V5
#       define _FULLSPEED_FLASHWAIT DISABLED
#   elif (PLATFORM_HSCLOCK_HZ > 2000000)
#       define _FULLSPEED_VOLTAGE   POWER_1V2
#       define _FULLSPEED_FLASHWAIT ENABLED
#   else
#       define _FULLSPEED_VOLTAGE   POWER_1V2
#       define _FULLSPEED_FLASHWAIT DISABLED
#   endif
#   if (BOARD_FEATURE(FULLXTAL))
#       define _FULLOSC_RDYFLAG         RCC_CR_HSERDY
#       define _FULLOSC_CLOCKBIT        2
#       if BOARD_FEATURE(HFBYPASS)
#           define _FULLOSC_ONBIT       (RCC_CR_HSEON | RCC_CR_HSEBYP)
#           define _FULLOSC_TIMEOUT     1000
#       else
#           define _FULLOSC_ONBIT       RCC_CR_HSEON
#           define _FULLOSC_TIMEOUT     3000 //HSE_STARTUP_TIMEOUT
#       endif
#   else
#       define _FULLOSC_ONBIT           _RCC_CR_HSI
#       define _FULLOSC_RDYFLAG         RCC_CR_HSIRDY
#       define _FULLOSC_CLOCKBIT        (1 | RCC_CFGR_STOPWUCK)
#       define _FULLOSC_TIMEOUT         300 //HSI_STARTUP_TIMEOUT
#   endif
#endif

// Standard Speed uses the MSI
#if BOARD_FEATURE(STDSPEED)
#   if (PLATFORM_MSCLOCK_HZ == 4200000)
#       define _MSIRANGE    6
#   elif (PLATFORM_MSCLOCK_HZ == 2100000)
#       define _MSIRANGE    5
#   elif (PLATFORM_MSCLOCK_HZ == 1050000)
#       define _MSIRANGE    4
#   elif (PLATFORM_MSCLOCK_HZ == 524000)
#       define _MSIRANGE    3
#   elif (PLATFORM_MSCLOCK_HZ == 262000)
#       define _MSIRANGE    2
#   elif (PLATFORM_MSCLOCK_HZ == 131000)
#       define _MSIRANGE    1
#   elif (PLATFORM_MSCLOCK_HZ == 655000)
#       define _MSIRANGE    0
#   else 
#       error "PLATFORM_MSCLOCK_HZ is not set to a value matching HW options"
#   endif
#   define _STDSPEED_VOLTAGE    POWER_1V2
#   define _STDSPEED_FLASHWAIT  DISABLED
#endif

// For systems with only FLANKSPEED enabled, drop APB clocks by half
#if ( BOARD_FEATURE(FLANKSPEED)             \
  && (BOARD_FEATURE(STDSPEED) != ENABLED)   \
  && (BOARD_FEATURE(FULLSPEED) != ENABLED)  )
#   undef BOARD_PARAM_AHBCLKDIV
#   define BOARD_PARAM_AHBCLKDIV 2
#   define _APB1SCALE    0
#   define _APB2SCALE    0
#else
#   define _APB1SCALE    0
#   define _APB2SCALE    0
#endif

// Speed Change configurations
#if MCU_CONFIG(MULTISPEED)
#   if BOARD_FEATURE(STDSPEED)
#       define _STDSPEED_ON()       (RCC->CR & RCC_CR_MSION)
#       define _STDSPEED_OFF()      ((RCC->CR & RCC_CR_MSION) == 0)
#   else
#       define _STDSPEED_ON()       0
#       define _STDSPEED_OFF()      1
#   endif
#   if BOARD_FEATURE(FULLSPEED)
#       define _FULLSPEED_ON()      ((RCC->CR & (RCC_CR_PLLON | RCC_CR_MSION)) == 0)
#       define _FULLSPEED_OFF()     (RCC->CR & (RCC_CR_PLLON | RCC_CR_MSION))
#       define _FULLRQ()            (platform_ext.rqfull != 0)
#   else
#       define _FULLSPEED_ON()      0
#       define _FULLSPEED_OFF()     1
#       define _FULLRQ()            0
#   endif
#   if BOARD_FEATURE(FLANKSPEED)
#       define _FLANKSPEED_ON()     (RCC->CR & RCC_CR_PLLON)
#       define _FLANKSPEED_OFF()    ((RCC->CR & RCC_CR_PLLON) == 0)
#       define _FLANKRQ()           (platform_ext.rqflank != 0)
#   else
#       define _FLANKSPEED_ON()     0
#       define _FLANKSPEED_OFF()    1
#       define _FLANKRQ()           0
#   endif
#   define _STDFULL_DOWNVOLT()      (_FULLSPEED_ON() && (_FULLSPEED_VOLTAGE != _STDSPEED_VOLTAGE))
#   define _STDFLANK_DOWNVOLT()     (_FLANKSPEED_ON() && (_STDSPEED_VOLTAGE != _FLANKSPEED_VOLTAGE))
#   define _STD_DOWNVOLT()          (_STDFLANK_DOWNVOLT() || _STDFULL_DOWNVOLT())
#   define _FULL_UPVOLT()           (_STDSPEED_ON() && (_FULLSPEED_VOLTAGE != _STDSPEED_VOLTAGE))
#   define _FULL_DOWNVOLT()         (_FLANKSPEED_ON() && (_FULLSPEED_VOLTAGE != _FLANKSPEED_VOLTAGE))
#   define _FLANKSTD_UPVOLT()       (_STDSPEED_ON() && (_STDSPEED_VOLTAGE != _FLANKSPEED_VOLTAGE))
#   define _FLANKFULL_UPVOLT()      (_FULLSPEED_ON() && (_FULLSPEED_VOLTAGE != _FLANKSPEED_VOLTAGE))
#   define _FLANK_UPVOLT()          (_FLANKSTD_UPVOLT() || _FLANKFULL_UPVOLT())
#elif BOARD_FEATURE(STDSPEED)
#   define _STDSPEED_ON()       (RCC->CR & RCC_CR_MSION)
#   define _STDSPEED_OFF()      ((RCC->CR & RCC_CR_MSION) == 0)
#   define _STD_DOWNVOLT()      0
#   define _FULLRQ()            0
#   define _FLANKRQ()           0
#   define _FLANK_UPVOLT()      (_STDSPEED_VOLTAGE != _FLANKSPEED_VOLTAGE)
#elif BOARD_FEATURE(FULLSPEED)
#   define _FULLSPEED_ON()      ((RCC->CR & (RCC_CR_PLLON | RCC_CR_MSION)) == 0)
#   define _FULLSPEED_OFF()     (RCC->CR & (RCC_CR_PLLON | RCC_CR_MSION))
#   define _FULL_UPVOLT()       0
#   define _FULL_DOWNVOLT()     0
#   define _FULLRQ()            1
#   define _FLANKRQ()           0
#   define _FLANK_UPVOLT()      (_FULLSPEED_VOLTAGE != _FLANKSPEED_VOLTAGE)
#elif BOARD_FEATURE(FLANKSPEED)
#   define _FLANKSPEED_ON()     (RCC->CR & RCC_CR_PLLON)
#   define _FLANKSPEED_OFF()    ((RCC->CR & RCC_CR_PLLON) == 0)
#   define _FLANK_UPVOLT()      0
#   define _FULL_DOWNVOLT()     0
#   define _FULLRQ()            0
#   define _FLANKRQ()           1
#endif

//Validate and set AHB Divider based on board config header setting
#if (BOARD_PARAM_AHBCLKDIV == 1)
#   define _AHB_DIV     (0<<4)
#elif (BOARD_PARAM_AHBCLKDIV == 2)
#   define _AHB_DIV     (8<<4)
#elif (BOARD_PARAM_AHBCLKDIV == 4)
#   define _AHB_DIV     (9<<4)
#elif (BOARD_PARAM_AHBCLKDIV == 8)
#   define _AHB_DIV     (10<<4)
#elif (BOARD_PARAM_AHBCLKDIV == 16)
#   define _AHB_DIV     (11<<4)
#elif (BOARD_PARAM_AHBCLKDIV == 64)
#   define _AHB_DIV     (12<<4)
#elif (BOARD_PARAM_AHBCLKDIV == 128)
#   define _AHB_DIV     (13<<4)
#elif (BOARD_PARAM_AHBCLKDIV == 256)
#   define _AHB_DIV     (14<<4)
#elif (BOARD_PARAM_AHBCLKDIV == 512)
#   define _AHB_DIV     (15<<4)
#else
#   error "BOARD_PARAM_AHBCLKDIV not set to a value permitted by this HW"
#endif

//Validate and set APB1 Divider based on board config header setting
#if (BOARD_PARAM_APB1CLKDIV == 1)
#   define _APB1_DIV    ((0+_APB1SCALE)<<8)
#elif (BOARD_PARAM_APB1CLKDIV == 2)
#   define _APB1_DIV    ((4+_APB1SCALE)<<8)
#elif (BOARD_PARAM_APB1CLKDIV == 4)
#   define _APB1_DIV    ((5+_APB1SCALE)<<8)
#elif (BOARD_PARAM_APB1CLKDIV == 8)
#   define _APB1_DIV    ((6+_APB1SCALE)<<8)
#elif (BOARD_PARAM_APB1CLKDIV == 16)
#   define _APB1_DIV    (7<<8)
#else
#   error "BOARD_PARAM_APB1CLKDIV not set to a value permitted by this HW"
#endif

//Validate and set APB2 Divider based on board config header setting
#if (BOARD_PARAM_APB2CLKDIV == 1)
#   define _APB2_DIV    ((0+_APB2SCALE)<<11)
#elif (BOARD_PARAM_APB2CLKDIV == 2)
#   define _APB2_DIV    ((4+_APB2SCALE)<<11)
#elif (BOARD_PARAM_APB2CLKDIV == 4)
#   define _APB2_DIV    ((5+_APB2SCALE)<<11)
#elif (BOARD_PARAM_APB2CLKDIV == 8)
#   define _APB2_DIV    ((6+_APB2SCALE)<<11)
#elif (BOARD_PARAM_APB2CLKDIV == 16)
#   define _APB2_DIV    (7<<11)
#else
#   error "BOARD_PARAM_APB2CLKDIV not set to a value permitted by this HW"
#endif












/** Platform Data <BR>
  * ========================================================================<BR>
  */
platform_struct     platform;
platform_ext_struct platform_ext;





/** Local Subroutines <BR>
  * ========================================================================<BR>
  */
void sub_voltage_config(ot_u16 pwr_cr_vos_x) {
/// Set Power Configuration based on Voltage Level parameters.
/// Input must be: POWER_1V2, POWER_1V5, POWER_1V8
/// Additionally, PWR_CR_DBP can be ORed in for RTC hacking
    ot_u16 scratch;
    // Power should be enabled by periphclk function, not here
    //RCC->APB1ENR   |= RCC_APB1ENR_PWREN;
    scratch     = PWR->CR & ~(ot_u32)((3<<11) | (1<<8) | (7<<5));
    scratch    |= pwr_cr_vos_x;
    PWR->CR     = scratch;

    // Wait Until the Voltage Regulator is ready
    while((PWR->CSR & PWR_CSR_VOSF) != 0) { }
}


void sub_osc_startup(ot_u16 counter, ot_u32 osc_mask) {
    ///@todo figure out a way to do this with WFE
    // Wait for Oscillator to get ready, counter goes to 0 on failure
    RCC->CR    |= osc_mask;
    osc_mask  <<= (osc_mask & 1);   // hack for STM32L0 HSI
    osc_mask  <<= 1;
    while ( ((RCC->CR & osc_mask) == 0) && (--counter) );

    /// Test if oscillator failed to startup
    if (counter == 0) {
        ///@todo Death message / Death Blinkly
    }
}


void sub_osc_setclock(ot_u32 clock_mask) {
    ot_u32 scratch;
    scratch         = RCC->CFGR & ~(3 | RCC_CFGR_STOPWUCK);
    scratch        |= clock_mask;
    clock_mask      = (clock_mask & 3) << 2;
    RCC->CFGR       = scratch;
    while ( (RCC->CFGR & (3<<2)) != clock_mask);
}



void sub_set_clockhz(ot_ulong cpu_clock_hz) {
/// In interest of speed and size, you need to setup your clock dividers as
/// constants in the board configuration file.
    ///@todo map these 0 shifts to derived constants
    platform_ext.clock_hz[0]    = cpu_clock_hz >> 0;    //AHB
    platform_ext.clock_hz[1]    = cpu_clock_hz >> 0;    //APB1
    platform_ext.clock_hz[2]    = cpu_clock_hz >> 0;    //APB2
}









/** Extended Platform (STM32L-specific) power and PLL control<BR>
  * ========================================================================<BR>
  */
#ifndef EXTF_platform_ext_wakefromstop
void platform_ext_wakefromstop() {

    // Start the clocker right on wakeup.  The clock may get skewed a bit, 
    // but this is acceptable.
    systim_start_clocker();

    // If using Multispeed setup, we want to use the clock that was in use 
    // before going into STOP.
    // - In Standard Speed, don't do anything, MSI is already up and running
    // - In Full Speed + HSI, STOPWUCK will be set and still nothing to do 
    // - In Full Speed + HSE, we need to start-up the crystal
    // - In Flank Speed, we need to start up the PLL and optionally the crystal
#   if MCU_CONFIG(MULTISPEED)
        if ( _FLANKRQ() ) {
            platform_flank_speed();
        }
        else if ( _FULLRQ() && ((_FULLOSC_ONBIT & _RCC_CR_HSI) == 0)) {
            platform_full_speed();
        }

    // Same basic rules as above apply to single-speed configurations.
    // MSI and HSI based clocks will be already running on wakeup.
#   elif (BOARD_FEATURE(FULLSPEED) && ((_FULLOSC_ONBIT & _RCC_CR_HSI) == 0))       
        platform_full_speed();
#   elif BOARD_FEATURE(FLANKSPEED)
        platform_flank_speed();

#   endif
}
#endif


#ifndef EXTF_platform_ext_usbcrson
void platform_ext_usbcrson(void) {
    RCC->CRRCR      = RCC_CRRCR_HSI48ON;
    {   ot_int limit = 3;
        while (((RCC->CRRCR & RCC_CRRCR_HSI48RDY) == 0) && --limit);
        if (limit == 0) {
            __NOP();
            //HW Fault;
            //Reset
        }
    }
    
    RCC->APB1ENR   |= (RCC_APB1ENR_USBEN | RCC_APB1ENR_CRSEN);
    CRS->CFGR       = CRS_CFGR_SYNCPOL_RISING \
                    | CRS_CFGR_SYNCSRC_USB \
                    | (0x22 << 16) /* Default FELIM */ \
                    | (0xBB7F);    /* Default RELOAD (1ms USB SOF) */

    ///@todo make sure CEN doesn't need to be set as an independent follow-up
    CRS->CR         = (32 << 8) | CRS_CR_AUTOTRIMEN | CRS_CR_CEN;
    
    
    // HSI48 requires 6 pulses to stabilize (according to ref manual), so by 
    // this point it should be stable even if CPU is running at 32 MHz.
    // nop nop nop
    //if ((RCC->CRRCR & RCC_CRRCR_HSI48RDY) == 0) {
    //    platform_ext_usbcrsoff();
    //    ///@todo HW Fault
    //}
}
#endif


#ifndef EXTF_platform_ext_usbcrsoff
void platform_ext_usbcrsoff(void) {
    CRS->CR         = (32 << 8) | CRS_CR_AUTOTRIMEN | 0;
    RCC->APB1ENR   &= ~(RCC_APB1ENR_USBEN | RCC_APB1ENR_CRSEN);
    RCC->CRRCR      = 0;
}
#endif



#ifndef EXTF_platform_ext_pllon
void platform_ext_pllon() {
#if BOARD_FEATURE(PLL)
    if ( _FLANK_UPVOLT() ) {
        sub_voltage_config((POWER_1V8 | PWR_CR_DBP | (b010 << 5)));
    }
    BOARD_HSXTAL_ON();
    sub_osc_startup(_FLANKOSC_TIMEOUT, _FLANKOSC_ONBIT);

    RCC->CR |= RCC_CR_PLLON;
    while((RCC->CR & RCC_CR_PLLRDY) == 0) { }
#endif
}
#endif


#ifndef EXTF_platform_ext_plloff
void platform_ext_plloff() {
#if BOARD_FEATURE(PLL)
/// Don't call this function unless you know what you are doing.  STM32L will
/// not shut-off an active clock, so you won't kill your app, but worse: the
/// PLL will stay on even if you probably think it is off.
    RCC->CR &= ~RCC_CR_PLLON;
    BOARD_HSXTAL_OFF();
#endif
}
#endif


#ifndef EXTF_platform_ext_hsitrim
void platform_ext_hsitrim() {
/// Calibrate the HSI clock against LSE.
/// This feature is presently only available through Haystack Distribution of
/// OpenTag (HDO).  Contact Haystack Technologies for more information.
}
#endif


#ifndef EXTF_platform_ext_lsihz
ot_u16 platform_ext_lsihz() {
/// Calibrate the LSI clock against HSE or HSI.
/// This feature is presently only available through Haystack Distribution of
/// OpenTag (HDO).  Contact Haystack Technologies for more information.
    return 37000;
}
#endif








/** Clock Hz retrieval function
  * ========================================================================<BR>
  */
ot_ulong platform_get_clockhz(ot_uint clock_index) {
#   if defined(__DEBUG__)
    if (clock_index > 2) {
        while(1);   //trap in debugging
    }
#   elif defined(__API__)
    if (clock_index > 2) {
        return 0;   //result for dumb APIs
    }
#   endif
    return platform_ext.clock_hz[clock_index];
}








/** Platform Speed Control <BR>
  * ========================================================================<BR>
  * These functions do not yet have a system-level call.  A function named
  * something like "sys_speedcontrol(param)" might be nice to write at some
  * point, which would manage the speed based on different active tasks that
  * request it.
  *
  * In the meantime, a task can call any of these functions, but it is only
  * recommended to call platform_full_speed and platform_flank_speed.  The
  * kernel and indeed the hardware itself manage down-speeding when going
  * into STOP mode.
  */

ot_int sysclock_request(SPEED_enum Speed) {
#if MCU_CONFIG(MULTISPEED)
    ot_int handle;
    switch (Speed) {
#   if BOARD_FEATURE(FULLSPEED)
        case SPEED_Full:    platform_full_speed();
                            platform_ext.rqfull++;
                            handle = 0;
                            break;
#   endif
#   if BOARD_FEATURE(FLANKSPEED)
        case SPEED_Flank:   platform_flank_speed();
                            platform_ext.rqflank++;
                            handle = 1;
                            break;
#   endif
        default: return -1;
    }
    return handle;
#else
    return -1;
#endif
}

ot_int sysclock_dismiss(ot_int handle) {
#if MCU_CONFIG(MULTISPEED)
    if (handle < 0) || (handle > (BOARD_FEATURE(FLANKSPEED) == ENABLED)) {
        return -1;
    }
    platform_ext.rqspeed[handle]--;
    
    if ( _FLANKRQ() )       platform_flank_speed();
    else if ( _FULLRQ() )   platform_full_speed();
    else                    platform_standard_speed();
    return 0;
    
#else
    return -1;
#endif
}


#ifndef EXTF_platform_standard_speed
void platform_standard_speed() {
/// Best efficient speed.  (MSI)
/// typ config: 4.2 MHz, Power Level 3, 0 wait state.  ~0.63mA, 4 DMIPS
/// 
/// Only go through the process of entering Standard Speed if it is not
/// activated already.  Also, in Multispeed systems, we need to make sure
/// there are no active full speed or flank speed requests.
#if BOARD_FEATURE(STDSPEED)
    if ( _STDSPEED_OFF() /* && !_FULLRQ() && !_FLANKRQ() */ ) {
        platform_disable_interrupts();
        
        sub_osc_startup(300, RCC_CR_MSION);

        // On STM32L0, MSI speeds never require flash wait states or prefetch
        sub_osc_setclock(0);
        FLASH->ACR = 0;
        
        // Set new core voltage, if necessary.
        if (STD_DOWNVOLT()) {
            sub_voltage_config(_STDSPEED_VOLTAGE | _RTC_PROTECTION);
        }
        
        // Turn off non-MSI clocks to save power
#       if (BOARD_FEATURE(FULLSPEED) && (_FULLOSC_ONBIT & _RCC_CR_HSI))
        RCC->CFGR  &= ~(RCC_CFGR_STOPWUCK);
#       endif
        RCC->CR    &= ~(RCC_CR_PLLON | RCC_CR_HSEON | _RCC_CR_HSI);
        sub_set_clockhz(PLATFORM_MSCLOCK_HZ);
        platform_ensable_interrupts();
    }
#endif
}
#endif




#ifndef EXTF_platform_full_speed
void platform_full_speed() {
/// All Ahead Full.  (HSI or HSE, no PLL)
/// <LI> typ config: 16MHz, Power Level 2, 1 wait state. ~2.6mA, 15 DMIPS </LI>
/// <LI> In system with attachable USB, check for flank-enable </LI>
/// 
/// Only go through the process of entering Full Speed if it is not activated
/// already.  Also, in Multispeed systems, we need to make sure there are no
/// active full speed or flank speed requests.

#if BOARD_FEATURE(FULLSPEED)
    if ( _FULLSPEED_OFF() /* && !_FLANKRQ() */ ) {
        platform_disable_interrupts();
        
        if (_FULL_UPVOLT()) {
            sub_voltage_config(_FULLSPEED_VOLTAGE | _RTC_PROTECTION);
        }
        
        sub_osc_startup(_FULLOSC_TIMEOUT, _FULLOSC_ONBIT);
        
#       if (_FULLSPEED_FLASHWAIT == ENABLED)
            FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY;
            sub_osc_setclock(_FULLOSC_CLOCKBIT);
#       else
            sub_osc_setclock(_FULLOSC_CLOCKBIT);
            FLASH->ACR = FLASH_ACR_PRFTEN;
#       endif

        if (_FULL_DOWNVOLT()) {
            sub_voltage_config(_FULLSPEED_VOLTAGE | _RTC_PROTECTION);
        }

#       if ((_FLANKOSC_ONBIT & _RCC_CR_HSI) || (BOARD_FEATURE(FULLSPEED) && (_FULLOSC_ONBIT & _RCC_CR_HSI)))
        RCC->CFGR  |= RCC_CFGR_STOPWUCK;
#       endif
        RCC->CR    &= ~(RCC_CR_MSION | RCC_CR_PLLON);
        sub_set_clockhz(PLATFORM_HSCLOCK_HZ);
        platform_enable_interrupts();
    }
#endif
}
#endif


#ifndef EXTF_platform_flank_speed
void platform_flank_speed() {
/// Coming in hot!  (HSI or HSE + PLL)
/// typ config: 32MHz, Power Level 1, 1 wait state.  ~6.3mA, 30 DMIPS.
#if BOARD_FEATURE(FLANKSPEED)
#   if (BOARD_FEATURE(PLL) != ENABLED)
#       error "Cannot have Flank Speed without PLL"
#   endif

    if ( _FLANKSPEED_OFF() ) {
        platform_disable_interrupts();
        platform_ext_pllon();           // This function Manages the UPVOLTING

#       if (BOARD_FEATURE(STDSPEED) || BOARD_FEATURE(FULLSPEED))
            RCC->CFGR = _PLL_SRC | _PLL_MULT | _PLL_DIV | (8<<4) | _APB1_DIV | _APB2_DIV;
#       endif
#       if (_FLANKSPEED_FLASHWAIT == ENABLED)
            FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY;
            sub_osc_setclock(_FLANKOSC_CLOCKBIT);
#       else
            sub_osc_setclock(_FLANKOSC_CLOCKBIT);
            FLASH->ACR = FLASH_ACR_PRFTEN;
#       endif

#       if (BOARD_FEATURE(FULLSPEED) && (_FULLOSC_ONBIT & RCC_CR_HSION))
        RCC->CFGR  &= ~(RCC_CFGR_STOPWUCK);
#       endif
        RCC->CR &= ~RCC_CR_MSION;
        sub_set_clockhz(PLATFORM_PLLCLOCK_HZ);
        platform_enable_interrupts();
    }

#else
    platform_full_speed();

#endif
}
#endif








/** Platform Interrupts <BR>
  * ========================================================================<BR>
  */

#ifndef EXTF_platform_disable_interrupts
OT_INLINE void platform_disable_interrupts(void) {
    __disable_irq();    // CMSIS intrinsic
}
#endif


#ifndef EXTF_platform_enable_interrupts
OT_INLINE void platform_enable_interrupts(void) {
    __enable_irq();     // CMSIS intrinsic
}
#endif







/** Platform Initialization & Control <BR>
  * ========================================================================<BR>
  */
#ifndef EXTF_platform_poweron
void platform_poweron() {

    ///1. Set cooperative task runtime in P-stack,
    ///   Interrupts and protected system calls run in M-stack
    __set_PSP( __get_MSP() );
    __set_CONTROL(2);
    __set_MSP( (ot_u32)&platform_ext.sstack[(OT_PARAM_SSTACK_ALLOC/4)-1] );

    /// 3. Board-Specific power-up configuration
    BOARD_PERIPH_INIT();
    BOARD_POWER_STARTUP();

    ///2. Configure GPIO
    //platform_init_gpio();
    BOARD_PORT_STARTUP();

    /// 3. Configure Clocks
    platform_init_periphclk();
    platform_init_busclk();

    /// 5. Debugging setup: apply to all peripherals
#   if defined(__DEBUG__)
    DBGMCU->CR     |= ( DBGMCU_CR_DBG_SLEEP \
                      | DBGMCU_CR_DBG_STOP \
                      | DBGMCU_CR_DBG_STANDBY);

    DBGMCU->APB1FZ |= ( DBGMCU_APB1_FZ_DBG_TIM2_STOP \
                      | DBGMCU_APB1_FZ_DBG_TIM6_STOP \
                      | DBGMCU_APB1_FZ_DBG_RTC_STOP \
                      | DBGMCU_APB1_FZ_DBG_WWDG_STOP \
                      | DBGMCU_APB1_FZ_DBG_IWDG_STOP \
                      | DBGMCU_APB1_FZ_DBG_I2C1_STOP \
                      | DBGMCU_APB1_FZ_DBG_I2C2_STOP \
                      | DBGMCU_APB1_FZ_DBG_LPTIMER_STOP );

    DBGMCU->APB2FZ |= ( DBGMCU_APB2_FZ_DBG_TIM22_STOP \
                      | DBGMCU_APB2_FZ_DBG_TIM21_STOP );
#   endif

    /// 6. Final initialization of OpenTag system resources
               // Set up connections on the board
    platform_init_interruptor();    // Interrupts OpenTag cares about
    systim_init(NULL);         // Initialize GPTIM (to 1024 Hz)

    /// 7. Initialize Low-Level Drivers (worm, mpipe)
    // Restore vworm (following save on shutdown)
    vworm_init();

    /// 8. This prevents the scheduler from getting called by a preemption
    ///    event until it officially begins.  It allows some tasks to be
    ///    used for special purposes at power-on (namely MPipe).
    platform_ext.task_exit = (void*)__get_PSP();
}
#endif


#ifndef EXTF_platform_poweroff
void platform_poweroff() {
    ISF_syncmirror();
    vworm_save();

#   if ( OT_FEATURE(MPIPE) && MCU_CONFIG(MPIPECDC) )
    mpipe_disconnect(NULL);
#   endif
}
#endif



#ifndef EXTF_platform_init_OT
void platform_init_OT() {
    /// 1. Initialize Data sources required by basically all OT features
    ///    - Buffers module allocates the data queues, used by all I/O
    ///    - Veelite module allocates and prepares the filesystem
    buffers_init();
    vl_init();

    /// 2. Initialize the System Time.  If the build does not support time,
    ///    then this will do nothing.
    time_set_utc(364489200);

    /// 3. Look for errors, report them, and make sure to clear the error RAM.
    /// @note This is a decent place to put a breakpoint when debugging.
    if (RTC->BKP2R != 0) {
        vlFILE*     fp;
        ot_uni16    resets;
        ot_uni16    faults;
        ot_uni32    error;

        error.ulong     = RTC->BKP2R;
        RTC->BKP2R      = 0;
        fp              = ISF_open_su(ISF_ID(hardware_fault_status));
        resets.ushort   = vl_read(fp, 0);
        faults.ushort   = vl_read(fp, 2);
        resets.ubyte[0]+= (error.ushort[LOWER] != 0);
        resets.ubyte[1]+= (error.ushort[UPPER] != 0);
        faults.ubyte[1]+= (error.ushort[LOWER] == 11) << 1; //Memory Fault

        vl_write(fp, 0, resets.ushort);
        vl_write(fp, 2, faults.ushort);
        vl_close(fp);
    }

    /// 4. Initialize the System (Kernel & more).  The System initializer must
    ///    initialize all modules that are built onto the kernel.  These include
    ///    the DLL and MPipe.
    sys_init();

    /// 5. If debugging, copy the UNIQUE ID that ST writes into the ROM into
    ///    the lower 48 bits of the Mode 2 UID (Device Settings ISF)
    ///
    /// @note the ID is inserted via Veelite, so it is abstracted from the
    /// file memory configuration of your board and/or app.
    ///
    /// @note for production (__RELEASE__) the default UID should be written to
    ///      the default file location by the manufacturer firmware upload.
#   if (defined(__DEBUG__) || defined(__PROTO__))
    {   vlFILE* fpid;
        ot_u16* hwid;
        ot_int  i;

        fpid    = ISF_open_su(ISF_ID(device_features));
        hwid    = (ot_u16*)(0x1FF80050);
        for (i=6; i!=0; i-=2) {
            vl_write(fpid, i, *hwid++);
        }
        vl_close(fpid);
    }
#   else

#   endif
}
#endif



#ifndef EXTF_platform_init_busclk
void platform_init_busclk() {
/// This function should be called during initialization and restart, right at
/// the top of platform_poweron().
    ot_u16 counter;

    ///1. RESET System Clocks
    ///@todo This may not be necessary.  These settings should be reset default settings.

    // Assure MSI bit is on (it should be, by default)
    RCC->CR    |= (uint32_t)0x00000100;

    // Configure dividers and PLL information (even if not used) and keep 
    // active clock via MSI (these fields are 0)
    RCC->CFGR   = ( _PLL_SRC | _PLL_MULT | _PLL_DIV | _AHB_DIV | _APB1_DIV | _APB2_DIV );

    // Reset HSION, HSEON, HSEBYP, CSSON and PLLON bits
    // Disable all clocker interrupts (default)
    RCC->CR    &= (uint32_t)0xEEFAFFFE;
    //RCC->CIR    = 0x00000000;


    ///2. Prepare external Memory bus (not currently supported)
//#   ifdef DATA_IN_ExtSRAM
//        SystemInit_ExtMemCtl();
//#   endif


    ///3a. Begin clocking system with MSI clock at specified frequency.
    ///    <LI> Specified as PLATFORM_MSCLOCK_HZ in board support header </LI>
    ///    <LI> MSI is only used as standard clock if BOARD_FEATURE_STDSPEED
    ///           is also ENABLED in board support header.
#   if BOARD_FEATURE(STDSPEED)
        FLASH->ACR = 0;
        sub_voltage_config((_STDSPEED_VOLTAGE | PWR_CR_DBP));
        {   ot_u32 rcc_icscr;
            rcc_icscr   = RCC->ICSCR;
            rcc_icscr  &= ~(7 << 13);
            rcc_icscr  |= (_MSIRANGE << 13);
            RCC->ICSCR  = rcc_icscr;
        }

        // Setup the Bus Dividers as specified (MSI already selected as system clock)
        sub_set_clockhz(PLATFORM_MSCLOCK_HZ);


    ///3b. Use HSE or HSI without PLL as Full-Speed clock
    ///    <LI> HSE or HSI is full speed clock if BOARD_FEATURE_STDSPEED is
    ///           DISABLED and BOARD_FEATURE_FULLSPEED is ENABLED</LI>
    ///    <LI> HSE is used if BOARD_FEATURE_HFXTAL is ENABLED, else HSI used. </LI>
    ///    <LI> Boards using HSE can declare any value into PLATFORM_HSCLOCK_HZ.
    ///           Board using HSI may only declare 2, 4, 8, or 16 MHz</LI>
#   elif BOARD_FEATURE(FULLSPEED)
#       if ((_FULLSPEED_VOLTAGE != POWER_1V5) && (_FULL_UPVOLT() == 0))
            sub_voltage_config(_FULLSPEED_VOLTAGE | _RTC_PROTECTION);
#       endif
        // Basic Flash setup, then run normal routine
        FLASH->ACR = FLASH_ACR_PRFTEN;
        platform_full_speed();


    ///3c. Begin clocking system with PLL driven from HSE or HSI
    ///    <LI> PLL only used if .... </LI>
    ///    <LI> Use BOARD_PARAM_PLLdiv, BOARD_PARAM_PLLmult to specify the
    ///           particular PLL configuration.  For example, if using USB
    ///           (BOARD_PARAM_HFHz * BOARD_PARAM_PLLmult) must be 96 MHz, and
    ///           (96 MHz / BOARD_PARAM_PLLdiv) == PLATFORM_HSCLOCK_HZ. </LI>
#   elif BOARD_FEATURE(FLANKSPEED)
#       if ((_FLANKSPEED_VOLTAGE != POWER_1V5) && (_FLANK_UPVOLT() == 0))
            sub_voltage_config(_FULLSPEED_VOLTAGE | _RTC_PROTECTION);
#       endif
        // Basic Flash setup, then run normal routine
        FLASH->ACR = FLASH_ACR_PRFTEN;
        platform_flank_speed();

#   else
#       error "At least one of BOARD_FEATURE_STDSPEED, _FULLSPEED, or _FLANKSPEED must be ENABLED"
#   endif

    
    ///4. Clock selection for special buses
    RCC->CCIPR  = ((BOARD_FEATURE(USBPLL)!=ENABLED) << 26)  /* HSI48MSEL */ \
                | (BOARD_FEATURE(LFXTAL) << 19) | (1 << 18) /* LSI/LSE for LPTIM */ \
                | ((MCU_CONFIG(MULTISPEED)*2) << 12)        /* APB/HSI16 for I2C1 */ \
                | ((BOARD_FEATURE(LFXTAL)*3) << 10)         /* APB/LSE for LPUART */ \
                | ((MCU_CONFIG(MULTISPEED)*2) << 2)         /* Use APB/HSI16 for USART2 */ \
                | ((MCU_CONFIG(MULTISPEED)*2) << 0);        /* APB/HSI16 for USART1 Clock */


    /// X. Vector Table Relocation in Internal SRAM or FLASH.
#   ifdef VECT_TAB_SRAM
#       error "Silly rabbit! SRAM is for DATA!"
        SCB->VTOR   = SRAM_BASE;
#   else
        SCB->VTOR   = FLASH_BASE;
#   endif
}
#endif



#ifndef EXTF_platform_init_periphclk
void platform_init_periphclk() {
/// Turn-on LSE or LSI, it is used by some peripherals.  In particular,
/// OpenTag likes having a 32768Hz clock for timing purposes.  TIM9, 10, 11,
/// and the RTC are all driven by the LF clock.

/*
#   define CSR_BYTE2_ADDRESS        ((uint32_t)0x40023835)
#   define RCC_LSE_OFF              ((uint8_t)0x00)
#   define RCC_LSE_ON               ((uint8_t)0x01)
#   define RCC_LSE_Bypass           ((uint8_t)0x05)
#   define RCC_LSE_RDY              ((uint8_t)0x02)
#   define RCC_LSI_OFF              ((uint8_t))
#   define RCC_LSI_ON               ((uint8_t))
#   define RCC_LSI_Bypass           ((uint8_t))
#   define RCC_LSI_RDY              ((uint8_t))

#define CR_OFFSET                (PWR_OFFSET + 0x00)
#define DBP_BitNumber            0x08
#define CR_DBP_BB                (PERIPH_BB_BASE + (CR_OFFSET * 32) + (DBP_BitNumber * 4))
*/
    ot_u32  pwr_cr_save = (PWR->CR | PWR_CR_DBP);

#   if BOARD_FEATURE(LFXTAL)
    PWR->CR     = ((1 << 11) | PWR_CR_DBP);
    RCC->CSR    = RCC_CSR_RMVF | RCC_CSR_RTCRST;
    RCC->CSR    = RCC_CSR_LSEON | RCC_CSR_RTCEN | RCC_CSR_RTCSEL_LSE;
    while ((RCC->CSR & RCC_CSR_LSERDY) == 0);

#   else // enable LSI
    PWR->CR     = ((1 << 11) | PWR_CR_DBP);
    RCC->CSR    = RCC_CSR_RMVF | RCC_CSR_RTCRST;
    RCC->CSR    = RCC_CSR_LSION | RCC_CSR_RTCEN | RCC_CSR_RTCSEL_LSI;
    while ((RCC->CSR & RCC_CSR_LSIRDY) == 0);

#   endif

   PWR->CR = (pwr_cr_save);
}
#endif










/** OpenTag Resource Initializers <BR>
  * ========================================================================<BR>
  */

#ifndef EXTF_platform_init_interruptor
void platform_init_interruptor() {
/// The NVIC setup can use any number of groups supported by Cortex M.  The
/// strategy is to store I/O interrupts as the highest priority group.  The
/// Kernel interrupts go in the next highest group.  Everything else is above.
/// Apps/Builds can get quite specific about how to set up the groups.

#   define _KERNEL_GROUP    b00
#   define _HIPRI_BASE      b00
#   define _LOPRI_BASE      b11
#   define _SUB_LIMIT       b11

    /// 1. Set the EXTI channels using the board function.  Different boards
    ///    are connected differently, so this function must be implemented in
    ///    the board support header.
    BOARD_EXTI_STARTUP();

    /// 2. Cortex M0 doesn't have NVIC priority grouping, so nothing to do here
    //NVIC_SetPriorityGrouping(_GROUP_PRIORITY);

    /// 3. Setup Cortex-M system interrupts
    /// <LI> Fault IRQs (Mem management, bus-fault, usage-fault) can be enabled
    ///         if you want more clarity of the fault than just Hard-Fault </LI>
    /// <LI> SVC IRQ is for supervisor-call.  The kernel needs it. </LI>
    /// <LI> Pend SV is for supervisor-call-pending.  The kernel needs it. </LI>
    /// <LI> Debug-Monitor is not used </LI>
    /// <LI> Systick is not used and it is inadvisable to use because it is a
    ///         power hog and because it is mostly useless with OpenTag. </LI>
//  SCB->SHP[_SHP_IDX(MemoryManagement_IRQn)  = (b00 << 4);
//  SCB->SHP[_SHP_IDX(BusFault_IRQn)]         = (b00 << 4);
//  SCB->SHP[_SHP_IDX(UsageFault_IRQn)]       = (b00 << 4);
    SCB->SHP[_SHP_IDX(SVC_IRQn)]            = (b00 << 4);
    SCB->SHP[_SHP_IDX(PendSV_IRQn)]         = (b11 << 4);
//  SCB->SHP[_SHP_IDX(DebugMonitor_IRQn)]     = (b00 << 4);

    // Systick needs SCB and NVIC to be enabled in order to run.
#   if defined(YOU_ARE_AN_IDIOT)
    NVIC_SetPriority(IRQn_Type IRQn, _LOPRI_BASE);
    NVIC_EnableIRQ(SysTick_IRQn);
#   endif

    /// 4. Setup NVIC for Kernel Interrupts.  Kernel interrupts cannot interrupt
    /// each other, but there are subpriorities.  I/O interrupts should be set
    /// in their individual I/O driver initializers.
    ///    <LI> NMI will interrupt anything.  It is used for panics.    </LI>
    ///    <LI> SVC is priority 0-0.  It runs the scheduler. </LI>
    ///    <LI> LPTIM is priority 0-2.  It runs the tasker.  </LI>
    ///    <LI> If Mode 2 is enabled, RTC-Wakeup is the MAC-insertion timer and
    ///         is priority 0-1.  If not, RTC-Wakeup is low-priority and it is
    ///         only used for the interval timer (watchdog/systick) </LI>

    // From Reference Manual RM0376, page 263:
    // Line 16: PVD
    // Line 17: RTC Alaram
    // Line 18: USB Wakeup Event
    // Line 19: RTC-Tamper/Timestamp/CSS_LSE
    // Line 20: RTC-Wakeup
    // Line 21: COMP1-out
    // Line 22: COMP2-out
    // Line 23: I2C1-wakeup
    // Line 25: USART1-wakeup
    // Line 26: USART2-wakeup
    // Line 28: LPUART1-wakeup
    // Line 29: LPTIM-wakeup

    EXTI->PR    = (1<<20) | (1<<29);
    EXTI->IMR  |= (1<<20) | (1<<29);
    EXTI->RTSR |= (1<<20) | (1<<29);

#   if OT_FEATURE(M2)
        NVIC_SetPriority(RTC_IRQn, (_KERNEL_GROUP+1));
        NVIC_EnableIRQ(RTC_IRQn);
#   else
        NVIC_SetPriority(RTC_IRQn, (_LOPRI_BASE+1));
        NVIC_EnableIRQ(RTC_IRQn);
#   endif

    NVIC_SetPriority(LPTIM1_IRQn, (_KERNEL_GROUP+2));
    NVIC_EnableIRQ(LPTIM1_IRQn);


    /// 5. Setup other external interrupts
    /// @note Make sure board files use the __USE_EXTI(N) definitions
#   if defined(__USE_EXTI0) || defined(__USE_EXTI1)
    NVIC_SetPriority(EXTI0_1_IRQn, _HIPRI_BASE);
    NVIC_EnableIRQ(EXTI0_1_IRQn);
#   endif
#   if defined(__USE_EXTI2) || defined(__USE_EXTI3)
    NVIC_SetPriority(EXTI2_3_IRQn, _HIPRI_BASE);
    NVIC_EnableIRQ(EXTI2_3_IRQn);
#   endif
#   if( defined(__USE_EXTI4)  || defined(__USE_EXTI5)  || defined(__USE_EXTI6) \
    ||  defined(__USE_EXTI7)  || defined(__USE_EXTI8)  || defined(__USE_EXTI9) \
    ||  defined(__USE_EXTI10) || defined(__USE_EXTI11) || defined(__USE_EXTI12) \
    ||  defined(__USE_EXTI13) || defined(__USE_EXTI14) )
    NVIC_SetPriority(EXTI4_15_IRQn, _HIPRI_BASE);
    NVIC_EnableIRQ(EXTI4_15_IRQn);
#   endif


    /// 6. Setup ADC interrupt.  This is needed only for ADC-enabled builds,
    ///    but ADC is used for true-random-number generation as well as actual
    ///    analog voltage sensing.
//#   if defined(__USE_ADC1)
    NVIC_SetPriority(ADC1_COMP_IRQn, _HIPRI_BASE);
    NVIC_EnableIRQ(ADC1_COMP_IRQn);
//#   endif

}
#endif





#ifndef EXTF_platform_init_gpio
void platform_init_gpio() {
    BOARD_PORT_STARTUP();
}
#endif



// systim_init() is implemented in platform_systim_STM32L1xx.c



#ifndef EXTF_platform_init_watchdog
void platform_init_watchdog() {
/// The watchdog used here is the "Independent Watchdog."  OpenTag does not
/// officially use the watchdog anywhere -- it has a kernel to manage tasks.
/// You can use this for your needs.
}
#endif


#ifndef EXTF_platform_init_resetswitch
void platform_init_resetswitch() {
/// Currently unused
}
#endif



#ifndef EXTF_platform_init_memcpy
void platform_init_memcpy() {
#if (MCU_CONFIG(MEMCPYDMA) == ENABLED)
#endif
}
#endif



