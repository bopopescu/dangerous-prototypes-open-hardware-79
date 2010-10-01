/*
*
*	USB infrared remote control receiver transmitter firmware v1.0
*	License: creative commons - attribution, share-alike 
*	Copyright Ian Lesnet 2010
*	http://dangerousprototypes.com
*
*/
//
//	IR sampler
//
// This new mode counts the duration of each IR pulse/space
// the signal is returned as 16bit timer counts that each represent 21.3us
// the values are returned Pulse-h P-l Blank-h B-l P-h P-l etc.
// the last value will be 0xff 0xff, representing 1.7seconds of blank


//This is a hacked copy of the IRIO source
// it still contains a lot of variables and cruft that's not needed
//to do:
//sampling frequency adjust
//timeout adjust
//LED on/off 

#include "globals.h"

#define IRS_TRANSMIT_HI	0
#define IRS_TRANSMIT_LO	1

extern struct _irtoy irToy;

static unsigned char h,l,tmr0_buf[2];
static unsigned char modFreq[8];
static unsigned char modFreqCnt;

void cleanup(void);

static struct{
	unsigned char RXsamples;
	unsigned char TXsamples;
	unsigned char timeout;
	unsigned char TX:1;
	unsigned char rxflag:1;
	unsigned char txflag:1;
	unsigned char flushflag:1;
	unsigned char overflow:1;
	unsigned char TXInvert:1;
	unsigned char TXend:1;
	unsigned char bigendian:1;
} irS;

void irsSetup(void){

	//send version string
  	if( mUSBUSARTIsTxTrfReady() ){ //it's always ready, but this could be done better
		irToy.usbOut[0]='S';//answer OK
		irToy.usbOut[1]='0';
		irToy.usbOut[2]='1';
		putUnsignedCharArrayUsbUsart(irToy.usbOut,3);
	}

	//setup for IR TX
	/*
	 * PWM registers configuration
	 * Fosc = 48000000 Hz
	 * Fpwm = 36144.58 Hz (Requested : 36000 Hz)
	 * Duty Cycle = 50 %
	 * Resolution is 10 bits
	 * Prescaler is 4
	 * Ensure that your PWM pin is configured as digital output
	 * see more details on http://www.micro-examples.com/
	 * this source code is provided 'as is',
	 * use it at your own risks
	 * http://www.micro-examples.com/public/microex-navig/doc/097-pwm-calculator
	 */
	IRTX_TRIS&=(~IRTX_PIN);//digital output
	IRTX_LAT&=(~IRTX_PIN); //low (hold PWM transistor base off)
	T2IF=0;//clear the interrupt flag
	T2IE=0; //disable interrupts
	PR2 = 0b01010010 ; //82
	T2CON = 0b00000101 ;
	CCPR1L = 0b00101001 ;	//upper 8 bits of duty cycte
	CCP1CON = 0b00010000 ; //should be cleared on exit! (5-4 two LSB of duty, 3-0 set PWM)

	IRFREQ_PIN_SETUP();
						
	//setup for IR RX
	irS.rxflag=0;
	irS.txflag=0;
	irS.flushflag=0;
	irS.timeout=0;
	irS.RXsamples=0;
	irS.TXsamples=0;
	irS.TX=0;
	irS.overflow=0;
	irS.bigendian=0;

	//setup timer 0
	T0CON=0;
	//configure prescaler
	//bit 2-0 T0PS2:T0PS0: Timer0 Prescaler Select bits
	//111 = 1:256 Prescale value
	//110 = 1:128 Prescale value
	//101 = 1:64 Prescale value
	//100 = 1:32 Prescale value
	//011 = 1:16 Prescale value
	//010 = 1:8 Prescale value
	//001 = 1:4 Prescale value
	//000 = 1:2 Prescale value
	T0CON=0b111;
	//T0CONbits.T08BIT=1; //16bit mode
	//internal clock
	//low to high
	T0CONbits.PSA=0; //1=not assigned

	//timer 1 as USB packet send timeout
	T1CON=0;

	IRRX_IE = 1;  //IR RX interrupt on
	IRRX_IF = 0;  //IR RX interrupt on
}


