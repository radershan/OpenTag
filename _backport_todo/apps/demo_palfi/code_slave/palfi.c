/* Copyright 2012 JP Norair
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
  */
/**
  * @file       /apps/demo_palfi/code_slave/palfi.c
  * @author     JP Norair
  * @version    R100
  * @date       10 October 2012
  * @brief      PaLFi Demo Slave Features
  *
  * This application for OpenTag would qualify as an intermediate example (OK, 
  * maybe an advanced example) of what can be done with an asynchronous-pre-
  * emptive kernel (APE).  The PaLFi app requires a lot of ordered wait-slots, 
  * and the APE kernel is great for this.  
  *
  * The TI code that was used as a reference is entirely blocking, and this 
  * code is almost entirely non-blocking.  Making it non-blocking allows the 
  * MCU to sleep, saving power.  Make sure to check the eventno in the sleeping
  * loop, because this is like a Mutex for the PaLFi application, and PaLFi 
  * cannot go into LPM3 while it is engaged (it needs port IO).  In the main.c
  * associated with this demo, this Mutex feature is already implemented.
  *
  * The PaLFi application has multiple features, depending on what buttons are
  * pressed on the board or what type of PaLFi LF signal is sent from the base
  * station.  The features and their processes are managed as an internal state
  * machine.  The event handle (sys.evt.EXT.eventno) is used to identify the
  * active feature.  When eventno == 0, the PaLFi App is not underway, which
  * means also that there is no PaLFi signal.  When eventno != 0, it means that
  * something is happening with PaLFi.  The precise value of eventno depends on
  * what state of operation the PaLFi app is undergoing.  In addition, for some
  * complex features (like trimming), there is an internal state-tracking 
  * mechanism that uses a function pointer to select different routines, with
  * wait slots in between.
  *
  ******************************************************************************
  */

#include "OTAPI.h"
#include <otplatform.h>
#include "palfi.h"


#define PALFI_TASK (&sys.task[TASK_external])


/** Number of switches (physical buttons) on your board that are attached to
  * the PaLFi core.  On the standard keyfob board, there are 3 switches, but
  * only SW0 and SW1 are connected to the PaLFi Core.  SW2 is connected to 
  * P1.5, of the normal MCU core.
  */
#ifndef PALFI_SWITCHES
#   define PALFI_SWITCHES 2
#endif


/** The PaLFi app here has two buttons and two kinds of Wake-up.
  * - Wakeup A and Button 0 will cause DASH7 message on CHAN1
  * - Wakeup B and Button 1 will cause DASH7 message on CHAN2
  *
  * CHAN1 and CHAN2 can be the same channel.  Channel 0x07 is the base channel
  * for DASH7, that is required on all devices.
  */
#ifndef APP_FEATURE_CHAN1
#   define ALERT_CHAN1  0x07
#else
#   define ALERT_CHAN1  APP_FEATURE_CHAN1
#endif

#ifndef APP_FEATURE_CHAN2
#   define ALERT_CHAN2  0x07
#else
#   define ALERT_CHAN2  APP_FEATURE_CHAN2
#endif


tempmodel_struct    tmodel;
palfi_struct        palfi;
palfiext_struct     palfiext;




void sub_program_channels(palfi_CHAN channel, ot_u8 trim_val, ot_u8 base_val);
void sub_prog_trimswitch(ot_s8 trim_val);
ot_int sub_measurefreq_init(ot_u8 trim_val);
void sub_measurefreq_finish(float* t_pulse);
void sub_calculate_trim();

ot_u8 sub_spi_trx(ot_u8 write);
void applet_adcpacket(m2session* session);
void sub_starttask_uhf();
void sub_adc_measurement(ot_int* buffer);
void sub_build_uhfmsg(ot_int* buffer);


// Palfi Actions are non-blocking states in the application.  Simple features 
// of the application execute in a single state.  More complex features (e.g.
// trimming) require a multi-state approach.
ot_int palfi_action_spitrim_0(void);
ot_int palfi_action_spitrim_1(void);
ot_int palfi_action_spitrim_2(void);
ot_int palfi_action_spitrim_3(void);
ot_int palfi_action_spitrim_4(void);
ot_int palfi_action_spitrim_5(void);
ot_int palfi_action_swtrim_0(void);
ot_int palfi_action_swtrim_1(void);





