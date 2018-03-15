/*
 * nmuel_project.c
 *
 * Created: 3/5/2018 1:49:33 PM
 *  Author: Nathanael Mueller 861237712
 */ 


#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdlib.h>
#include <avr/eeprom.h>
#include "io.c"

//globals
double note = 0.0;
enum recording {unarmed, armed, in_progress, playback} rec_state;
unsigned short rec_tick = 0;
signed short x = 0;  // Value of ADC register now stored in variable x.
enum states {start, wait, play} state;
signed short prev_x = 0;
unsigned char viz_tick = 0; // used to track the vizualizer rise and fall
unsigned char button = 0; // used to track when the button changes
unsigned char rec_flag = 0;
//timer shit
volatile unsigned char TimerFlag = 0; // TimerISR() sets this to 1. C programmer should clear to 0.

// Internal variables for mapping AVR's ISR to our cleaner TimerISR model.
unsigned long _avr_timer_M = 1; // Start count from here, down to 0. Default 1 ms.
unsigned long _avr_timer_cntcurr = 0; // Current internal count of 1ms ticks

void TimerOn() {
	// AVR timer/counter controller register TCCR1
	TCCR1B = 0x0B;// bit3 = 0: CTC mode (clear timer on compare)
	// bit2bit1bit0=011: pre-scaler /64
	// 00001011: 0x0B
	// SO, 8 MHz clock or 8,000,000 /64 = 125,000 ticks/s
	// Thus, TCNT1 register will count at 125,000 ticks/s

	// AVR output compare register OCR1A.
	OCR1A = 125;	// Timer interrupt will be generated when TCNT1==OCR1A
	// We want a 1 ms tick. 0.001 s * 125,000 ticks/s = 125
	// So when TCNT1 register equals 125,
	// 1 ms has passed. Thus, we compare to 125.
	// AVR timer interrupt mask register
	TIMSK1 = 0x02; // bit1: OCIE1A -- enables compare match interrupt

	//Initialize avr counter
	TCNT1=0;

	_avr_timer_cntcurr = _avr_timer_M;
	// TimerISR will be called every _avr_timer_cntcurr milliseconds

	//Enable global interrupts
	SREG |= 0x80; // 0x80: 1000000
}

void TimerOff() {
	TCCR1B = 0x00; // bit3bit1bit0=000: timer off
}

void TimerISR() {
	TimerFlag = 1;
}

// In our approach, the C programmer does not touch this ISR, but rather TimerISR()
ISR(TIMER1_COMPA_vect) {
	// CPU automatically calls when TCNT1 == OCR1 (every 1 ms per TimerOn settings)
	_avr_timer_cntcurr--; // Count down to 0 rather than up to TOP
	if (_avr_timer_cntcurr == 0) { // results in a more efficient compare
		TimerISR(); // Call the ISR that the user uses
		_avr_timer_cntcurr = _avr_timer_M;
	}
}

// Set TimerISR() to tick every M ms
void TimerSet(unsigned long M) {
	_avr_timer_M = M;
	_avr_timer_cntcurr = _avr_timer_M;
}

//PWM shit

// 0.954 hz is lowest frequency possible with this function,
// based on settings in PWM_on()
// Passing in 0 as the frequency will stop the speaker from generating sound
void set_PWM(double frequency) {
	static double current_frequency; // Keeps track of the currently set frequency
	// Will only update the registers when the frequency changes, otherwise allows
	// music to play uninterrupted.
	if (frequency != current_frequency) {
		if (!frequency) { TCCR3B &= 0x08; } //stops timer/counter
		else { TCCR3B |= 0x03; } // resumes/continues timer/counter
		
		// prevents OCR3A from overflowing, using prescaler 64
		// 0.954 is smallest frequency that will not result in overflow
		if (frequency < 0.954) { OCR3A = 0xFFFF; }
		
		// prevents OCR0A from underflowing, using prescaler 64					// 31250 is largest frequency that will not result in underflow
		else if (frequency > 31250) { OCR3A = 0x0000; }
		
		// set OCR3A based on desired frequency
		else { OCR3A = (short)(8000000 / (128 * frequency)) - 1; }

		TCNT3 = 0; // resets counter
		current_frequency = frequency; // Updates the current frequency
	}
}

void PWM_on() {
	TCCR3A = (1 << COM3A0);
	// COM3A0: Toggle PB6 on compare match between counter and OCR0A
	TCCR3B = (1 << WGM32) | (1 << CS31) | (1 << CS30);
	// WGM02: When counter (TCNT0) matches OCR0A, reset counter
	// CS01 & CS30: Set a prescaler of 64
	set_PWM(0);
}

