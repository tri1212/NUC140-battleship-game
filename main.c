#include <stdio.h>
#include <stdbool.h>
#include "NUC100Series.h"
#include "LCD.h"

#define TIMER0_COUNTS 59999 	// 7-segment scan frequency = 5ms
#define TIMER1_COUNTS 3999999	// LED and Buzzer toggle frequency = 0.33s
#define ROWS 8
#define COLS 8

void System_Config(void);
void UART0_Config(void);
void start_LCD(void);

void displayNums(int, int);
void displayMap(void);
void initShotMap(void);
uint8_t scanKeypad(void);

void TMR0_IRQHandler(void);
void TMR1_IRQHandler(void);
void UART02_IRQHandler(void);
void EINT1_IRQHandler(void);

static volatile uint8_t tmr0_interrupts = 0;
static volatile uint8_t bytes_read = 0;
static volatile uint8_t turns = 0;
static volatile uint8_t blinks = 0;
static volatile uint8_t beeps = 0;

static volatile bool blink_led = false;
static volatile bool beep_buzzer = false;
static volatile bool game_start = false;
static volatile bool next_turn = false;
static volatile bool game_over = false;

static volatile uint8_t map[ROWS][COLS];
static uint8_t shot_map[ROWS][COLS];

enum game_state { HIT, MISS, WIN, LOSE };
enum selection { X, Y };

enum game_state nextTurn(int x, int y);

static volatile enum selection selected = X;
static int selectedX = -1;
static int selectedY = -1;

static uint8_t pattern[] = {
	0b10000010,	// 0
	0b11101110,	// 1
	0b00000111,	// 2
	0b01000110,	// 3
	0b01101010,	// 4
	0b01010010,	// 5
	0b00010010,	// 6
	0b11100110,	// 7
	0b00000010,	// 8
	0b01000010	// 9
};

int main(void)
{
	System_Config();
	UART0_Config();

	start_LCD();
	clear_LCD();
	
	printS(24, 0, "BATTLESHIP");
	printS_5x7(10, 32, "Input map to continue");
	
	// Wait for the map to be sent
	while (bytes_read < 64);

	clear_LCD();
	printS_5x7(6, 24, "Map Loaded Successfully");
	
	// Wait for player to start the game
	while(!game_start);
	
	initShotMap();
	clear_LCD();
	displayMap();
	
	unsigned int count = 0, prevCount = 0;

	while (1) {
		// Get keypad input
		int key = scanKeypad();
		// Debounce
		count++;
		if (key && !game_over && (count - prevCount > 25000)) {
			prevCount = count;	
			// X - Y selection
			if (key == 9) {
				if (selected == X)
					selected = Y;
				else
					selected = X;
			// Coordinate selection
			} else {
				if (selected == X)
					selectedX = key;
				else
					selectedY = key;
			}
		}
		
		if (next_turn) {
			// Get next game state
			enum game_state state = nextTurn(selectedX - 1, selectedY - 1);
			switch (state) {
				case HIT:
					// Blink led and refresh screen				
					clear_LCD();
					displayMap();
					blink_led = true;
					break;
				case MISS:
					// Do nothing
					break;
				case WIN:
					// Display Winning screen					
					clear_LCD();
					printS(28, 8, "Game Over");
					printS(36, 24, "You Win");
					game_over = true;
					beep_buzzer = true;
					break;
				case LOSE:
					// Display Losing screen
					clear_LCD();
					printS(28, 8, "Game Over");
					printS(30, 24, "You Lose");
					game_over = true;
					beep_buzzer = true;
					break;
			}
			next_turn = false;
			selectedX = -1;
			selectedY = -1;
			selected = X;
		}
	}
}

//------------------------------------------------------------------------------------------------------------------------------------
// Function definitions
//------------------------------------------------------------------------------------------------------------------------------------