typedef enum  { //in out data state machine
	I_IDLE = 0,
	I_PARAMETERS,
	I_PROCESS,
	I_DATA_L,
	I_DATA_H
} _smio;

typedef struct _smCommand {
	unsigned char command[5];
	unsigned char parameters;
	unsigned char parCnt;
} _smCommand;

//
// COMMANDS
//
#define IRIO_RESET 		0x00
//1 and 2 reserved for SUMP RUN and ID
#define IRIO_TRANSMIT 	0x03
#define IRIO_FREQ 		0x04
#define IRIO_SETUP_SAMPLETIMER 0x05
#define IRIO_SETUP_PWM 	0x06
#define IRIO_LEDMUTEON 	0x10
#define IRIO_LEDMUTEOFF 0x11
#define IRIO_LEDON 		0x12
#define IRIO_LEDOFF 	0x13
#define IRIO_LITTLEENDIAN 0x20
#define IRIO_BIGENDIAN 	0x21
//
// irIO periodic service routine
// moves bytes in and out
//
unsigned char irsService(void){
	static unsigned char TxBuffCtr;
	static unsigned int tmr16;
	static _smio irIOstate = I_IDLE;

	static _smCommand irIOcommand;

	if(irS.TXsamples==0){
		irS.TXsamples=getUnsignedCharArrayUsbUart(irToy.s,64);
		TxBuffCtr=0;
	}

	if(irS.TXsamples>0){
		switch (irIOstate){
			case I_IDLE:

					switch(irToy.s[TxBuffCtr]){
						case IRIO_RESET: //reset, return to RC5 (same as SUMP) 
							cleanup();
							LedOff();
							LedOut();
							return 1; //need to flag exit!
							break;
						case IRIO_TRANSMIT: //start transmitting
							//setup for transmit mode:
								//disable timer 0, 1, 2, etc
							TM0IE=0; 
							T1IE=0;
							IRRX_IE = 0;
							irS.TXend=0;
							irS.TX=0;
								//setup the PWM pin, frequency etc.
								//setup timer 0
							irS.txflag=0;//transmit flag =0 reset the transmit flag
							irIOstate = I_DATA_H; //change to transmit data processing state 
							break;
						case IRIO_FREQ:
							if((irToy.HardwareVersion>1)&&(mUSBUSARTIsTxTrfReady())){ //if we have full buffer, or end of capture flush
								modFreq[7]=TMR3L;
								modFreq[6]=TMR3H;
								putUnsignedCharArrayUsbUsart(modFreq,8);//send current buffer to USB
							}							
							break;
						case IRIO_LEDMUTEON:
							LedIn();
							break;
						case IRIO_LEDMUTEOFF:
							LedOut();
							break;
						case IRIO_LEDON:
							LedOn();
							break;
						case IRIO_LEDOFF:
							LedOff();
							break;
						case IRIO_LITTLEENDIAN:
							irS.bigendian=0;
							break;
						case IRIO_BIGENDIAN:
							irS.bigendian=1;
							break;
						case IRIO_SETUP_PWM: //setup PWM frequency
							irIOcommand.command[0]=irToy.s[TxBuffCtr];
							irIOcommand.parameters=2;
							irIOstate=I_PARAMETERS;
							break;
						case IRIO_SETUP_SAMPLETIMER: //setup sample timer prescaler
							irIOcommand.command[0]=irToy.s[TxBuffCtr];
							irIOcommand.parameters=1;
							irIOstate=I_PARAMETERS;
							break;
						default:
							break;
					}
					irS.TXsamples--;
					TxBuffCtr++;
				break;
			case I_PARAMETERS://get optional parameters
				irIOcommand.command[irIOcommand.parCnt]=irToy.s[TxBuffCtr];//store each parameter
				irS.TXsamples--;
				TxBuffCtr++;
				irIOcommand.parCnt++;
				if(irIOcommand.parCnt<irIOcommand.parameters) break; //if not all parameters, quit
			case I_PROCESS:	//process long commands
				switch(irIOcommand.command[0]){
					case IRIO_SETUP_PWM: //setup user defined PWM frequency
						T2CON = 0;
						PR2 = irIOcommand.command[1];//user period
						CCPR1L =(irIOcommand.command[1]>>1);//upper 8 bits of duty cycle, 50% of period by binary division
						if((irIOcommand.command[1]& 0b1)!=0)//if LSB is set, set bit 1 in CCP1CON
							CCP1CON = 0b00101100 ; //5-4 two LSB of duty, 3-0 set PWM
						else
							CCP1CON = 0b00001100 ; //5-4 two LSB of duty, 3-0 set PWM

						T2CON = 0b00000101; //enable timer again, 4x prescaler				
						break;
					case IRIO_SETUP_SAMPLETIMER:	
						T0CON=0;	//setup timer 0
						IRRX_IE=0;  //IR RX interrupt off
						//configure prescaler
						//bit 2-0 T0PS2:T0PS0: Timer0 Prescaler Select bits
						T0CON|=(irIOcommand.command[1]&0b111); //set new prescaler bits
						IRRX_IF = 0;  //IR RX interrupt on
						IRRX_IE = 1;  //IR RX interrupt on
						IRRX_IF = 0;  //IR RX interrupt on
						break;
				}
				irIOstate=I_IDLE;//return to idle state
				break;	
			case I_DATA_H://hang out here and process data
				if(irS.txflag==0){//if there is a free spot in the interrupt buffer
					tmr0_buf[0]=irToy.s[TxBuffCtr];//put the first byte in the buffer
					irIOstate = I_DATA_L; //advance and get the next byte on the next pass
					irS.TXsamples--;
					TxBuffCtr++;
				}
				break;
			case I_DATA_L:
				if(irS.bigendian==1){ //switch byte order for bigendian mode
					tmr0_buf[1]=tmr0_buf[0];
					tmr0_buf[0]=irToy.s[TxBuffCtr];//put the second byte in the buffer
				}else{
					tmr0_buf[1]=irToy.s[TxBuffCtr];//put the second byte in the buffer
				}

				//check here for 0xff 0xff and return to IDLE state
				if((tmr0_buf[0]==0xff) && (tmr0_buf[1]==0xff)){
					//irIO.TXInvert=IRS_TRANSMIT_LO;
					//irIO.TXend=1;	
					tmr0_buf[0]=0;				
					tmr0_buf[1]=20;
					irIOstate=I_IDLE;//return to idle state, data done
				}else{
					irIOstate = I_DATA_H; //advance and get the next byte on the next pass
				}	

				//prepare the values for the timer
				//subtract from 0x10000 (or 0x0000)
				//this would be a lot nicer with a little pointer magic
				//but for now we dance (test)
				tmr16=tmr0_buf[0];
				tmr16<<=8;
				tmr16|=tmr0_buf[1];

				tmr16=0x0000-tmr16;

				tmr0_buf[1]=(tmr16 & 0xff);
				tmr0_buf[0]=((tmr16>>8)&0xff);		

				irS.txflag=1;//reset the interrupt buffer full flag		

				if(irS.TX==0){//enable interrupt if this is the first time
					TMR0H=tmr0_buf[0];//first set the high byte
					TMR0L=tmr0_buf[1];//set low byte copies high byte too
					TM0IF=0;
					TM0IE=1;
					TM0ON=1;//enable the timer

					//enable the PWM
					TMR2=0;
					CCP1CON |=0b1100; 
					irS.TXInvert=IRS_TRANSMIT_LO;

					irS.txflag=0; //buffer ready for new byte
					irS.TX=1;
					LedOn();
				}

				irS.TXsamples--;
				TxBuffCtr++;			

				break;

		}//switch 
	}

	if(irS.overflow==0){
		//service the inbound samples here
		//keep in 64 byte buffer then send to USB for max sample rate
		if(irS.rxflag==1){ //a RX byte is in the buffer
			if(irS.RXsamples<=62){ //if we have room in the USB send buffer
				if(irS.bigendian==1){
					irToy.usbOut[irS.RXsamples]=l; //add to USB send buffer
					irS.RXsamples++;
					irToy.usbOut[irS.RXsamples]=h; //add to USB send buffer
				}else{
					irToy.usbOut[irS.RXsamples]=h; //add to USB send buffer
					irS.RXsamples++;
					irToy.usbOut[irS.RXsamples]=l; //add to USB send buffer
				}
				irS.RXsamples++;
				irS.rxflag=0;				//reset the flag
			//removed this check. we might hit this several times while before the send can clear
			//that can be perfectly fine, as long as it happens before we need to move the next samples in
			//the new overflow check in interrupt routine will also catch this error
			//}else{//underrun error, no more room!
			//	irIO.overflow=1;
			//	LED_TRIS |= LED_PIN; //error, LED off by making it input 
			}
		}

		//if the buffer is full, send it to USB
		if( ( (irS.RXsamples==64) || (irS.flushflag==1) ) && (mUSBUSARTIsTxTrfReady()) ){ //if we have full buffer, or end of capture flush
			putUnsignedCharArrayUsbUsart(irToy.usbOut,irS.RXsamples);//send current buffer to USB
			irS.RXsamples=0;
			irS.flushflag=0;
		}
	}else{//overflow error
		//on overflow we loop until we can send 6 0xff then reset everything
		if(mUSBUSARTIsTxTrfReady()){
			irToy.usbOut[0]=0xff; //add to USB send buffer
			irToy.usbOut[1]=0xff; //add to USB send buffer
			irToy.usbOut[2]=0xff; //add to USB send buffer
			irToy.usbOut[3]=0xff; //add to USB send buffer
			irToy.usbOut[4]=0xff; //add to USB send buffer
			irToy.usbOut[5]=0xff; //add to USB send buffer
			irS.RXsamples=6;
			putUnsignedCharArrayUsbUsart(irToy.usbOut,irS.RXsamples);//send current buffer to USB

			T1ON=0;		//t1 is the usb packet timeout, disable it
			TM0ON=0; //timer0 off
			TM0IF=0;
			//reset the pin interrupt, just in case
			IRRX_IE=1;
			IRRX_IF=0;

			irS.RXsamples=0;
			irS.flushflag=0;
			irS.overflow=0;

			//LED_LAT &=(~LED_PIN); //LED off
			LedOff();
		}
	}
	return 0;//CONTINUE
}

