/*  Copyright (c) 2009, Texas Instruments Incorporated
  * All rights reserved.
  * 
  * Redistribution and use in source and binary forms, with or without
  * modification, are permitted provided that the following conditions are met:
  *  * Redistributions of source code must retain the above copyright notice,
  *    this list of conditions, and the following disclaimer.
  *  * Redistributions in binary form must reproduce the above copyright 
  *    notice, this list of conditions and the following disclaimer in the
  *    documentation and/or other materials provided with the distribution.
  *  * Neither the name of the organization, Texas Instruments, nor the names 
  *    of its contributors may be used to endorse or promote products derived 
  *    from this software without specific prior written permission.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
  * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
  * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
  * ARE DISCLAIMED. IN NO EVENT SHALL TEXAS INSTRUMENTS BE LIABLE FOR ANY 
  * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF 
  * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  */
/**
  * @file       /otplatform/msp430f5/usb_cdc_driver/usb_isr.c
  * @author     RSTO, JP Norair
  * @version    R100
  * @date       1 Nov 2012
  * @brief      USB Module ISR Implementation
  * @ingroup    MSP430F5 USB CDC
  *
  * This module is a derivative work of TI's USB library file, UsbIsr.c.
  * JP has integrated it into OpenTag and heavily optimized the code for a
  * CDC-only USB system.
  * 
  * The most notable change from the stock library is that the OpenTag-targeted
  * ISR subsystem uses the platform_isr_...() methodology of OpenTag.  Also,
  * the USB interrupts will NEVER cause the MSP430 to exit low-power mode after
  * the interrupt exits.  USB interrupts that result in ISRs which, in turn,
  * result in conditions that should affect the system ... should use system
  * calls.  This is the only way to make sure that a task does not break the 
  * system.  Otherwise, the OpenTag exokernel does not have many demands on how
  * you like to architect an ISR.
  *
  * <PRE>
  * Author: RSTO
  * Source: UsbIsr.c, File Version 1.00 2009/12/03   
  *
  * WHO         WHEN        WHAT
  * ---         ----------  ------------------------------------------------
  * RSTO        2008/09/03  born
  * RSTO        2008/12/23  enhancements of CDC API
  * ---         ----------  ------------------------------------------------
  * JPN         2012/05/01  Integrated with OpenTag 0.3
  * JPN         2012/11/01  Integrated with OpenTag 0.4
  * </PRE>                           
  *****************************************************************************/

#include "OT_platform.h"

#include "usb_cdc_driver/usb_descriptors.h"
#include "usb_cdc_driver/defMSP430USB.h"
#include "usb_cdc_driver/usb_isr.h"
#include "usb_cdc_driver/usb_main.h"
#include "usb_cdc_driver/usb_cdc_backend.h"

#ifdef _CDC_

/** USB Interrupt Service Routines (ISRs)   <BR>
  * ========================================================================<BR>
  */


void PWRVBUSonHandler(void) {
    volatile unsigned int i;
    for (i =0; i < USB_MCLK_FREQ/1000*1/10; i++);          // waiting till voltage will be stable (1ms delay)
    USBKEYPID =  0x9628;                // set KEY and PID to 0x9628 -> access to configuration registers enabled
    USBPWRCTL |= VBOFFIE;               // enable interrupt VBUSoff
    USBPWRCTL &= ~ (VBONIFG + VBOFFIFG);             // clean int flag (bouncing)
    USBKEYPID =  0x9600;                // access to configuration registers disabled
}


void PWRVBUSoffHandler(void) {
	volatile unsigned int i;
    for (i =0; i < USB_MCLK_FREQ/1000*1/10; i++); // 1ms delay
    if (!(USBPWRCTL & USBBGVBV)) {
    	USBKEYPID   =    0x9628;        // set KEY and PID to 0x9628 -> access to configuration registers enabled
        bEnumerationStatus = 0x00;      // device is not enumerated
    	bFunctionSuspended = False;     // device is not suspended
    	USBCNF     =    0;              // disable USB module
    	USBPLLCTL  &=  ~UPLLEN;         // disable PLL
    	USBPWRCTL &= ~(VBOFFIE + VBOFFIFG + SLDOEN);          // disable interrupt VBUSoff
    	USBKEYPID   =    0x9600;        // access to configuration registers disabled
    }
}



void IEP0InterruptHandler(void) {
    USBCTL |= FRSTE;                              // Function Reset Connection Enable
    tEndPoint0DescriptorBlock.bOEPBCNT = 0x00;     
    if(bStatusAction == STATUS_ACTION_DATA_IN) {
	    usbSendNextPacketOnIEP0();
    }
    else {
        tEndPoint0DescriptorBlock.bIEPCNFG |= EPCNF_STALL; // no more data
    }
}



void OEP0InterruptHandler(void) {
//ot_u8 OEP0InterruptHandler(void) {
    //ot_u8 bWakeUp = False;
    USBCTL |= FRSTE;                              // Function Reset Connection Enable
    tEndPoint0DescriptorBlock.bIEPBCNT = 0x00;    
    if (bStatusAction == STATUS_ACTION_DATA_OUT) {
        usbReceiveNextPacketOnOEP0();
        if (bStatusAction == STATUS_ACTION_NOTHING) {
            if (tSetupPacket.bRequest == USB_CDC_SET_LINE_CODING) {
                //bWakeUp = Handler_SetLineCoding();
                Handler_SetLineCoding();
            }
      	}
    }
    else {
	    tEndPoint0DescriptorBlock.bOEPCNFG |= EPCNF_STALL; // no more data
    }
    //return (bWakeUp);
}





