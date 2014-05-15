/*
 * phone.c
 *
 *  Created on: 03.05.2014
 *      Author: andreasbehnke
 */

#include <avr/interrupt.h>
#include "include/phone.h"
#include "include/uart.h"
#include "include/fbus.h"

#define COMMAND_ACKNOWLEDGE                   0x7F
#define COMMAND_GET_HARDWARE_VERSION          0xD1
#define COMMAND_RECEIVE_HARDWARE_VERSION      0xD2

volatile uint8_t phone_state = PHONE_STATE_OFF;

uint8_t phone_command_transmission;
uint8_t phone_command_expected;

void phone_init() {
    // initialize UART
    uart_async_init(PHONE_UART, PHONE_BAUD, PHONE_IN_BUF_SIZE, PHONE_OUT_BUF_SIZE);
    fbus_init(uart_async_open_stream(PHONE_UART, 0));
	fbus_input_clear();

	// TODO: turn phone on
}

uint8_t _phone_process_state(FILE *debug) {
    uint8_t command = fbus_input_frame.command;
    switch (phone_state) {
    case PHONE_STATE_OFF:
        /*
         Nokia 3310 sends power up commands:

         1E FF 00 D0 00 03 01 01 E0 00 FF 2D  First Command
         1E 14 00 F4 00 01 03 00 1D E1        Second Command
         */
        fbus_input_clear();
        if (command == 0xF4) { // second power on frame received
            // Receiving of these two frames is no indicator for end of power-on-pulse!
            fbus_synchronize();
            fputs("Synchronized serial port by sending 0x55 127 times.\n\r", debug);
            phone_state = PHONE_STATE_READY;
        }
        break;
    case PHONE_STATE_WAIT_FOR_ACK:
        if (command == COMMAND_ACKNOWLEDGE) {
            /*
             Example acknowledge command send by phone:
             1E 0C 00 7F 00 02 D1 00 CF 71
             */
            if (fbus_input_frame.command != phone_command_transmission) {
                fputs("Error: Received acknowledge for unexpected command\n\r", debug);
                fbus_input_clear();
                phone_state = PHONE_STATE_ERROR;
            } else {
                fputs("Received acknowledge\n\r", debug);
                fbus_input_clear();
                phone_state = PHONE_STATE_WAIT_FOR_RESPONSE;
            }
        } else {
            // unexpected phone response
            fprintf(debug, "Error: Expected acknowledge but got %d\n\r", command);
            fbus_input_clear();
            phone_state = PHONE_STATE_ERROR;
        }
        break;
    case PHONE_STATE_WAIT_FOR_RESPONSE:
        if (command == phone_command_expected) {
            fputs("Received response, send acknowledge\n\r", debug);
            // send acknowledge
            uint8_t received_sequence = fbus_input_frame.data[fbus_input_frame.data_size - 1] & 0x0F;
            uint8_t cmd_data[] = {command, received_sequence};
            fbus_send_frame(COMMAND_ACKNOWLEDGE, 2, cmd_data);
            phone_state = PHONE_STATE_RESPONSE_READY;
        } else {
            // unexpected phone response
            fprintf(debug, "Error: Phone sends unexpected response: %d\n\r", command);
            fbus_input_clear();
            phone_state = PHONE_STATE_ERROR;
        }
        break;
    case PHONE_STATE_RESPONSE_READY:
        fprintf(debug, "Received message from phone: %d \n\r", command);
        break;
    default:
        break;
    }
    return phone_state;
}

uint8_t phone_process(FILE *debug) {
    uint8_t fbus_state = fbus_read_frame();
    if (IS_FBUS_ERROR()) {
        fputs("FBUS ERROR", debug);
        fbus_input_clear();
        phone_state = PHONE_STATE_ERROR;
    } else if (IS_FBUS_READY()) {
        fputs("FBUS READY", debug);
        _phone_process_state(debug);
    }
    return phone_state;
}

void phone_send_get_version() {
    uint8_t req[] = {0x00, 0x01, 0x00, 0x03, 0x00, 0x01, 0x60};
    fbus_send_frame(COMMAND_GET_HARDWARE_VERSION, 7, req);
    phone_command_transmission = COMMAND_GET_HARDWARE_VERSION;
    phone_command_expected = COMMAND_RECEIVE_HARDWARE_VERSION;
    phone_state = PHONE_STATE_WAIT_FOR_ACK;
}
