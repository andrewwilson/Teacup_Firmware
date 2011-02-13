#include	"intercom.h"

#include	<avr/io.h>
#include	<avr/interrupt.h>

#include	"config.h"
#include	"delay.h"

#if	 (defined TEMP_INTERCOM) || (defined EXTRUDER)
#define		INTERCOM_BAUD			57600

#define	START	0x55

enum {
	ERROR_BAD_CRC
} err_codes;

typedef struct {
	uint8_t		start;
	union {
		struct {
			uint8_t		dio0		:1;
			uint8_t		dio1		:1;
			uint8_t		dio2		:1;
			uint8_t		dio3		:1;
			uint8_t		dio4		:1;
			uint8_t		dio5		:1;
			uint8_t		dio6		:1;
			uint8_t		dio7		:1;
		};
		uint8_t		dio;
	};
	uint8_t		controller_num;
	uint16_t	temp[3];
	uint8_t		err;
	uint8_t		crc;
} intercom_packet_t;

typedef union {
	intercom_packet_t packet;
	uint8_t						data[sizeof(intercom_packet_t)];
} intercom_packet;

intercom_packet tx;
intercom_packet rx;

uint8_t packet_pointer;
uint8_t	rxcrc;

volatile uint8_t	intercom_flags;

void intercom_init(void)
{
#ifdef HOST
	#if INTERCOM_BAUD > 38401
		UCSR1A = MASK(U2X1);
		UBRR1 = (((F_CPU / 8) / INTERCOM_BAUD) - 0.5);
	#else
		UCSR1A = 0;
		UBRR1 = (((F_CPU / 16) / INTERCOM_BAUD) - 0.5);
	#endif
	UCSR1B = MASK(RXEN1) | MASK(TXEN1);
	UCSR1C = MASK(UCSZ11) | MASK(UCSZ10);

	UCSR1B |= MASK(RXCIE1) | MASK(TXCIE1);
#else
	#if INTERCOM_BAUD > 38401
		UCSR0A = MASK(U2X0);
		UBRR0 = (((F_CPU / 8) / INTERCOM_BAUD) - 0.5);
	#else
		UCSR0A = 0;
		UBRR0 = (((F_CPU / 16) / INTERCOM_BAUD) - 0.5);
	#endif
	UCSR0B = MASK(RXEN0) | MASK(TXEN0);
	UCSR0C = MASK(UCSZ01) | MASK(UCSZ00);

	UCSR0B |= MASK(RXCIE0) | MASK(TXCIE0);
#endif

	intercom_flags = 0;
}

void send_temperature(uint8_t index, uint16_t temperature) {
	tx.packet.temp[index] = temperature;
}

uint16_t read_temperature(uint8_t index) {
	return rx.packet.temp[index];
}

#ifdef HOST
void set_dio(uint8_t index, uint8_t value) {
	if (value)
		tx.packet.dio |= (1 << index);
	else
		tx.packet.dio &= ~(1 << index);
}
#else
uint8_t	get_dio(uint8_t index) {
	return rx.packet.dio & (1 << index);
}
#endif

void set_err(uint8_t err) {
	tx.packet.err = err;
}

uint8_t get_err() {
	return rx.packet.err;
}

void start_send(void) {
	uint8_t txcrc = 0, i;

	// atomically update flags
	uint8_t sreg = SREG;
	cli();
	intercom_flags = (intercom_flags & ~FLAG_TX_FINISHED) | FLAG_TX_IN_PROGRESS;
	SREG = sreg;

	// set start byte
	tx.packet.start = START;

	// calculate CRC for outgoing packet
	for (i = 0; i < (sizeof(intercom_packet_t) - 1); i++) {
		txcrc ^= tx.data[i];
	}
	tx.packet.crc = txcrc;

	// enable transmit pin
	enable_transmit();
	delay_us(15);

	// actually start sending the packet
	packet_pointer = 0;
#ifdef HOST
	UCSR1B |= MASK(UDRIE1);
#else
	UCSR0B |= MASK(UDRIE0);
#endif
}

/*
	Interrupts, UART 0 for mendel
*/

// receive data interrupt- stuff into rx
#ifdef HOST
ISR(USART1_RX_vect)
#else
ISR(USART_RX_vect)
#endif
{
	// pull character
	static uint8_t c;

	#ifdef HOST
		c = UDR1;
		UCSR1A &= ~MASK(FE1) & ~MASK(DOR1) & ~MASK(UPE1);
	#else
		c = UDR0;
		UCSR0A &= ~MASK(FE0) & ~MASK(DOR0) & ~MASK(UPE0);
	#endif

	// are we waiting for a start byte? is this one?
	if ((packet_pointer == 0) && (c == START)) {
		rxcrc = rx.packet.start = START;
		packet_pointer = 1;
		intercom_flags |= FLAG_RX_IN_PROGRESS;
	}

	// we're receiving a packet
	if (packet_pointer > 0) {
		// calculate CRC (except CRC character!)
		if (packet_pointer < (sizeof(intercom_packet_t) - 1))
			rxcrc ^= c;
		// stuff byte into structure
		rx.data[packet_pointer++] = c;
		// last byte?
		if (packet_pointer >= sizeof(intercom_packet_t)) {
			// reset pointer
			packet_pointer = 0;

			intercom_flags = (intercom_flags & ~FLAG_RX_IN_PROGRESS) | FLAG_NEW_RX;
			#ifndef HOST
				if (rx.packet.controller_num == THIS_CONTROLLER_NUM) {
					if (rxcrc != rx.packet.crc)
						tx.packet.err = ERROR_BAD_CRC;
					start_send();
				}
			#endif
		}
	}
}

// finished transmitting interrupt- only enabled at end of packet
#ifdef HOST
ISR(USART1_TX_vect)
#else
ISR(USART_TX_vect)
#endif
{
	if (packet_pointer >= sizeof(intercom_packet_t)) {
		disable_transmit();
		packet_pointer = 0;
		intercom_flags = (intercom_flags & ~FLAG_TX_IN_PROGRESS) | FLAG_TX_FINISHED;
		#ifdef HOST
			UCSR1B &= ~MASK(TXCIE1);
		#else
			UCSR0B &= ~MASK(TXCIE0);
		#endif
	}
}

// tx queue empty interrupt- send next byte
#ifdef HOST
ISR(USART1_UDRE_vect)
#else
ISR(USART_UDRE_vect)
#endif
{
	#ifdef	HOST
	UDR1 = tx.data[packet_pointer++];
	#else
	UDR0 = tx.data[packet_pointer++];
	#endif

	if (packet_pointer >= sizeof(intercom_packet_t)) {
		#ifdef HOST
			UCSR1B &= ~MASK(UDRIE1);
			UCSR1B |= MASK(TXCIE1);
		#else
			UCSR0B &= ~MASK(UDRIE0);
			UCSR0B |= MASK(TXCIE0);
		#endif
	}
}

#endif	/* TEMP_INTERCOM */