void PWM_off() {
	TCCR3A = 0x00;
	TCCR3B = 0x00;
}

//ADC shit
void ADC_init() {
	ADCSRA |= (1 << ADEN) | (1 << ADSC) | (1 << ADATE);
	// ADEN: setting this bit enables analog-to-digital conversion.
	// ADSC: setting this bit starts the first conversion.
	// ADATE: setting this bit enables auto-triggering. Since we are
	//        in Free Running Mode, a new conversion will trigger whenever
	//        the previous conversion completes.
}

//turn recording on and off
//state shit
void tickRecord() {
	unsigned char temp = ~PINB & 0x0F;
	switch(rec_state) {
		case unarmed:
			if (temp == 0x01) {
				//arm recording
				rec_state = armed;
				rec_tick = 0;
			}
			if (temp == 0x02) {
				//playback
				rec_state = playback;
			}
			break;
		case in_progress:
			if (rec_tick < 200) {
				eeprom_write_word((uint16_t *)(rec_tick*18), (uint16_t)(note));
				rec_tick++;
			}
			else {
				rec_tick = 0;
				rec_state = unarmed;
			}
			break;
		case playback:
			if (rec_tick < 200) {
				note = eeprom_read_word((uint16_t *)(rec_tick*18));
				rec_tick++;
			}
			else {
				rec_tick = 0;
				rec_state = unarmed;
			}
		default:
			break;
	}
}

//SM shit
void tickSM(unsigned char display_counter) {
	unsigned char temp = ~PINA & 0xFE;
	switch(state) { //transitions
		case start:
			state = wait;
			break;
		case wait:
			if (temp == 0x00) {
				state = wait;
			}
			else {
				state = play;
			}
			break;
		case play:
			if (temp == 0x00) {
				state = wait;
			}
			else {
				state = play;
			}
			break;
		default:
			state = wait;
		break;
	}//transitions
	
	switch(state) { //actions
		case start:
			break;
		case wait:
			note = 0;
			if (viz_tick > 0 && display_counter == 4) {
				viz_tick--;
			}
			
			button = 0;
			break;
		case play:
			if (rec_state == armed) {
				rec_state = in_progress;
			}
			//play sound
			/*
			if (temp == 0x01) {
				set_PWM(261.63);
			}*/
			if (temp ==  0x02) {
				note = 293.66;
				if (button == 2) {
					if (display_counter == 4) {
						viz_tick++;
					}
				}
				else {
					button = 2;
					viz_tick = 0;
				}
			}
			else if (temp == 0x04) {
				note = 329.63;
				if (button == 3) {
					if (display_counter == 4) {
						viz_tick++;
					}
				}
				else {
					button = 3;
					viz_tick = 0;
				}
			}
			else if (temp == 0x08) {
				note = 349.23;
				if (button == 4) {
					if (display_counter == 4) {
						viz_tick++;
					}
				}
				else {
					button = 4;
					viz_tick = 0;
				}
			}
			else if (temp == 0x10) {
				note = 392.0;
				if (button == 5) {
					if (display_counter == 4) {
						viz_tick++;
					}
				}
				else {
					button = 5;
					viz_tick = 0;
				}
			}
			else if (temp == 0x20) {
				note = 440.0;
				if (button == 6) {
					if (display_counter == 4) {
						viz_tick++;
					}
				}
				else {
					button = 6;
					viz_tick = 0;
				}
			}
			else if (temp == 0x40) {
				note = 493.88;
				if (button == 7) {
					if (display_counter == 4) {
						viz_tick++;
					}
				}
				else {
					button = 7;
					viz_tick = 0;
				}
			}
			else if (temp == 0x80) {
				note = 523.25;
				if (button == 8) {
					if (display_counter == 4) {
						viz_tick++;
					}
				}
				else {
					button = 8;
					viz_tick = 0;
				}
			}
			break;
		default:
			break;
	} //actions
	
	if (viz_tick > 4) {
		viz_tick = 4;
	}
}

//pitch bend
void bend_pitch() {
	x = ADC - 500;	//normalize the ADC reading so not touching ~= 519
	if (x > -500 && x < 500) {	//if its within this range it means you're touching it
		if (note != 0) {	//only bend if there is a note playing
			if ((prev_x < (x-15)) || (prev_x > (x+15))) {	//bend smoothener, only update if the reading has changed by 15
				prev_x = x;
				note += (x/2);
			}
			else {	//if less than 15, keep current bend
				note += (prev_x/2);
			}
		}
	}
}