/** PALFI Wake-up source interrupt  <BR>
  * ========================================================================<BR>
  * This is the highest-level PaLFi interrupt.  It is generated by the PaLFi
  * core and received on Port 1 of the CC430 (PALFI_WAKEUP_PORT).  PaLFi 
  * processes begin on the CC430 when this interrupt initializes one of those
  * processes.
  */

// PALFI_WAKE_ISR should be defined in the board config file.
void PALFI_WAKE_ISR(void) {
/// Disable and clear the LF wakeup interrupt bit.  It will need to be
/// re-enabled after the application runs, but with it off it will not get in
/// the way of non-blocking process.
    ot_u8 event_call;

    // disable & clear P1.0 interrupt 
    PALFI_WAKE_PORT->IE  &= ~PALFI_WAKE_PIN;
    PALFI_WAKE_PORT->IFG &= ~PALFI_WAKE_PIN;
    
    /// If the SW2 is being held, then trimming will occur.  If not, then the
    /// normal routine will occur.  Both require an initial wait slot, because
    /// there is some time needed for the PaLFI I/O subsystem to stabilize.
    /// The exact wait slot is not documented, but it is less than 32 ticks and
    /// more than 20 ticks.  Here, 32 ticks is used.
    event_call = 1 + ((BOARD_SW2_PORT->DIN & BOARD_SW2_PIN) == 0);

    // Pre-empt the kernel, which will clock events and attach the wait slot
    sys_task_setevent(PALFI_TASK, event_call);
    sys_task_setreserve(PALFI_TASK, 64);
    sys_task_setlatency(PALFI_TASK, 1);
    sys_preempt(PALFI_TASK, 32);
}




/** PALFI Timer edge-capture interrupt  <BR>
  * ========================================================================<BR>
  * The trimming process needs to measure some time between edges in order to
  * determine the necessary adjustment.  This interrupt is used during the 
  * SPI Trimming process, and it is transparent to the application.
  */

//PALFI_TIM_ISR should be defined in the board config file...
void PALFI_TIM_ISR(void) {
/// When the pulse counter passes the 1st interval, log the pulse width.
/// When the pulse counter passes the 2nd interval, log the pulse width and
/// terminate the pulse measuring subprocess.

    if (palfi.trim.count == palfi.trim.startcount) {
        palfi.trim.startval = PALFI_TIM->CCR0;
    }

    palfi.trim.count++;
    
    if (palfi.trim.count == palfi.trim.endcount) {
        palfi.trim.endval       = PALFI_TIM->CCR0;
        PALFI_TIM->CCTL0       &= ~CCIE;
        
        ///@todo Need to loop back to the trimming
    }
}






/** OpenTag Kernel "External Process" Signal callback  <BR>
  * ========================================================================<BR>
  * The main OpenTag feature used with the PaLFi application is the External
  * process feature of the kernel.  This features allows an application process
  * to make use of the kernel's timing abilities, and it guarantees that this
  * process will not collide with other important processes that may need to
  * occur during normal OpenTag operation.
  *
  * The OpenTag kernel implements asynchronous-pre-emptive multiprocessing.  
  * You need to activate the process by setting sys.evt.EXT.event_no to a non-
  * zero value.  The value of event_noonly matters to the application.  In this
  * application, it is used as a feature selector and also as a state-tracker.
  * 
  * In addition, the kernel is asynchronous so you also need to tell it when it
  * should return to this process by setting sys.evt.EXT.nextevent to a 
  * positive value (roughly milliseconds)
  */

#ifndef EXTF_ext_systask
#   error "EXTF_ext_systask must be defined in extf_config.h for this app to work"
#endif

