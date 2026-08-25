// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2025 RVX Project Contributors

#include "libsteel.h"


// ============================================================
// RISC-V STEEL peripherals
// ============================================================

#define DEFAULT_UART ((UartController *)0x80000000)
#define DEFAULT_GPIO ((GpioController *)0x80020000)


// ============================================================
// GPIO assignment
//
// GPIO[0] -> HC-SR04 TRIG
// GPIO[1] -> HC-SR04 ECHO
// GPIO[2] -> LED
// ============================================================

#define TRIG_PIN 0
#define ECHO_PIN 1
#define LED_PIN  2


// ============================================================
// Machine Timer
//
// CPU clock = 50 MHz
//
// 1 clock = 20 ns
// 50 clocks = 1 us
// ============================================================

#define MTIMER_BASE 0x80010000UL

#define MTIMER_CR \
    (*(volatile unsigned int *)(MTIMER_BASE + 0x00))

#define MTIMER_LO \
    (*(volatile unsigned int *)(MTIMER_BASE + 0x04))


// ============================================================
// HC-SR04 timing constants
// ============================================================

// 50 MHz clock
#define TICKS_PER_US 50U

// HC-SR04:
//
// Distance in cm = Echo time / 58
//
// 58 us * 50 clocks/us = 2900 clocks/cm
//
// 2900 is already calculated.
// The CPU does NOT perform multiplication here.

#define TICKS_PER_CM 2900U

// Maximum echo wait time = 30 ms
//
// 30 ms = 30,000 us
//
// 30,000 * 50 = 1,500,000
//
// Again, this is a pre-calculated constant.

#define ECHO_TIMEOUT_TICKS 1500000U


// ============================================================
// Timer read
// ============================================================

static unsigned int timer_read(void)
{
    return MTIMER_LO;
}


// ============================================================
// Delay approximately one microsecond
//
// No multiplication or division.
//
// The caller specifies the number of microseconds.
// ============================================================

static void delay_us(unsigned int us)
{
    unsigned int start;
    unsigned int count;

    start = timer_read();
    count = 0;

    while (count < us)
    {
        while ((unsigned int)(timer_read() - start)
               < TICKS_PER_US)
        {
            __asm__ volatile ("nop");
        }

        start = timer_read();
        count++;
    }
}


// ============================================================
// HC-SR04 trigger
//
// TRIG HIGH for at least 10 us.
// ============================================================

static void ultrasonic_trigger(void)
{
    // Make sure TRIG is LOW
    gpio_clear(DEFAULT_GPIO, TRIG_PIN);

    // Small settling delay
    delay_us(2);

    // 10 us HIGH pulse
    gpio_set(DEFAULT_GPIO, TRIG_PIN);

    delay_us(10);

    // Return TRIG LOW
    gpio_clear(DEFAULT_GPIO, TRIG_PIN);
}


// ============================================================
// Wait for ECHO to become HIGH
//
// Return:
//   1 = ECHO detected
//   0 = timeout
// ============================================================

static unsigned int wait_echo_high(void)
{
    unsigned int start;
    unsigned int elapsed;

    start = timer_read();

    while (gpio_read(DEFAULT_GPIO, ECHO_PIN) == 0)
    {
        elapsed = timer_read() - start;

        if (elapsed > ECHO_TIMEOUT_TICKS)
        {
            return 0;
        }
    }

    return 1;
}


// ============================================================
// Measure ECHO pulse width
//
// Return value:
//     Echo pulse width in timer ticks
//
// No:
//     multiplication
//     division
//     modulo
//     64-bit arithmetic
// ============================================================

static unsigned int ultrasonic_echo_ticks(void)
{
    unsigned int start;
    unsigned int elapsed;


    // --------------------------------------------------------
    // Wait until ECHO becomes HIGH
    // --------------------------------------------------------

    if (wait_echo_high() == 0)
    {
        return 0;
    }


    // --------------------------------------------------------
    // Start timer when ECHO becomes HIGH
    // --------------------------------------------------------

    start = timer_read();


    // --------------------------------------------------------
    // Wait until ECHO becomes LOW
    // --------------------------------------------------------

    while (gpio_read(DEFAULT_GPIO, ECHO_PIN) != 0)
    {
        elapsed = timer_read() - start;

        if (elapsed > ECHO_TIMEOUT_TICKS)
        {
            return 0;
        }
    }


    // --------------------------------------------------------
    // Calculate pulse width
    // --------------------------------------------------------

    elapsed = timer_read() - start;

    return elapsed;
}


// ============================================================
// Convert timer ticks to distance in cm
//
// Normally:
//
//     distance = ticks / 2900
//
// But RV32 STEEL has no M extension and the runtime
// does not provide the required division routine.
//
// Therefore:
//
//     subtract 2900 repeatedly.
//
// Example:
//
//     8700 ticks
//
//     8700 - 2900 = 5800
//     5800 - 2900 = 2900
//     2900 - 2900 = 0
//
// Result = 3 cm
// ============================================================

static unsigned int ultrasonic_distance_cm(
    unsigned int ticks)
{
    unsigned int distance;

    distance = 0;

    while (ticks >= TICKS_PER_CM)
    {
        ticks -= TICKS_PER_CM;
        distance++;
    }

    return distance;
}


