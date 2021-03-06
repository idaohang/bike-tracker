/**
 * Implementation of mobile device API for nokia 6110 based handies.
 * See gnokii project for details: www.gnokii.org, file nk6110.txt
 */
#include <string.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/pgmspace.h>
#include "include/debug.h"
#include "include/mdevice.h"
#include "include/timer.h"
#include "include/uart.h"
#include "include/fbus.h"

#ifdef NK6110

#define COMMAND_SMS_HANDLING                  0x02
#define COMMAND_STATUS                        0x04
#define COMMAND_CODE                          0x08
#define COMMAND_SIM_STATUS                    0x09
#define COMMAND_NETWORK_STATUS                0x0a
#define COMMAND_SMS_FUNCTIONS                 0x14
#define COMMAND_TX_GET_HARDWARE_VERSION       0xd1
#define COMMAND_RC_HARDWARE_VERSION           0xd2

uint8_t mdevice_state = MDEVICE_STATE_OFF;

uint8_t mdevice_tx_command;
uint8_t mdevice_rc_expected_command;

#define MDEVICE_NO_TIMEOUT 0
#define MDEVICE_TIMEOUT    1
uint8_t volatile _mdevice_timeout = MDEVICE_NO_TIMEOUT;

uint8_t const frame_get_status[] PROGMEM =      {FBUS_FRAME_HEADER, 0x01};
uint8_t const frame_get_pin_status[] PROGMEM =  {FBUS_FRAME_HEADER, 0x07, 0x01, 0x01, 0x00};
uint8_t const frame_get_hdw_version[] PROGMEM = {FBUS_FRAME_HEADER, 0x03, 0x00, 0x01, 0x00};
uint8_t const frame_enter_pin[] PROGMEM =       {FBUS_FRAME_HEADER, 0x0a, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
uint8_t const frame_get_smsc[] PROGMEM =        {FBUS_FRAME_HEADER, 0x33, 0x64, 0x01, 0x01, 0x00};
uint8_t const frame_send_sms[] PROGMEM =        {FBUS_FRAME_HEADER, 0x01, 0x02, 0x00};
uint8_t const frame_delete_sms[] PROGMEM =      {FBUS_FRAME_HEADER, 0x0a, 0x00, 0x00, 0x01, 0x00};
uint8_t mdevice_data[256];

MDEVICE_SMS_DATA mdevice_sms;

static void mdevice_timeout_reached(void *data) {
    _mdevice_timeout = MDEVICE_TIMEOUT;
}

static void mdevice_start_timeout() {
    _mdevice_timeout = MDEVICE_NO_TIMEOUT;
    timer_start_timeout(TIMER_MDEVICE_INDEX, mdevice_timeout_reached, NULL, MDEVICE_TIMEOUT_MS);
}

static void mdevice_stop_timeout() {
    timer_stop_timeout(TIMER_MDEVICE_INDEX);
    _mdevice_timeout = MDEVICE_NO_TIMEOUT;
}

void mdevice_init() {
    // initialize UART
    uart_async_init(MDEVICE_UART, MDEVICE_BAUD, MDEVICE_IN_BUF_SIZE, MDEVICE_OUT_BUF_SIZE);
    fbus_init(uart_async_open_stream(MDEVICE_UART, 0));
	fbus_input_clear();
}

static void mdevice_send_acknowledge(uint8_t rc_command) {
    // send acknowledge
    uint8_t received_sequence = fbus_input_frame.data[fbus_input_frame.data_size - 1] & 0x0F;
    uint8_t cmd_data[2];
    cmd_data[0] = rc_command;
    cmd_data[1] = received_sequence;
    fbus_send_frame(FBUS_COMMAND_ACKNOWLEDGE, 2, cmd_data);
}

static uint8_t mdevice_process_state() {
    uint8_t command = fbus_input_frame.command;
    switch (mdevice_state) {
    case MDEVICE_STATE_WAIT_FOR_POWER_ON:
         //Nokia 3310 sends power up commands:
         //1e ff 00 d0 00 03 01 01 e0 00 ff 2d  First Command
         //1e 14 00 f4 00 01 03 00 1d e1        Second Command
        fbus_input_clear();
        if (command == 0xf4) { // second power on frame received
            // Receiving of these two frames is no indicator for end of power-on-pulse!
            mdevice_stop_timeout();
            mdevice_state = MDEVICE_STATE_READY;
        }
        break;
    case MDEVICE_STATE_WAIT_FOR_ACK:
        if (command == FBUS_COMMAND_ACKNOWLEDGE) {
            //Example acknowledge command send by phone:
            //1e 0c 00 7f 00 02 d1 00 cf 71
            if (fbus_input_frame.data[0] != mdevice_tx_command) {
                debug_puts("Error: Received acknowledge for unexpected command\n\r");
                mdevice_state = MDEVICE_STATE_ERROR;
            } else {
                debug_puts("Received acknowledge\n\r");
                fbus_input_clear();
                mdevice_start_timeout();
                mdevice_state = MDEVICE_STATE_WAIT_FOR_RESPONSE;
            }
        } else {
            // unexpected phone response
            debug_printf("Warning: Expected acknowledge but got %#.2x\n\r", command);
            // this might be some status frame, send acknowledge to keep in sync with phone
            mdevice_send_acknowledge(command);
            fbus_input_clear();
            mdevice_start_timeout();
        }
        break;
    case MDEVICE_STATE_WAIT_FOR_RESPONSE:
        if (command == mdevice_rc_expected_command) {
            debug_puts("Received response, send acknowledge\n\r");
            // send acknowledge
            mdevice_send_acknowledge(command);
            mdevice_stop_timeout();
            mdevice_state = MDEVICE_STATE_RESPONSE_READY;
        } else {
            // unexpected phone response
            debug_printf("Warning: Phone sends unexpected response: %#.2x\n\r", command);
            mdevice_send_acknowledge(command);
            fbus_input_clear();
            mdevice_start_timeout();
        }
        break;
    case MDEVICE_STATE_RESPONSE_READY:
        mdevice_stop_timeout();
        debug_printf("Received message from phone: %#.2x\n\r", command);
        break;
    default:
        break;
    }
    return mdevice_state;
}

uint8_t mdevice_process() {
    uint8_t fbus_state = fbus_read_frame();
    if (IS_FBUS_ERROR()) {
        mdevice_stop_timeout();
        fbus_debug_dump_input();
        fbus_input_clear();
        mdevice_state = MDEVICE_STATE_ERROR;
    } else if (IS_FBUS_READY()) {
        mdevice_process_state();
    }
    if (_mdevice_timeout == MDEVICE_TIMEOUT) {
        debug_puts("MDEVICE: Timeout\n\r");
        fbus_input_clear();
        mdevice_state = MDEVICE_STATE_ERROR;
    }
    return mdevice_state;
}

static void mdevice_send_frame(uint8_t command, uint8_t expected_command, uint16_t command_length, uint8_t *command_data) {
    fbus_input_clear();
    fbus_send_frame(command, command_length, command_data);
    mdevice_tx_command = command;
    mdevice_rc_expected_command = expected_command;
    mdevice_state = MDEVICE_STATE_WAIT_FOR_ACK;
    mdevice_start_timeout();
}

static void mdevice_wait_for_command(uint8_t expected_command) {
    fbus_input_clear();
    mdevice_rc_expected_command = expected_command;
    mdevice_state = MDEVICE_STATE_WAIT_FOR_RESPONSE;
    mdevice_start_timeout();
}

void mdevice_power_on() {
    mdevice_state = MDEVICE_STATE_WAIT_FOR_POWER_ON;
    mdevice_start_timeout();
}

void mdevice_tx_get_status() {
    memcpy_P(mdevice_data, frame_get_status, 4);
    mdevice_send_frame(COMMAND_STATUS, COMMAND_STATUS, 4, mdevice_data);
}

uint8_t mdevice_get_status() {
    return fbus_input_frame.data[2];
}

void mdevice_tx_get_hdw_version() {
    memcpy_P(mdevice_data, frame_get_hdw_version, 7);
    mdevice_send_frame(COMMAND_TX_GET_HARDWARE_VERSION, COMMAND_RC_HARDWARE_VERSION, 7, mdevice_data);
}

uint8_t *mdevice_get_hdw_version() {
    return fbus_input_frame.data + 4;
}

void mdevice_rc_wait_for_network_status() {
    mdevice_wait_for_command(COMMAND_NETWORK_STATUS);
}

void mdevice_tx_get_pin_status() {
    memcpy_P(mdevice_data, frame_get_pin_status, 7);
    mdevice_send_frame(COMMAND_CODE, COMMAND_CODE, 7, mdevice_data);
}

void mdevice_tx_enter_pin(uint8_t pin[4]) {
    //1e 00 0c 08 00 0d    00 01 00 0a 02 31 32 33 34 00 00 - 01 - 46 - 00 - 50 0d
    memcpy_P(mdevice_data, frame_enter_pin, 13);
    mdevice_data[5] = pin[0];
    mdevice_data[6] = pin[1];
    mdevice_data[7] = pin[2];
    mdevice_data[8] = pin[3];
    mdevice_send_frame(COMMAND_CODE, COMMAND_CODE, 13, mdevice_data);
}

uint8_t mdevice_get_pin_status() {
    switch (fbus_input_frame.data[3]) {
    case 0x05:
        return MDEVICE_PIN_CHANGE_OK;
    case 0x06:
    case 0x09:
    case 0x0c:
        switch (fbus_input_frame.data[4]) {
        case 0x6f:
        case 0x79:
            return MDEVICE_PIN_SIM_CARD_NOT_READY;
        case 0x88: // or: code not needed?
        case 0x8d:
            return MDEVICE_PIN_WRONG_PIN_CODE;
        }
        break;
    case 0x08:
        switch (fbus_input_frame.data[4]) {
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
            return MDEVICE_PIN_WAIT_FOR;
        }
        break;
    case 0x0b:
        return MDEVICE_PIN_ACCEPTED;
    }
    return MDEVICE_PIN_UNKNOWN;
}

void mdevice_rc_wait_for_sim_login() {
    mdevice_wait_for_command(COMMAND_SIM_STATUS);
}


void mdevice_tx_get_smsc() {
    //1e 00 0c 02 00 08    00 01 00 33 64 01 - 01 - 46 - 77 7f
    memcpy_P(mdevice_data, frame_get_smsc, 8);
    mdevice_send_frame(COMMAND_SMS_HANDLING, COMMAND_SMS_HANDLING, 8, mdevice_data);
}

void mdevice_get_smsc() {
    //                      0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22
    // 1e 0c 00 02 00 28   01 08 00 34 01 ed 00 00 a8 00 00 00 00 00 00 00 00 00 00 00 00 07 91 94 71 01
    // 67 00 00 00 00 00 00 53 4d 53 43 00 - 01 40 - 3e 25
    memcpy(mdevice_sms.smsc_octet, fbus_input_frame.data + 21, 12);
}

void mdevice_tx_send_sms() {
    //                    0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32
    // 1e 00 0c 02 00 40 00 01 00 01 02 00 07 91 94 71 01 67 00 00 00 00 00 00 11 00 00 00 16 0c 91 94 61 23 96 34 34 00 00
    //
    // 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65
    // 00 00 a9 00 00 00 00 00 00 54 74 7a 0e 4a cf 41 61 10 bd 3c a7 83 da e5 f9 3c 7c 2e 03 01 40 8b 31
    memcpy_P(mdevice_data, frame_send_sms, 6);
    uint8_t pos = 6;
    memcpy(mdevice_data + pos, mdevice_sms.smsc_octet, 12);
    pos = 18;
    mdevice_data[pos++] = 0x11; // flags
    mdevice_data[pos++] = 0x00;
    mdevice_data[pos++] = 0x00; // pid - protocol identifier
    mdevice_data[pos++] = 0x00; // dcs - data coding scheme
    mdevice_data[pos++] = mdevice_sms.message_length;
    memcpy(mdevice_data + pos, mdevice_sms.remote_number_octet, 12);
    pos += 12;
    mdevice_data[pos++] = 0xa9; // validity-period code
    memset(mdevice_data + pos, 0x00, 6); // service center time stamp for SMS-Deliver
    pos += 6;
    memcpy(mdevice_data + pos, mdevice_sms.encoded_message, mdevice_sms.encoded_message_length); // message
    pos += mdevice_sms.encoded_message_length;
    mdevice_data[pos++] = 0x01;
    mdevice_data[pos++] = 0x00;
    mdevice_send_frame(COMMAND_SMS_HANDLING, COMMAND_SMS_HANDLING, pos, mdevice_data);
}

uint8_t mdevice_get_sms_send_status() {
    if (fbus_input_frame.data[3] == 0x02) {
        return MDEVICE_SMS_SEND_OK;
    }
    return MDEVICE_SMS_SEND_ERROR;
}

void mdevice_rc_wait_for_sms() {
    mdevice_wait_for_command(COMMAND_SMS_HANDLING);
}

void mdevice_get_sms() {
    //   0    1    2    3    4    5    6    7    8    9
    // 0 0x01 0x08 0x00 0x10 0x02 0x0b 0x00 0x07 0x91 0x94
    // 1 0x71 0x01 0x67 0x00 0x00 0x00 0x10 0x19 0x20 0x04
    // 2 0x00 0x00 0x0c 0x0c 0x91 0x94 0x61 0x23 0x96 0x34
    // 3 0x34 0x00 0x70 0x52 0x02 0x41 0x60 0x10 0x51 0x30
    // 4 0x75 0x80 0xc8 0x32 0x9b 0xfd 0x06 0x5d 0xdf 0x72
    // 5 0x36 0x39 0x04 0x01 0x43
    mdevice_sms.memory_type = fbus_input_frame.data[4];
    mdevice_sms.memory_location = fbus_input_frame.data[5];
    mdevice_sms.message_length = fbus_input_frame.data[22];
    memcpy(mdevice_sms.remote_number_octet, fbus_input_frame.data + 22, 12);
    uint8_t encoded_message_length = fbus_input_frame.data_size - 44;
    memcpy(mdevice_sms.encoded_message, fbus_input_frame.data + 42, encoded_message_length);
    mdevice_sms.encoded_message_length = encoded_message_length;
}

void mdevice_tx_delete_sms() {
    //Byte: 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15
    //Data: 1E 00 0C 14 00 08 00 01 00 0A 02 02 01 41 11 54
    memcpy_P(mdevice_data, frame_delete_sms, 8);
    mdevice_data[4] = mdevice_sms.memory_type;
    mdevice_data[5] = mdevice_sms.memory_location;
    mdevice_send_frame(COMMAND_SMS_FUNCTIONS, COMMAND_SMS_FUNCTIONS, 8, mdevice_data);
}

#endif