void ext_systask(ot_task task) {
/// This function is a static callback that the kernel calls after the EOF
/// wait slot elapses.  It is defined in /OTlib/system.h.  This implementation
/// checks the value of the event number, which indicates if there should be 
/// the normal routine, the trimming routine, or nothing.

    sys_sig_extprocess_TOP:
    switch (task->event) {
        // Normal 0.1
        case 1:     // no break
        
        // Trimming & Normal 0.1
        case 2: {   
            palfi_spi_startup();
            palfi_cmdstatus();

            // Check for Wake A/B event on [0]:BIT0/BIT1, which we call event A/B
            // Wake A and B cannot physically happen at the same time
            palfi.wake_event = (palfi.status[0] & 3);

            // On PaLFI wakeup, return to the kernel and supply enough
            // time to RX the whole PaLFI packet (250ms is generic)
            if (palfi.wake_event) {
                palfi.wake_event   += ('A'-1);
                task->event        += 2;
                palfi_cmdrssi();
            }

            // On a switch-press wakeup, set the event number to the
            // appropriate switch case (7-10) and go directly to it.
            // Also, wipe the RSSI buffer.
            else {
                ot_u8 i;
                for (i=0; i<PALFI_SWITCHES; i++) {
                    if (palfi.status[2] & (1<<i)) {
                        palfi.wake_event = i+'1';
                        break;
                    }
                }
                task->event = (palfi.wake_event) ? task->event+4+(i<<1) : 0;
                memset(&palfi.rssi_info, 0xFF, 6);
            }
        }
        goto sys_sig_extprocess_TOP;
        
        // Normal Exit (no break, also includes Trimming exit)
        case 3:
        sys_sig_extprocess_EXIT1:
            // if transmitted LF data = 1/2, activate/deactivate TPS
            if (palfi.status[3] == 1) {
                PALFI_BYPASS_PORT->DOUT &= ~PALFI_BYPASS_PIN;
            }
            else if (palfi.status[3] == 2) {
                PALFI_BYPASS_PORT->DOUT |= PALFI_BYPASS_PIN;
            }
        
        // Trimming & Normal Exit
        // There was success, so start the UHF dialog task.  You can edit what
        // exactly the uhf task does in its implementation (also this file)
        case 4:
        sys_sig_extprocess_EXIT2:
            palfi_powerdown();
            { // Add new DASH7 comm task to kernel, using most defaults.
                session_tmpl s_tmpl;
                s_tmpl.channel      = (palfi.wake_event & 1) ? \
                                        ALERT_CHAN1 : ALERT_CHAN2;
                s_tmpl.subnetmask   = 0;
                s_tmpl.flagmask     = 0;
                m2task_immediate(&s_tmpl, &applet_adcpacket);
            }
            return;
        
        // Normal Event 1: Bypass ON, VCL OFF
        case 5:     
            PALFI_BYPASS_PORT->DOUT    |= PALFI_BYPASS_PIN;
            PALFI_VCLD_PORT->DOUT      &= ~PALFI_VCLD_PIN;
            goto sys_sig_extprocess_EXIT1;
        
        // Trimming Event 1: SPI Trimming (multi-state process)
        case 6:     
            palfi.action = &palfi_action_spitrim_0;
            break;
        
        // Normal Event 2: Bypass OFF
        case 7:     
            PALFI_BYPASS_PORT->DOUT &= ~PALFI_BYPASS_PIN;
            goto sys_sig_extprocess_EXIT1;
        
        // Trimming Event 2: Switch Trimming (multi-state process)
        case 8:     
            palfi.action = &palfi_action_swtrim_0;
            break;
        
        // Some type of error
        default:    palfi_powerdown();
                    return;
    }

    // PaLFI action manager
    // If Action returns 0, the process has completed.
    // If Action returns <0, the process is interrupt-driven and the kernel
    //   task should ignore.
    // If Action returns >0, the process is timed via the task.
    {
    	ot_int task_next;
    	task_next = palfi.action();
    	if (task_next == 0) {
    		goto sys_sig_extprocess_EXIT2;
    	}
    	if (task_next > 0) {
    		sys_task_setnext(task, (ot_u32)task_next);
    	}
    }
}






/** PaFLi Application Functions
  * ========================================================================<BR>
  *
  */
void applet_adcpacket(m2session* session) {
/// This is an OpenTag session Applet.  It gets called by the kernel when the
/// communication task (session) that it is attached-to gets activated by the
/// kernel.  The kernel will wait until a currently-running communication task
/// is over before starting a new one.
///
/// In order to create a new communication task and bind this applet to it, use
/// m2task_immediate() or one of the other tasker functions.
///
/// This applet does two things:
/// 1. Do an ADC capture
/// 2. Build a DASH7 UDP packet that includes PaLFI data and the ADC values
///    that were just captured.  The app protocol inside UDP is a generic TLV.
///
/// @note The kernel automatically detaches the applet from the session after
/// it runs.  You can reattach in this function code by setting:
/// session->applet = &applet_adcpacket;
/// However, there is no reason to do so in this application, because the
/// communication method is not a persistent stream or query.  It is just a
/// single Push+ACK.  Retries are managed internally by the session.
///
    ot_int data_buffer[2];
    sub_adc_measurement(data_buffer);
    sub_build_uhfmsg(data_buffer);
}