void System_Config(void) {
	SYS_UnlockReg(); // Unlock protected registers
	CLK->PWRCON |= (0x01 << 0);
	while (!(CLK->CLKSTATUS & (1 << 0)));

	// Clock source selection
	CLK->CLKSEL0 &= ~(0b111 << 0);
	CLK->CLKDIV &= ~(0xF << 0);

	// Enable clock of SPI3
	CLK->APBCLK |= (1 << 15);
	
	// UART0 Clock selection and configuration
	CLK->CLKSEL1 |= (0b11 << 24);	// UART0 clock source is 22.1184 MHz
	CLK->CLKDIV &= ~(0xF << 8); 	// Clock divider is 1
	CLK->APBCLK |= (1 << 16); 		// Enable UART0 clock
	
	// Configure GPIO for 7-segment
	PC->PMD &= ~(0xFF << 8);
	PC->PMD |= (0b01010101 << 8);			// PC4 to PC7: output push-pull
	PE->PMD &= ~(0xFFFF << 0);
	PE->PMD |= (0b0101010101010101 << 0);	// PE0 to PE7: output push-pull
	
	// Configure GPIO for Key Matrix
	PA->PMD |= (0b111111111111 << 0);		// PA0 to PA5: quasi-bidirectional mode
	
	// Configure PB15(button)
	PB->PMD &= ~(0b11 << 30);		// Input mode
	
	PB->DBEN |= (1 << 15);			// Enable debounce
	GPIO->DBNCECON &= ~(1 << 4);	// Select debounce clock source (0:HCLK, 1:LIRC)
	GPIO->DBNCECON &= ~(0xF << 0);
	GPIO->DBNCECON |= (0xC << 0);	// Sample interrupt input every 4096 clocks => 4096/12000 = 0.34s
	
	// Configure PB15 interrupt
	PB->IMD &= ~(1 << 15);			// Enable edge-trigger interrupt
	PB->IEN |= (1 << 31);			// Trigger on rising-edge
	NVIC->ISER[0] |= (1 << 3);		// Enable interrupt for GPIO-B15
	
	// Set PB11(buzzer) to output push-pull
	PB->PMD &= ~(0b11 << 22);
	PB->PMD |= (0b01 << 22);

	// Set PC12(LED) to output push-pull
	PC->PMD &= ~(0b11<< 24);
	PC->PMD |= (0b01 << 24);
	
	// Timer 0 configuration (for 7 segment display)
	CLK->CLKSEL1 &= ~(0b111 << 8);
	CLK->CLKSEL1 |= (0b010 << 8);		// Select clock
	CLK->APBCLK |= (1 << 2);			// Enable timer

	TIMER0->TCSR &= ~(0xFF << 0);		// Pre-scale = 1
	TIMER0->TCSR |= (1 << 26);			// Reset timer
	
	NVIC->ISER[0] |= (1 << 8);
	TIMER0->TCSR |= (1 << 29);			// Enable Timer 0 interrupt
	
	// Define Timer 0 operation mode
	TIMER0->TCSR &= ~(0b11 << 27);
	TIMER0->TCSR |= (0b01 << 27);		// Counting mode: periodic
	TIMER0->TCSR &= ~(1 << 24);			// Timer event counter mode: disabled
	TIMER0->TCSR |= (1 << 16);			// Update TDR while the timer is running

	TIMER0->TCMPR = TIMER0_COUNTS;		// Set timer count
	
	// Timer 1 configuration (for LED and buzzer)
	CLK->CLKSEL1 &= ~(0b111 << 12);
	CLK->CLKSEL1 |= (0b010 << 12);		// Select clock
	CLK->APBCLK |= (1 << 3);			// Enable timer

	TIMER1->TCSR &= ~(0xFF << 0);		// Pre-scale = 1
	TIMER1->TCSR |= (1 << 26);			// Reset timer
	
	NVIC->ISER[0] |= (1 << 9);
	TIMER1->TCSR |= (1 << 29);			// Enable Timer 1 interrupt
	
	// Define Timer 1 operation mode
	TIMER1->TCSR &= ~(0b11 << 27);
	TIMER1->TCSR |= (0b01 << 27);		// Counting mode: periodic
	TIMER1->TCSR &= ~(1 << 24);			// Timer event counter mode: disabled
	TIMER1->TCSR |= (1 << 16);			// Update TDR while the timer is running

	TIMER1->TCMPR = TIMER1_COUNTS;		// Set timer count
	
	TIMER0->TCSR |= (1 << 30);			// Start timer 0 counting
	TIMER1->TCSR |= (1 << 30);			// Start timer 1 counting

	SYS_LockReg();  // Lock protected registers
}

void UART0_Config(void) {
	PB->PMD &= ~(0b11 << 0);		// Set Pin Mode for GPB.0(RX - Input)
	SYS->GPB_MFP |= (1 << 0);		// GPB_MFP[0] = 1 -> PB.0 is UART0 RX pin	

	// UART0 operation configuration
	UART0->LCR |= (0b11 << 0);		// 8 data bits
	UART0->LCR &= ~(1 << 2);		// One stop bit	
	UART0->LCR &= ~(1 << 3);		// No parity bit
	UART0->FCR |= (1 << 1);			// Clear RX FIFO
	UART0->FCR &= ~(0xF << 16);		// FIFO Trigger Level is 1 byte
	
	// UART0 interrupt configuration
	NVIC->ISER[0] |= (1 << 12);		// Enable UART0 IRQ
	NVIC->IP[7] &= ~(0b11 << 6);	// Set highest interrupt priority
	UART0->IER |= (1 << 0);			// Enable UART0 RDA interrupt
	UART0->FCR &= ~(0xF << 4);		// Interrupt trigger level: 1 byte

	//Baud rate config: BRD/A = 1, DIV_X_EN=0
	//--> Mode 0, Baud rate = 115200 bps
	UART0->BAUD &= ~(0b11 << 28); // mode 0	
	UART0->BAUD &= ~(0xFFFF << 0);
	UART0->BAUD |= 10;
}