ot_u8 SetupPacketInterruptHandler(void) {
    ot_u8 bTemp;
    ot_u8 bWakeUp = False;
    USBCTL |= FRSTE;      // Function Reset Connection Enable - set enable after first setup packet was received

    //usbProcessNewSetupPacket:
    SetupPacketInterruptHandler_newpacket:
    
    // copy the MSB of bmRequestType to DIR bit of USBCTL
    if ((tSetupPacket.bmRequestType & USB_REQ_TYPE_INPUT)) {
    	USBCTL |= DIR;
    }
    else {
    	USBCTL &= ~DIR;
    }

    //{
    //	ot_u16 dir_bit;
    //	dir_bit = (tSetupPacket.bmRequestType & USB_REQ_TYPE_INPUT) >> 7;
    //	USBCTL |= dir_bit;
    //	USBCTL &= (dir_bit-2);
    //}
    

    bStatusAction = STATUS_ACTION_NOTHING;
    // clear out return data buffer
    for (bTemp=0; bTemp<USB_RETURN_DATA_LENGTH; bTemp++) {
    	abUsbRequestReturnData[bTemp] = 0x00;
    }
    // decode and process the request
    bWakeUp = usbDecodeAndProcessUsbRequest();
    
    // check if there is another setup packet pending
    // if it is, abandon current one by NAKing both data endpoint 0
    if((USBIFG & STPOWIFG) != 0x00) {
    	USBIFG &= ~(STPOWIFG | SETUPIFG);
    	goto SetupPacketInterruptHandler_newpacket;
    }
    
    return bWakeUp;
}






ot_u8 platform_isr_usb (void) {
/// Check if the setup interrupt is pending.  We need to check it before other 
/// interrupts, to work around that the Setup Int has lower priority then Input 
/// Endpoint 0

    ot_u8 bWakeUp = FALSE;
    
    if (USBIFG & SETUPIFG) {
        bWakeUp = SetupPacketInterruptHandler();
        USBIFG &= ~SETUPIFG;    // clear the interrupt bit
    }
    
    switch (__even_in_range(USBVECINT & 0x3f, USBVECINT_OUTPUT_ENDPOINT7)) {
        case USBVECINT_NONE:        break;

        case USBVECINT_PWR_DROP:    //__no_operation();
                                    break;
                                    
        case USBVECINT_PLL_LOCK:    break;
        case USBVECINT_PLL_SIGNAL:  break;
        
#		if (USBEVT_MASK & USBEVT_CLOCKFAULT)
        case USBVECINT_PLL_RANGE:   USB_handleClockEvent();
                                    break;
#		endif
        
        case USBVECINT_PWR_VBUSOn:  PWRVBUSonHandler();
#									if (USBEVT_MASK & USBEVT_VBUSON)
                                    USB_handleVbusOnEvent();
#									endif
                                    break;
        
        case USBVECINT_PWR_VBUSOff: PWRVBUSoffHandler();
#									if (USBEVT_MASK & USBEVT_VBUSOFF)
                                    USB_handleVbusOffEvent();
#									endif
                                    break;
        
        case USBVECINT_USB_TIMESTAMP: break;
        
        case USBVECINT_INPUT_ENDPOINT0: IEP0InterruptHandler();
                                        break;
                                        
        case USBVECINT_OUTPUT_ENDPOINT0: OEP0InterruptHandler();
                                        break;
        
        case USBVECINT_RSTR:        USB_reset();
#									if (USBEVT_MASK & USBEVT_RESET)
                                    USB_handleResetEvent();
#									endif
                                    break;
        
        case USBVECINT_SUSR:        USB_suspend();
#									if (USBEVT_MASK & USBEVT_SUSPEND)
                                    USB_handleSuspendEvent();
#									endif
                                    break;
        
        case USBVECINT_RESR:        USB_resume();
#									if (USBEVT_MASK & USBEVT_RESUME)
                                    USB_handleResumeEvent();
#									endif
                                    //bWakeUp = TRUE;		//Always wake on resume
                                    break;
        
        case USBVECINT_SETUP_PACKET_RECEIVED:   // NAK both IEP and OEP enpoints
                                    tEndPoint0DescriptorBlock.bIEPBCNT = EPBCNT_NAK;
                                    tEndPoint0DescriptorBlock.bOEPBCNT = EPBCNT_NAK;
                                    bWakeUp = SetupPacketInterruptHandler();
                                    break;
        
        case USBVECINT_STPOW_PACKET_RECEIVED: break;
        
        case USBVECINT_INPUT_ENDPOINT1: break;

        case USBVECINT_INPUT_ENDPOINT2: //bWakeUp = CdcToHostFromBuffer(CDC0_INTFNUM);
                                        CdcToHostFromBuffer(CDC0_INTFNUM);
        	                            break;
                                    
        case USBVECINT_INPUT_ENDPOINT3: break;
        case USBVECINT_INPUT_ENDPOINT4: break;
        case USBVECINT_INPUT_ENDPOINT5: break;
        case USBVECINT_INPUT_ENDPOINT6: break;
        case USBVECINT_INPUT_ENDPOINT7: break;

        case USBVECINT_OUTPUT_ENDPOINT1: break;

        case USBVECINT_OUTPUT_ENDPOINT2: //bWakeUp = CdcToBufferFromHost(CDC0_INTFNUM);
                                        CdcToBufferFromHost(CDC0_INTFNUM);
            						    break;
            
        case USBVECINT_OUTPUT_ENDPOINT3: break;
        case USBVECINT_OUTPUT_ENDPOINT4: break;
        case USBVECINT_OUTPUT_ENDPOINT5: break;
        case USBVECINT_OUTPUT_ENDPOINT6: break;
        case USBVECINT_OUTPUT_ENDPOINT7: break;
        
        default: break;
    }
    
    return bWakeUp;
}

#endif