void sub_adc_measurement(ot_int* buffer) {
/// This is a blocking ADC capture routine.  It should run in 50us or less.

    /// 1. Universal ADC config
    ///    <LI> Reset REFMSTR, REFVSEL_1 = 2.0V </LI>
    ///    <LI> Voltage Tsample > 1.2us, Temp Tsample > 30us, so use ADCCLK/32
    ///         for Voltage, ADCCLK/768 for Temp.</LI>
    ///    <LI> Also in ADCCTL0, use multisample mode, use REF=2.0V </LI>
    ///    <LI> Use MEM7 (Temp) & MEM8 (Volt), Use internal sampling timer, use MODCLK </LI>
    ///    <LI> Use 12 bit mode, use fast mode </LI>
    ///    <LI> MEM7 is Temp, MEM8 is Volt </LI>
    REFCTL0     = REFMSTR + REFON + REFVSEL_1;
    ADC12CTL0   = 0;
    ADC12CTL0   = ADC12SHT1_3 + ADC12SHT0_7 + ADC12MSC + ADC12REFON + ADC12ON;
    ADC12CTL1   = ADC12CSTARTADD_7 + ADC12SHP + ADC12CONSEQ_1;
    ADC12CTL2   = ADC12RES_2;
    ADC12MCTL7  = ADC12SREF_1 + ADC12INCH_10;
    ADC12MCTL8  = ADC12SREF_1 + ADC12INCH_11 + ADC12EOS;

    /// 2. Start ADC and Wait for ADC to finish.  Wait 75us for REF.
    ///    Grab the data, then kill everything
    delay_us(75);
    ADC12CTL0  |= ADC12ENC;
    ADC12CTL0  |= ADC12SC;
    while ((ADC12CTL1 & ADC12BUSY) == ADC12BUSY);
    
    ADC12CTL0  &= ~(ADC12ENC | ADC12SC);
    ADC12CTL0  &= ~(ADC12ON + ADC12REFON);
    REFCTL0    &= ~(REFMSTR + REFON + REFGENACT);

    /// 3. Convert Temperature:
    ///@todo Build a Fixed-Point Model instead of this heavy floating point one.
    ///
    /// This temperature conversion method pulls device-specific calibration
    /// data from the TLV space and uses it to produce a linear model to map
    /// the acquired ADC value.
    {
        float val_dC;
        val_dC      = tmodel.slope_dC*(float)ADC12MEM7 + tmodel.offset_dC;
        buffer[0]   = (ot_int)val_dC;
    }

    /// 4. Convert Voltage:
    /// Vdd is acquired as 12 bit number representing Vdd/2 in 1/4095V units.
    /// x(V) = 4095*(Vdd/2)/1.93V; x(mV) = (4095/2*1930mV)Vdd ~= Vdd
    //buffer[1]   = volt;                           // Cheap way, not accurate
    buffer[1]   = (ot_int)((float)ADC12MEM8 * (3860.f/4095.f));      // Higher accuracy method
}