void start_LCD(void)
{
	SYS->GPD_MFP |= (1 << 11);	// Select PD11 for SPI3 MOSI0 function
	SYS->GPD_MFP |= (1 << 9); 	// Select PD9 for SPI3 SPICLK function
	SYS->GPD_MFP |= (1 << 8); 	// Select PD8 for SPI3 SS30 function
	init_LCD();		// Initialize SPI3 and LCD using the standard configuration
}

uint8_t scanKeypad(void) {
	PA0 = 1; PA1 = 1; PA2 = 0; PA3 = 1; PA4 = 1; PA5 = 1;
	if (PA3 == 0) return 1;
	if (PA4 == 0) return 4;
	if (PA5 == 0) return 7;
	PA0 = 1; PA1 = 0; PA2 = 1; PA3 = 1; PA4 = 1; PA5 = 1;
	if (PA3 == 0) return 2;
	if (PA4 == 0) return 5;
	if (PA5 == 0) return 8;
	PA0 = 0; PA1 = 1; PA2 = 1; PA3 = 1; PA4 = 1; PA5 = 1;
	if (PA3 == 0) return 3;
	if (PA4 == 0) return 6;
	if (PA5 == 0) return 9;
	return 0;
}

// Reset the shot map by setting all values to 0
void initShotMap() {
	for(int i = 0; i < ROWS; i++) {
		for(int j = 0; j < COLS; j++) {
			shot_map[i][j] = 0;
		}
	}
}

// Display the shot map. Hits are 'X' and other cells are '-'
void displayMap(void) {
	for (int row = 0; row < ROWS; row++) {
		for (int col = 0; col < COLS; col++) {
			char temp = (shot_map[row][col] && map[row][col]) ? 'X' : '-';
			printC_5x7(col * 8 + 32, row * 8, temp);
		}
	}
}

// Display 2 numbers on the 7 segment display
// The first number is displayed on the first digit,
// and the second number is displayed on digits 3 and 4
void displayNums(int num1, int num2) {
	PC->DOUT &= ~(0xF << 4);	// All digits off

	if (tmr0_interrupts == 1) {	// Digit 3
		PC->DOUT |= (1 << 5);
		PE->DOUT = pattern[num2 / 10];
	} else if (tmr0_interrupts == 2) {	// Digit 4
		PC->DOUT |= (1 << 4);
		PE->DOUT = pattern[num2 % 10];		
	} else {												// Digit 1
		PC->DOUT |= (1 << 7);
		PE->DOUT = pattern[num1 == -1 ? 0 : num1];
		if (selected == Y)
			PE->DOUT &= ~(1 << 1);
		tmr0_interrupts = 0;
	}
}

// Advance the game state and return MISS, HIT, WIN or LOSE
// based on the shot coordinates
enum game_state nextTurn(int x, int y) {
	turns++;
	if (turns > 16) 
		return LOSE;
	
	if (!map[y][x] || (map[y][x] && shot_map[y][x])) 
		return MISS;
	
	shot_map[y][x] = 1;
	for (int row = 0; row < ROWS; row++) {
		for (int col = 0; col < COLS; col++) {
			if (map[row][col] && !shot_map[row][col]) {
				return HIT;
			}
		}
	}
	return WIN;
}

// UART0 interrupt function for reading map data
void UART02_IRQHandler(void) {
	char byte = UART0->DATA;
	if (byte == '0' || byte == '1') {
		map[bytes_read / ROWS][bytes_read % COLS] = byte - '0';
		bytes_read++;
	}
}

// Timer 0 interrupt function for 7 segment scanning
void TMR0_IRQHandler(void) {
	// Display the selected coordinate and turn number
	if (game_start && !game_over)
		displayNums(selected == X ? selectedX : selectedY, turns);

	tmr0_interrupts++;
	
	TIMER0->TISR |= (1 << 0);
}

// Timer 1 interrupt function for LED and buzzer
void TMR1_IRQHandler(void) {
	if (blink_led) {
		PC->DOUT ^= (1 << 12);
		blinks++;
		if (blinks >= 6) {
			PC->DOUT |= (1 << 12);
			blink_led = false;
			blinks = 0;
		}
	}
	if (beep_buzzer) {
		PB->DOUT ^= (1 << 11);
		beeps++;
		if (beeps >= 10) {
			PB->DOUT |= (1 << 11);
			beep_buzzer = false;
			beeps = 0;
		}
	}
	TIMER1->TISR |= (1 << 0);
}

// GPIO-B15 interrupt function
void EINT1_IRQHandler(void) {
	if (bytes_read >= 64) {
		game_start = true;
	}
	if (game_start) {
		// When both X and Y have been selected,
		// move on to the next turn
		if (selectedX != -1 && selectedY != -1)
			next_turn = true;
		
		// Reset game
		if (game_over) {
			game_over = false;
			turns = 0;
			initShotMap();
			clear_LCD();
			displayMap();
		}
	}
	PB->ISRC |= (1 << 15);
}