void cleanup(void){
	CCP1CON=0; 
	TM0ON=0;
	TM0IE=0;
	T0CON=0;
	T2ON=0; 
	T3CON=0;
	IRFREQ_CCPxCON=0; 
	T1ON=0; 
	T1IE=0;
	IRRX_IE = 0;
	IRTX_TRIS|=IRTX_PIN;//digital INPUT (no PWM until active)
	IRTX_LAT&=(~IRTX_PIN);//direction 0
}

//the first falling edge starts timer0
//the next pin interrupt copies the timer0 value to a buffer and resets timer0
//if timer0 interrupts, then timeout and end.
//high priority interrupt routine
#pragma interrupt irsInterruptHandlerHigh
void irsInterruptHandlerHigh (void){
	static unsigned int lastCap, newCap;

	if(IRRX_IE==1 && IRRX_IF == 1){ //if RB Port Change Interrupt	
		l=IRRX_PORT;
		if(TM0ON==0){ //timer not running, setup and start
			if( ((l & IRRX_PIN)==0)){;//only if 0, must read PORTB to clear RBIF
				//LED_LAT |= LED_PIN;//LED ON
				LedOn();
				TMR0H=0;//first set the high byte
				TMR0L=0;//set low byte copies high byte too
				TM0IE=1;
				TM0IF=0;
				TM0ON=1;//enable the timer
				
				TMR1H=0;
				TMR1L=0;
				T1IF=0;		//clear the interrupt flag
				T1IE=1; 	//able interrupts...
				T1ON=1;		//timer on
				
				//setup the period capture module to measure the raw IR waveform period
				if(irToy.HardwareVersion>1){
					//capture module measures raw IR frequency
					IRFREQ_CAP_SETUP();		
					IRFREQ_CAP_IF=0;
					IRFREQ_CAP_IE=1;

					//frequency detector is also connected to TMR3 external clock
					//here we setup the 16bit counter to count the total number of clocks in the signal
					T3CON=0b00000110;
					TMR3H=0;
					TMR3L=1; //first tick is rising edge after next falling edge
					T3CON|=0b1; //turn it on
					
					modFreqCnt=0;//used to ignore the first measurement and get the more accurate second measurement
				}
			}

		}else{//timer running, save value and reset
			//the goal is to reset the timer as quickly as possible
			//later we can fine tune the start value to compensate for the lost cycles
			TM0ON=0;//disable the timer
			l=TMR0L;//read low byte, puts high byte in H
			h=TMR0H; //read high byte
			TMR0H=0;//first set the high byte
			TMR0L=0;//set low byte copies high byte too
			TM0IF=0;
			TM0ON=1;//enable the timer

			//reset timer1, USB packet send timeout
			T1ON=0;		//timer on
			TMR1H=0;
			TMR1L=0;
			T1ON=1;		//timer on
			
			if(irS.rxflag==0){//check if data is pending
				irS.rxflag=1;
			}else{//error, overflow
				irS.overflow=1;
			}
		}
		//clear portb interrupt		
    	IRRX_IF=0;    //Reset the RB Port Change Interrupt Flag bit  
		
	}else if(IRFREQ_CAP_IF==1 && IRFREQ_CAP_IE==1){//capture of raw IR frequency
		if (modFreqCnt<6){
			//we capture on rising edge, 
			//this will return 3 samples
			//subtract 2nd from 1st, 3rd from 2nd to get actual time
			modFreq[modFreqCnt]=IRFREQ_CAP_H;
			modFreqCnt++;
			modFreq[modFreqCnt]=IRFREQ_CAP_L;
			modFreqCnt++;

			IRFREQ_CAP_IF=0;

			if(modFreqCnt==6){
				IRFREQ_CAP_IE=0;
				IRFREQ_CCPxCON=0;
			}

		}
	}else if(TM0IE==1 && TM0IF==1){ //is this timer 0 interrupt?
		//the idea is that if we got here
		//it has been so long without a pin change that 
		//there is not more signal
		//it would be more robust to check the pin state for 0
		//need to examine the limits of typical protocols closer

		T1ON=0;		//t1 is the usb packet timeout, disable it
		T3CON&=(~0b1); //turn it off
		TM0ON=0; //timer0 off
		TM0IF=0; //clear interrupt flag

		if(irS.TX==1){//timer0 interrupt means the IR transmit period is over
			TM0IE=0;

			//!!!!if txflag is 0 then raise error flag!!!!!!!
			if(irS.txflag==0 || irS.TXend==1){
				//disable the PWM, output ground
				PWMoff(); 
				LedOff();
				irS.TX=0;
				//reset receive interrupt
				IRRX_IF=0;
				IRRX_IE=1;
				IRRX_IF=0;
				return;
			}

			if(irS.TXInvert==IRS_TRANSMIT_HI){
				//enable the PWM
				PWMon();
				irS.TXInvert=IRS_TRANSMIT_LO;
			}else{
				//disable the PWM, output ground
				PWMoff(); 
				irS.TXInvert=IRS_TRANSMIT_HI;
			}
			//setup timer
			TMR0H=tmr0_buf[0];//first set the high byte
			TMR0L=tmr0_buf[1];//set low byte copies high byte too
			TM0IF=0;
			TM0IE=1;
			TM0ON=1;//enable the timer
			irS.txflag=0; //buffer ready for new byte

		}else{//receive mode

			if(irS.rxflag==0){//check if data is pending
				//packet terminator, 1.7S with no signal
				h=0xff; //add to USB send buffer
				l=0xff; //add to USB send buffer
				irS.rxflag=1;
				//set the flush flag to send the packet from the main loop
				irS.flushflag=1;
			}else{//error, overflow
				irS.overflow=1;
			}
	
			//reset the pin interrupt, just in case
			IRRX_IE=1;
			IRRX_IF=0;

			LedOff();
		}
	}else if(T1IE==1 && T1IF==1){ //is this timer 1 interrupt?
		//this is another timer
		//it tells the main loop to send any pending USB bytes
		// after a few MS
		//the idea is that the 1.7s delay for the terminaor byte is really long
		//we want to send the accumulated data sooner than that, or response will appear sluggish
		//time1 (adjust as needed) sets teh flush flag and sends any pending data on it's way
		irS.flushflag=1;	
		T1IF=0;		//clear the interrupt flag
	}  
}

//#endif //IRS.c