void sub_build_uhfmsg(ot_int* buffer) {
/// This is the routine that builds the DASH7 UDP generic protocol message.
/// The protocol has data elements marked by a letter (T, V, R, E, D) that
/// signify Temperature, Voltage, RSSI (LF), PaLFi wake Event, and RX Data.
/// The elements are fixed/known length.
    command_tmpl    c_tmpl;
    ot_u8*          data_start;
    ot_u8           status;
    
    // Broadcast request (takes no 2nd argument)
    otapi_open_request(ADDR_broadcast, NULL);
    
    // Insert Transport-Layer headers
    c_tmpl.type     = CMDTYPE_na2p_request;
    c_tmpl.opcode   = CMD_udp_on_file;
    c_tmpl.extension= CMDEXT_no_response;
    otapi_put_command_tmpl(&status, &c_tmpl);
    otapi_put_dialog_tmpl(&status, NULL);       // NULL = defaults
    
    // UDP Header
    q_writebyte(&txq, 255);        // Source Port: 255 (custom application port)
    q_writebyte(&txq, 255);        // Destination Port (same value)
    
    data_start = txq.putcursor;
    
    // Place temperature data
    q_writebyte(&txq, 'T');
    q_writeshort(&txq, buffer[0]);
    
    // Place Voltage data
    q_writebyte(&txq, 'V');
    q_writeshort(&txq, buffer[1]);
    
    // Place RSSI data
    q_writebyte(&txq, 'R');
    q_writestring(&txq, (ot_u8*)&palfi.rssi1, 3);

    // Place Action data
    q_writebyte(&txq, 'E');
    q_writebyte(&txq, (ot_int)palfi.wake_event);
    
    // Dump some received data
    if (palfi.wake_event) {
        q_writebyte(&txq, 'D');
        q_writestring(&txq, palfi.rxdata, 8);
    }
    
    // Store this information into the Port 255 file for continuous, automated
    // reporting by DASH7/OpenTag until it is updated next time.  The length of 
    // this information is always 23 bytes.
    {
        vlFILE* fp;
        fp = ISF_open_su(255);
        if (fp != NULL) {
            vl_store(fp, 23, data_start);
            vl_close(fp);
        }
    }
    
    // Finish Message
    otapi_close_request();
}







/** Top Level PaFLi Functions & subroutines
  * ========================================================================<BR>
  *
  */
void palfi_init() {
    // Build Calibrated Model for Temperature Sensor
    {
        float test_30C2V;
        float test_85C2V;
        test_30C2V          = (float)(*((ot_u16*)0x1A1E));
        test_85C2V          = (float)(*((ot_u16*)0x1A20));
        tmodel.slope_dC     = (850.f - 300.f) / (test_85C2V - test_30C2V);
        tmodel.offset_dC    = 300.f - (tmodel.slope_dC*test_30C2V);
    }

    // Clear Status buffer
    memset(palfi.status, 0, 4);

    // init wakeup port (should be P1.0)
    PALFI_WAKE_PORT->DDIR  &= ~PALFI_WAKE_PIN;
    PALFI_WAKE_PORT->IFG   &= ~PALFI_WAKE_PIN;
    PALFI_WAKE_PORT->IES    = 0;
    PALFI_WAKE_PORT->IE    |= PALFI_WAKE_PIN;
    
    // init EOB & BUSY ports (should be P4.1, P4.2)
    PALFI_EOB_PORT->DDIR   &= ~(PALFI_EOB_PIN | PALFI_BUSY_PIN);
    
    // init Clock Extern output (should be P2.5)
    PALFI_CLKEXT_PORT->DDIR |= PALFI_CLKEXT_PIN;
    
    // init PaLFI Clock-out input onto PaLFi Timer (should be P3.5)
    PALFI_TIM_PORT->DDIR   &= ~(PALFI_CLKOUT_PIN);
    PALFI_TIM_PORT->SEL    |= PALFI_CLKOUT_PIN;
    
    // init SPI pins: don't perform SEL, this is automated later
    PALFI_SPI_PORT->DDIR   &= ~(PALFI_SPIMISO_PIN);
    PALFI_SPI_PORT->DDIR   |= (PALFI_SPIMOSI_PIN | PALFI_SPISCK_PIN);
    
    // init palfi LED pins, set all to OFF
    PALFI_LEDS_OFF();
    PALFI_LEDS_PORT->DDIR  |= PALFI_LEDS_PINS;
    //PALFI_LEDS_PORT->SEL    |= PALFI_LED_MCLK;   //LED2 on board
    
    // init palfi Bypass output, set to OFF (enables TPS62730 to power device)
    PALFI_BYPASS_PORT->DDIR |= PALFI_BYPASS_PIN;
    PALFI_BYPASS_PORT->DOUT &= ~PALFI_BYPASS_PIN;
    
    // init palfi VCLD output
    PALFI_VCLD_PORT->DDIR |= PALFI_VCLD_PIN;
     
    // Map SPI and Timer to their pins
    PMAPPWD = 0x02D52;
    PALFI_SPI_Px->PALFI_SPIMISO_MAP = PALFI_SPIMISO_SIG;
    PALFI_SPI_Px->PALFI_SPIMOSI_MAP = PALFI_SPIMOSI_SIG;
    PALFI_SPI_Px->PALFI_SPISCK_MAP  = PALFI_SPISCK_SIG;
    PALFI_TIM_Px->PALFI_TIM_MAP     = PALFI_TIM_SIG;
    PMAPPWD = 0; 

    // init Timer peripheral
    PALFI_TIM->CTL      = TACLR;
    PALFI_TIM->CCTL0    = CM_1 + SCS + CAP; // capture rising edge,synchronous, capture mode

    // init SPI peripheral (reset)
    PALFI_SPI->CTL1    |= UCSWRST;
    PALFI_SPI->CTL0     = UCMSB + UCMST + UCSYNC;
    PALFI_SPI->CTL1     = UCSSEL_2;                 // Typ SMCLK = 2.5MHz
    PALFI_SPI->BRW      = 5;                        // goal is roughly 0.5MHz
  //PALFI_SPI->BR1      = 0;
    PALFI_SPI->CTL1    &= ~UCSWRST;
}