unsigned char low_bar[] = {
	0b00000,
	0b00000,
	0b00000,
	0b00000,
	0b00000,
	0b00000,
	0b11111,
	0b11111
};

unsigned char med_bar[] = {
	0b00000,
	0b00000,
	0b00000,
	0b00000,
	0b11111,
	0b11111,
	0b11111,
	0b11111
};

unsigned char high_bar[] = {
	0b00000,
	0b00000,
	0b11111,
	0b11111,
	0b11111,
	0b11111,
	0b11111,
	0b11111
};

unsigned char top_bar[] = {
	0b11111,
	0b11111,
	0b11111,
	0b11111,
	0b11111,
	0b11111,
	0b11111,
	0b11111
};


//visualize what's playing on the LCD
void update_LCD() {
	switch(viz_tick) {
		case 0:
			LCD_ClearScreen();
			break;
		case 1:
			LCD_Cursor(24);
			LCD_WriteData(0);
			LCD_WriteData(0);
			break;
		case 2:
			LCD_Cursor(22);
			LCD_WriteData(0);
			LCD_WriteData(0);
			LCD_WriteData(1);
			LCD_WriteData(1);
			LCD_WriteData(0);
			LCD_WriteData(0);
			break;
		case 3:
			LCD_Cursor(20);
			LCD_WriteData(0);
			LCD_WriteData(0);
			LCD_WriteData(1);
			LCD_WriteData(1);
			LCD_WriteData(2);
			LCD_WriteData(2);
			LCD_WriteData(1);
			LCD_WriteData(1);
			LCD_WriteData(0);
			LCD_WriteData(0);
			break;
		case 4:
			LCD_Cursor(18);
			LCD_WriteData(0);
			LCD_WriteData(0);
			LCD_WriteData(1);
			LCD_WriteData(1);
			LCD_WriteData(2);
			LCD_WriteData(2);
			LCD_WriteData(3);
			LCD_WriteData(3);
			LCD_WriteData(2);
			LCD_WriteData(2);
			LCD_WriteData(1);
			LCD_WriteData(1);
			LCD_WriteData(0);
			LCD_WriteData(0);
			break;
		default:
			break;
	}
	switch(rec_state) {
		case armed:
			LCD_DisplayString(1,(const unsigned char*)"ready");
			break;
		case in_progress:
			LCD_DisplayString(1,(const unsigned char*)"recording");
			rec_flag = 1;
			break;
		case playback:
			LCD_DisplayString(1,(const unsigned char*)"playback");
			break;
		default:
			if (rec_flag == 1) {
				rec_flag = 0;
				LCD_ClearScreen();
			}
			break;
	}
	
}

//da main
int main(void)
{
	DDRA = 0x00; PORTA = 0xFF; // potentiometer and piano button inputs
	DDRB = 0xF0; PORTB = 0x0F; // PB6 is speaker output, 0-3 are other inputs
	DDRC = 0xFF; PORTC = 0x00; // LCD data lines
	DDRD = 0xFF; PORTD = 0x00; // LCD control lines
	
	PWM_on();
	LCD_init(); 
	LCD_WriteCommand(0x0C);
	ADC_init();  
	TimerSet(20);
	TimerOn();
	
	//write custom chars
	LCD_WriteCommand(0x40);
	for(unsigned char i=0; i<8; i++){
		LCD_WriteData(low_bar[i]);
	}
	LCD_WriteCommand(0x80);
		
	LCD_WriteCommand(0x48);
	for(unsigned char i=0; i<8; i++){
		LCD_WriteData(med_bar[i]);
	}
	LCD_WriteCommand(0x80);
	
	LCD_WriteCommand(0x50);
	for(unsigned char i=0; i<8; i++){
		LCD_WriteData(high_bar[i]);
	}
	LCD_WriteCommand(0x80);
	
	LCD_WriteCommand(0x58);
	for(unsigned char i=0; i<8; i++){
		LCD_WriteData(top_bar[i]);
	}
	LCD_WriteCommand(0x80);

	state = start;
	rec_state = unarmed;
	unsigned char i = 0;
	while(1)
	{
		tickSM(i);
		bend_pitch();
		tickRecord();
		if (i == 4) {	//only update LCD every 200 ms, looks bad otherwise
			update_LCD();
			i = 0;
		}
		else {
			i++;
		}
		set_PWM(note);
		while (!TimerFlag);
		TimerFlag = 0;
	}
}

			/*
			LCD_ClearScreen();
			LCD_Cursor(1);
			char buff[4];
			itoa(x, buff, 10);
			LCD_DisplayString(1,(const unsigned char*)buff);
			*/