// ============================================================
// UART integer output
//
// IMPORTANT:
//
// Do NOT use:
//
//     value / 10
//     value % 10
//
// because these can generate:
//
//     __udivsi3
//     __umodsi3
//
// This implementation uses subtraction only.
// ============================================================

static void uart_write_uint(unsigned int value)
{
    unsigned int digit;
    unsigned int started;


    started = 0;


    // --------------------------------------------------------
    // Ten-thousands
    // --------------------------------------------------------

    digit = 0;

    while (value >= 10000U)
    {
        value -= 10000U;
        digit++;
    }

    if (digit != 0)
    {
        uart_write(
            DEFAULT_UART,
            (char)('0' + digit)
        );

        started = 1;
    }


    // --------------------------------------------------------
    // Thousands
    // --------------------------------------------------------

    digit = 0;

    while (value >= 1000U)
    {
        value -= 1000U;
        digit++;
    }

    if (digit != 0 || started)
    {
        uart_write(
            DEFAULT_UART,
            (char)('0' + digit)
        );

        started = 1;
    }


    // --------------------------------------------------------
    // Hundreds
    // --------------------------------------------------------

    digit = 0;

    while (value >= 100U)
    {
        value -= 100U;
        digit++;
    }

    if (digit != 0 || started)
    {
        uart_write(
            DEFAULT_UART,
            (char)('0' + digit)
        );

        started = 1;
    }


    // --------------------------------------------------------
    // Tens
    // --------------------------------------------------------

    digit = 0;

    while (value >= 10U)
    {
        value -= 10U;
        digit++;
    }

    if (digit != 0 || started)
    {
        uart_write(
            DEFAULT_UART,
            (char)('0' + digit)
        );

        started = 1;
    }


    // --------------------------------------------------------
    // Ones
    // --------------------------------------------------------

    uart_write(
        DEFAULT_UART,
        (char)('0' + value)
    );
}


// ============================================================
// Main
// ============================================================

void main(void)
{
    unsigned int echo_ticks;
    unsigned int distance;


    // ========================================================
    // Enable machine timer
    // ========================================================

    MTIMER_CR = 1;


    // ========================================================
    // Configure GPIO
    // ========================================================

    // GPIO[0] = TRIG = OUTPUT
    gpio_set_output(
        DEFAULT_GPIO,
        TRIG_PIN
    );

    // GPIO[1] = ECHO = INPUT
    gpio_set_input(
        DEFAULT_GPIO,
        ECHO_PIN
    );

    // GPIO[2] = LED = OUTPUT
    gpio_set_output(
        DEFAULT_GPIO,
        LED_PIN
    );


    // ========================================================
    // Initial GPIO state
    // ========================================================

    gpio_clear(
        DEFAULT_GPIO,
        TRIG_PIN
    );

    gpio_clear(
        DEFAULT_GPIO,
        LED_PIN
    );


    // ========================================================
    // UART startup message
    // ========================================================

    uart_write_string(
        DEFAULT_UART,
        "\n\r"
        "================================\n\r"
        " RISC-V STEEL HC-SR04 Demo\n\r"
        "================================\n\r"
    );

    uart_write_string(
        DEFAULT_UART,
        "CPU      : RV32\n\r"
    );

    uart_write_string(
        DEFAULT_UART,
        "M EXT    : Disabled\n\r"
    );

    uart_write_string(
        DEFAULT_UART,
        "CLOCK    : 50 MHz\n\r"
    );

    uart_write_string(
        DEFAULT_UART,
        "TRIG     : GPIO[0]\n\r"
    );

    uart_write_string(
        DEFAULT_UART,
        "ECHO     : GPIO[1]\n\r"
    );

    uart_write_string(
        DEFAULT_UART,
        "LED      : GPIO[2]\n\r\n"
    );


    // ========================================================
    // Main measurement loop
    // ========================================================

    while (1)
    {
        // ----------------------------------------------------
        // Generate trigger pulse
        // ----------------------------------------------------

        ultrasonic_trigger();


        // ----------------------------------------------------
        // Measure echo pulse
        // ----------------------------------------------------

        echo_ticks =
            ultrasonic_echo_ticks();


        // ----------------------------------------------------
        // No echo
        // ----------------------------------------------------

        if (echo_ticks == 0)
        {
            gpio_clear(
                DEFAULT_GPIO,
                LED_PIN
            );

            uart_write_string(
                DEFAULT_UART,
                "No echo detected\n\r"
            );
        }


        // ----------------------------------------------------
        // Echo detected
        // ----------------------------------------------------

        else
        {
            // Convert echo time to cm

            distance =
                ultrasonic_distance_cm(
                    echo_ticks
                );


            // Turn LED ON

            gpio_set(
                DEFAULT_GPIO,
                LED_PIN
            );


            // UART output

            uart_write_string(
                DEFAULT_UART,
                "Distance: "
            );

            uart_write_uint(
                distance
            );

            uart_write_string(
                DEFAULT_UART,
                " cm\n\r"
            );
        }


        // ----------------------------------------------------
        // Wait 60 ms before next measurement
        // ----------------------------------------------------

        delay_us(60000);
    }
}