void palfi_powerdown() {
    static const ot_u8 cmd_data[] = { 3, 0xF3, 0x41, 0x0F };
    palfi_writeout((ot_u8*)cmd_data);
    
    PALFI_SPI->CTL1        |= UCSWRST;
    PALFI_WAKE_PORT->IFG   &= ~PALFI_WAKE_PIN;
    PALFI_WAKE_PORT->IE    |= PALFI_WAKE_PIN;
    palfi.wake_event        = 0;
    sys_task_setevent(PALFI_TASK, 0);
}


void palfi_spi_startup() {
    // configure P3.1,P3.2,P3.3 as normal Port pins
    // this is necessary as enabling of USCI state machine
    // can cause the RAIDAES to hang up
    PALFI_WAKE_PORT->IE    &= ~PALFI_WAKE_PIN;
    PALFI_SPI_PORT->SEL    &= ~PALFI_SPI_PINS;
    PALFI_SPI->CTL1        &= ~UCSWRST;
    PALFI_SPI_PORT->SEL    |= PALFI_SPI_PINS;
}


void palfi_writeout(ot_u8* src) {
    ot_int size = *src;
    while (size >= 0) {
        sub_spi_trx(*src++);
        size--;
    }
}


void palfi_readback(ot_u8* dst, ot_int size) {
/// Read-back results using dummy SPI writes
    ot_u8*  endptr;
    endptr  = dst + size;
        
    while (dst != endptr) {
        *dst++ = sub_spi_trx(0x00);
    }
}


void palfi_raidctrl(ot_u8 cmd) {
    ot_u8   cmd_data[]  = { 0x03, 0xF3, 0x41, 0x00 };
    cmd_data[3]         = cmd;
    
    palfi_writeout(cmd_data);
}


ot_bool palfi_readbank7(ot_u8 page) {
    ot_u8 cmd_data[] = { 2, 0xF0, 0 };
    
    cmd_data[2] = page;
    palfi_writeout(cmd_data);
    palfi_readback(palfi.rxdata, 7);
    
    return (ot_bool)(   (palfi.rxdata[5] == 0xf1) && \
                        (palfi.rxdata[6] == page)   );
}


void palfi_cmdrssi() {
/// Typically used in Event Slots 9 & 10, for both normal and trimming modes
    /// @note The "RSSI Method" field takes values 0, 1, or 2.  Value=0 causes 
    /// RSSI sampling to occur between SOF and EOF.  Value=1 takes an immediate
    /// sample.  Value=2 takes an RSSI sample during the EOF.
    static const ot_u8 cmd_data[] = { 0x03, 0xF3, 0x44, 0x00 };
    
    palfi_writeout((ot_u8*)cmd_data);
    
    palfi_readback(&palfi.rssi_info, 6);
//  This block does the same as the loop above, but is more descriptive
//  palfi.rssi_info   = sub_spi_trx(0x00);  // if EOBA=stable=RSSI OK 0x01
//  palfi.rssi1     = sub_spi_trx(0x00);  // RSSI value of RF1
//  palfi.rssi2     = sub_spi_trx(0x00);  // RSSI value of RF2
//  palfi.rssi3     = sub_spi_trx(0x00);  // RSSI value of RF3
//  palfi.read_addr = sub_spi_trx(0x00);  // read address = 0x44
//  palfi.read_ext  = sub_spi_trx(0x00);  // read address extension = 0x01
}


void palfi_cmdstatus() {
/// Grab the command status from the PaLFi core
    sub_spi_trx(0x00);
    palfi_readback(palfi.status, 4);

//  This block does the same as the loop above, but is more descriptive
//  sub_spi_trx(0x00);                        // command: 0x00
//  palfi.status[0] = sub_spi_trx(0x00);      // device status
//  palfi.status[1] = sub_spi_trx(0x00);      // LF passive mode status
//  palfi.status[2] = sub_spi_trx(0x00);      // switch status
//  palfi.status[3] = sub_spi_trx(0x00);      // LF passive mode data
}


void palfi_cmdcrc(ot_u8 length) {
#define CRC_Start_Low   0x91
#define CRC_Start_High  0x37 
    ot_u8 cmd_data[] = { 5, 0xf3, 0x45, 0, CRC_Start_Low, CRC_Start_High };
    
    cmd_data[0] += length;
    palfi_writeout(cmd_data);
    {
        ot_u8 i;
        for (i=0; i<length; i++) { sub_spi_trx(palfiext.crcdata[i]); }
    }
    palfi_readback(palfiext.crcresult, 2);
    palfi_readback(palfi.rxdata, 2);
}




ot_u8 sub_spi_trx(ot_u8 write) {
/** @note This is the major remaining part of the PaLFi app that is blocking.
  * The PaLFi Core is slow, and the trx process can take 10-30ms.  So, the
  * MCLK frequency is divided by 32 during this sequence to conserve power.
  * Eventually, it might be nice to make it non-blocking, and have the CC430
  * sleep during the read/write cycle, but that would require saving a lot of
  * interrupt configurations, or finding a hack to make the BUSY signal a 
  * Non-Masking Interrupt (NMI).
  */
    //ot_u16 saved_ucsctl5;
    
    //saved_ucsctl5   = UCS->CTL5;
    //UCS->CTL5      |= 7;                //maximum MCLK div (32)

    while (PALFI_BUSY_PORT->DIN & PALFI_BUSY_PIN) {
        PALFI_SPICS_PORT->DOUT |= PALFI_SPICS_PIN;
    }
    PALFI_SPICS_PORT->DOUT &= ~PALFI_SPICS_PIN;
    
    while ((PALFI_SPI->IFG & UCTXIFG) == 0);
    
    PALFI_SPI->TXBUF = write;
    while ((PALFI_SPI->IFG & UCRXIFG) == 0);
    
    //UCS->CTL5 = saved_ucsctl5;
    return PALFI_SPI->RXBUF;
}







/** SPI Trimming Action sequence  <BR>
  * ========================================================================
  */

ot_int palfi_action_spitrim_0(void) {
    PALFI_BYPASS_PORT->DOUT    |= PALFI_BYPASS_PIN;
    PALFI_VCLD_PORT->DOUT      |= PALFI_VCLD_PIN;
    palfi.channel               = 1;
    return palfi_action_spitrim_1();
}


ot_int palfi_action_spitrim_1(void) {
	palfi.action = &palfi_action_spitrim_2;
    sub_prog_trimswitch(0);
    return 5;  //wait ~4 ms
}


ot_int palfi_action_spitrim_2(void) {
/// init frequency measurement with trim switches all programmed to off
	palfi.action = &palfi_action_spitrim_3;
    return sub_measurefreq_init(0);
}


ot_int palfi_action_spitrim_3(void) {
	palfi.action = &palfi_action_spitrim_4;
    sub_measurefreq_finish(&palfi.trim.tlow[palfi.channel]);
    return 5;  // wait ~4 ms
}


ot_int palfi_action_spitrim_4(void) {
/// measure frequency with trim switches all programmed to on
	palfi.action = &palfi_action_spitrim_5;
    return sub_measurefreq_init(0x7f);
}


ot_int palfi_action_spitrim_5(void) {
    sub_measurefreq_finish(&palfi.trim.thigh[palfi.channel]);
    sub_calculate_trim();
    sub_prog_trimswitch(palfi.trimval[palfi.channel]);
    
    // Finish-up if trimming has been done on all 3 channels
    if (palfi.channel == 3) {
        PALFI_VCLD_PORT->DOUT      &= ~PALFI_VCLD_PIN;      // disable VCL charging
        PALFI_BYPASS_PORT->DOUT    &= ~PALFI_BYPASS_PIN;    // enable DC/DC converter   
        return 0;
    }
    
    palfi.channel++;
    palfi.action  = &palfi_action_spitrim_1;
    return -1;
}







/** Switch Trimming Action sequence  <BR>
  * ========================================================================
  */
ot_int palfi_action_swtrim_0(void) {
	palfi.action = &palfi_action_swtrim_1;

    PALFI_BYPASS_PORT->DOUT    |= PALFI_BYPASS_PIN;
    PALFI_VCLD_PORT->DOUT      |= PALFI_VCLD_PIN;
    
    for (palfi.channel=0; palfi.channel<3; ) {
        palfi.channel++;
        sub_prog_trimswitch(0);
    }
    
    PALFI_LED3_ON();

    return 52;  // wait ~50 ms
}


ot_int palfi_action_swtrim_1(void) {
    PALFI_LED3_OFF();
    PALFI_VCLD_PORT->DOUT      &= ~PALFI_VCLD_PIN;      // disable VCL charging
    PALFI_BYPASS_PORT->DOUT    &= ~PALFI_BYPASS_PIN;    // enable DC/DC converter   
    return -1;
}







/** Action subroutines  <BR>
  * ========================================================================<BR>
  */
void sub_program_channels(palfi_CHAN channel, ot_u8 trim_val, ot_u8 base_val) {
    ot_u8 cmd_data[7]   = { 6, 0xF3, 0x02, 0, 0, 0, 0 };
    
    trim_val           &= ~0x80;  //clear MSB to prevent trim value from locking
    cmd_data[3]         = base_val + (ot_u8)channel;
    cmd_data[3]        += (channel == CH3);     //CH3 needs +1 offset
    cmd_data[3+channel] = trim_val;             // put trim val in selected chan only
    
    palfi_writeout(cmd_data);
}


void sub_prog_trimswitch(ot_s8 trim_val) {
    sub_program_channels((palfi_CHAN)palfi.channel, (ot_u8)trim_val, 0x38);
    palfi_readback(palfi.rxdata, 8);
}


ot_int sub_measurefreq_init(ot_u8 trim_val) {
    /// Prepare the measurement timer
    PALFI_TIM->CTL      = TACLR;                // reset Timer 0
    PALFI_TIM->CTL      = TASSEL_2 + MC_2;      // SMCLK, continuous
    PALFI_TIM->CCTL0   &= ~CCIFG;               // reset TIMER A0 4 CCIFG flag

    palfi.trim.startcount   = 20;
    palfi.trim.endcount     = 160;

    /// Prepare the channel for measurement
    sub_program_channels((palfi_CHAN)palfi.channel, trim_val, 0x88);
    
    /// Set this up as a non-blocking external process.  The timer interrupt  
    /// will pre-empt the kernel and cancel the timeout, if everything goes well  
    return 1024;  // task-timeout watchdog = ~1000ms
}


void sub_measurefreq_finish(float* t_pulse) {
    PALFI_TIM->CTL = TACLR;

    if (PALFI_TASK->nextevent <= 0) {
        //Watchdog Timeout, cancel the process
    	sys_task_setevent(PALFI_TASK, 0);
    }
    else {
        ot_uint num_periods;
        palfi_readback(palfi.rxdata, 8);
        
        num_periods     = palfi.trim.endcount - palfi.trim.startcount;
        num_periods    *= (PLATFORM_HSCLOCK_HZ/PLATFORM_SMCLK_DIV);             // Typ ~2.5 MHz
        *t_pulse        = (float)(palfi.trim.endval - palfi.trim.startval);
        *t_pulse       /= num_periods;
        *t_pulse       *= 1000;
    }
}


void sub_calculate_trim() {
    float tl2, th2;
    
    tl2     = (palfi.trim.tlow[palfi.channel] * palfi.trim.tlow[palfi.channel]);
    th2     = (palfi.trim.thigh[palfi.channel] * palfi.trim.thigh[palfi.channel]);
    th2     = ( (((7452.0f*7452.0f)/tl2)-1.0f) / ((th2/tl2)-1.0f) );
    th2    *= 127;
    
    palfi.trimval[palfi.channel] = (ot_s8)th2;
}



